#include "voice_input.h"
#include <algorithm>
#include <cstring>
#include <esp_log.h>
extern "C" {
#include "audio_vad.h"
}
#include "audio/audio_service.h"
#include "audio_dma_worker.h"
#include "audio_sdk_bridge.h"
#include "memory_diagnostics.h"
#include "opus_encoder_bouffalo.h"

VoiceInputSession::~VoiceInputSession() { Stop(); }
void VoiceInputSession::Fail(const char* reason) {
    const char* empty = nullptr;
    failure_.compare_exchange_strong(empty, reason);
}
bool VoiceInputSession::Prepare() {
    if (!codec_ || audio_.IsRunning() || encoding_owned_ || storage_)
        return false;
    storage_ = static_cast<int16_t*>(AudioSdkAllocateBuffer(
        (kFifoSamples + kPrerollSamples) * sizeof(int16_t)));
    if (!storage_) {
        Fail("voice PSRAM buffer allocation failed");
        return false;
    }
    auto encoder = CreateBouffaloOpusEncoder();
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
        audio_.Stop();
        encoding_owned_ = false;
        MemoryDiagnosticsStage("voice_stream_stopped");
    }
    AudioSdkFreeBuffer(storage_);
    storage_ = nullptr;
}
bool VoiceInputSession::Publish(const int16_t* pcm, size_t count) {
    const uint32_t write = published_.load(std::memory_order_relaxed);
    const uint32_t read = consumed_.load(std::memory_order_acquire);
    if (write - read > kFifoSamples || count > kFifoSamples - (write - read)) {
        Fail("voice PCM backlog full; network/encoder stalled");
        return false;
    }
    const size_t first = std::min<size_t>(count, kFifoSamples - write % kFifoSamples);
    std::memcpy(storage_ + write % kFifoSamples, pcm, first * sizeof(int16_t));
    std::memcpy(storage_, pcm + first, (count - first) * sizeof(int16_t));
    published_.store(write + count, std::memory_order_release);
    return true;
}
bool VoiceInputSession::PopPcmFrame(std::vector<int16_t>& frame) {
    const uint32_t read = consumed_.load(std::memory_order_relaxed);
    // Observe completion before the write index: done publishes the final partial frame.
    const bool ended = done_.load(std::memory_order_acquire);
    const uint32_t write = published_.load(std::memory_order_acquire);
    const size_t available = write - read;
    if (!storage_ || !available || (available < kFrameSamples && !ended))
        return false;
    if (available > kFifoSamples) {
        Fail("voice PCM ring invalid");
        return false;
    }
    const size_t count = std::min(available, kFrameSamples);
    const size_t first = std::min<size_t>(count, kFifoSamples - read % kFifoSamples);
    frame.assign(kFrameSamples, 0);
    std::memcpy(frame.data(), storage_ + read % kFifoSamples, first * sizeof(int16_t));
    std::memcpy(frame.data() + first, storage_, (count - first) * sizeof(int16_t));
    consumed_.store(read + count, std::memory_order_release);
    return true;
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
    std::vector<int16_t> block(AUDIO_VAD_BLOCK);
    int16_t* preroll = storage_ + kFifoSamples;
    size_t head = 0, retained = 0;
    audio_vad_t vad;
    // The capture owner enforces its exact 10-second cap including preroll.
    audio_vad_init_listener(&vad, UINT32_MAX);
    const char* reason = "cancelled";
    ESP_LOGI("VOICE", "VAD_CALIBRATING: stay quiet for 300 ms");
    while (!cancelled_ && !Failure()) {
        if (audio_.HasEncodingError()) {
            Fail("encoder or send queue backpressure");
            break;
        }
        if (!codec_->InputData(block)) {
            Fail("microphone DMA read failed");
            break;
        }
        const auto event = audio_vad_process(&vad, block.data(), block.size());
        if (!voice_started_) {
            for (int16_t sample : block) {
                preroll[head] = sample;
                head = (head + 1) % kPrerollSamples;
                retained = std::min(retained + 1, kPrerollSamples);
            }
            if (event == AUDIO_VAD_READY) {
                ESP_LOGI("VOICE", "AUTO_LISTENING: speak now; stop after 800 ms silence, max 10 s");
                ESP_LOGI("VOICE", "VAD_LEVEL: noise=%u start=%u hold=%u", unsigned(vad.noise),
                         unsigned(vad.start_threshold), unsigned(vad.hold_threshold));
            }
            // Validate >=160 ms of voice before opening a cloud request. A click or
            // short burst returns to listening without sending any audio or listen/start.
            if (vad.valid_voice) {
                recording_started_ = xTaskGetTickCount();
                const size_t oldest = retained == kPrerollSamples ? head : 0;
                const size_t first = std::min(retained, kPrerollSamples - oldest);
                if (!Publish(preroll + oldest, first) || !Publish(preroll, retained - first))
                    break;
                voice_started_ = true;
                ESP_LOGI("VOICE", "VOICE_START: validated voice; preroll retained; streaming upload");
            }
        } else {
            const size_t remaining = kMaxSamples - published_.load();
            if (!Publish(block.data(), std::min(block.size(), remaining)))
                break;
        }
        if (voice_started_ && (event == AUDIO_VAD_END_SILENCE ||
                               published_.load() >= kMaxSamples)) {
            reason = event == AUDIO_VAD_END_SILENCE ? "silence" : "limit";
            break;
        }
    }
    recording_ended_ = xTaskGetTickCount();
    codec_->EnableInput(false);
    if (!Failure() && !cancelled_ && voice_started_) {
        ESP_LOGI("VOICE", "VOICE_CAPTURE_DONE: samples=%u packets=%u reason=%s",
                 unsigned(SamplesRecorded()), unsigned(ExpectedPackets()), reason);
    } else if (!Failure()) {
        Fail("voice capture cancelled");
    }
    MemoryDiagnosticsStage("voice_capture_worker_done");
    done_.store(true, std::memory_order_release);
}
