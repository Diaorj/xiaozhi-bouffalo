#include "voice_input.h"
#include <algorithm>
#include <cstring>
#include <esp_log.h>
#include "audio/audio_service.h"
#include "audio_dma_worker.h"
#include "memory_diagnostics.h"
#include "opus_encoder_bouffalo.h"

VoiceInputSession::~VoiceInputSession() { Stop(); }
void VoiceInputSession::Fail(const char* reason) {
    const char* empty = nullptr;
    failure_.compare_exchange_strong(empty, reason);
}
bool VoiceInputSession::Prepare() {
    if (!codec_ || audio_.IsRunning() || encoding_owned_)
        return false;
    auto encoder = CreateBouffaloOpusEncoder();
    // Create the contiguous encoder stack before the TLS heap becomes fragmented.
    if (!encoder || !audio_.StartEncoding(std::move(encoder))) {
        Fail("encoder initialization or worker creation failed");
        return false;
    }
    encoding_owned_ = true;
    MemoryDiagnosticsStage("voice_stream_prepared");
    return true;
}
bool VoiceInputSession::StartCapture() {
    if (!encoding_owned_ || capture_pending_ || done_)
        return false;
    if (AudioDmaWorkerStart(CaptureEntry, this) != 0) {
        Fail("capture worker start failed");
        return false;
    }
    capture_pending_ = true;
    return true;
}
void VoiceInputSession::Stop() {
    cancelled_ = true;
    if (capture_pending_) {
        AudioDmaWorkerWait();
        capture_pending_ = false;
    }
    if (encoding_owned_) {
        audio_.Stop();  // Join encoder only after the PCM producer can no longer use it.
        encoding_owned_ = false;
        // Let FreeRTOS idle reclaim its dynamic encoder task stack before playback.
        vTaskDelay(pdMS_TO_TICKS(20));
        MemoryDiagnosticsStage("voice_stream_stopped");
    }
}
void VoiceInputSession::CaptureEntry(void* context) {
    static_cast<VoiceInputSession*>(context)->Capture();
}
void VoiceInputSession::Capture() {
    codec_->EnableInput(true);
    if (!codec_->input_enabled()) {
        Fail("microphone open failed");
        done_ = true;
        return;
    }
    std::vector<int16_t> block(160), frame(kFrameSamples, 0);
    size_t samples = 0, filled = 0, queued = 0;
    uint32_t peak = 0;
    uint64_t sum = 0;
    ESP_LOGI("VOICE", "RECORDING: speak now, 5 seconds; streaming upload");
    recording_started_ = xTaskGetTickCount();
    while (samples < kSamples && !cancelled_) {
        if (audio_.HasEncodingError()) {
            Fail("encoder or send queue backpressure");
            break;
        }
        if (!codec_->InputData(block)) {
            Fail("microphone DMA read failed");
            break;
        }
        for (int16_t sample : block) {
            uint32_t magnitude = sample < 0 ? -int32_t(sample) : sample;
            peak = std::max(peak, magnitude);
            sum += magnitude;
        }
        std::memcpy(frame.data() + filled, block.data(), block.size() * sizeof(int16_t));
        filled += block.size();
        samples += block.size();
        if (samples == kSamples)
            recording_ended_ = xTaskGetTickCount();
        if (filled == kFrameSamples || samples == kSamples) {
            // The final frame was initialized to zero before its partial copy.
            if (!audio_.PushPcmToEncodeQueue(std::move(frame))) {
                Fail("PCM encode queue full or stopped");
                break;
            }
            queued++;
            filled = 0;
            if (samples < kSamples)
                frame.assign(kFrameSamples, 0);
        }
    }
    codec_->EnableInput(false);
    if (!Failure() && !cancelled_ && samples == kSamples && queued == kFrames) {
        ESP_LOGI("VOICE", "VOICE_CAPTURE_DONE: samples=%u peak=%u mean_abs=%u packets=%u",
                 static_cast<unsigned>(samples), peak, static_cast<unsigned>(sum / samples),
                 static_cast<unsigned>(queued));
        if (peak < 100)
            ESP_LOGW("VOICE", "VOICE_WEAK: check microphone/speech against recorded levels");
    } else if (!Failure()) {
        Fail("voice capture cancelled");
    }
    MemoryDiagnosticsStage("voice_capture_worker_done");
    done_ = true;
}
