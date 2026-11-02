#include "voice_input.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <esp_log.h>
#include "audio/audio_service.h"
#include "audio_sdk_bridge.h"
#include "memory_diagnostics.h"
#include "opus_encoder_bouffalo.h"

VoiceInputClip::VoiceInputClip()
    : payload_(static_cast<uint8_t*>(AudioSdkAllocateBuffer(kFrames * kPacketBytes))) {}
VoiceInputClip::~VoiceInputClip() { AudioSdkFreeBuffer(payload_); }
bool VoiceInputClip::Append(const AudioStreamPacket& packet) {
    if (!valid() || count_ >= kFrames || packet.payload.empty() ||
        packet.payload.size() > kPacketBytes || packet.sample_rate != 16000 ||
        packet.frame_duration != 60)
        return false;
    std::memcpy(payload_ + count_ * kPacketBytes, packet.payload.data(), packet.payload.size());
    lengths_[count_++] = packet.payload.size();
    return true;
}
std::unique_ptr<AudioStreamPacket> VoiceInputClip::Packet(size_t index) const {
    if (index >= count_)
        return nullptr;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = 16000;
    packet->frame_duration = 60;
    const uint8_t* begin = payload_ + index * kPacketBytes;
    packet->payload.assign(begin, begin + lengths_[index]);
    return packet;
}

std::unique_ptr<VoiceInputClip> PrepareVoiceInput(AudioService& audio, AudioCodec* codec) {
    constexpr size_t kSamples = 16000 * 5;
    constexpr size_t kFrameSamples = 960;
    if (!codec || audio.IsRunning())
        return nullptr;
    auto clip = std::unique_ptr<VoiceInputClip>(new (std::nothrow) VoiceInputClip);
    auto free_pcm = [](int16_t* p) { AudioSdkFreeBuffer(p); };
    std::unique_ptr<int16_t, decltype(free_pcm)> pcm(
        static_cast<int16_t*>(AudioSdkAllocateBuffer(kSamples * sizeof(int16_t))), free_pcm);
    if (!clip || !clip->valid() || !pcm) {
        ESP_LOGE("VOICE", "VOICE_PSRAM_ALLOC_FAILED");
        return nullptr;
    }
    codec->EnableInput(true);
    if (!codec->input_enabled())
        return nullptr;
    ESP_LOGI("VOICE", "RECORDING: speak now, 5 seconds; one short question");
    std::vector<int16_t> block(160);
    uint32_t peak = 0;
    uint64_t sum = 0;
    bool captured = true;
    for (size_t offset = 0; offset < kSamples; offset += block.size()) {
        if (!codec->InputData(block)) {
            captured = false;
            break;
        }
        std::memcpy(pcm.get() + offset, block.data(), block.size() * sizeof(int16_t));
        for (int16_t sample : block) {
            uint32_t magnitude = sample < 0 ? -int32_t(sample) : sample;
            peak = std::max(peak, magnitude);
            sum += magnitude;
        }
    }
    codec->EnableInput(false);  // Always stop DMA/ADC before encoding or networking.
    if (!captured) {
        ESP_LOGE("VOICE", "VOICE_CAPTURE_FAILED: utterance discarded");
        return nullptr;
    }
    ESP_LOGI("VOICE", "VOICE_CAPTURE_DONE: samples=80000 peak=%u mean_abs=%u", peak,
             static_cast<unsigned>(sum / kSamples));
    if (peak < 100)
        ESP_LOGW("VOICE", "VOICE_WEAK: check microphone/speech against recorded levels");
    auto encoder = CreateBouffaloOpusEncoder();
    MemoryDiagnosticsStage("before_voice_encoding");
    if (!encoder || !audio.StartEncoding(std::move(encoder))) {
        ESP_LOGE("VOICE", "VOICE_ENCODER_START_FAILED");
        return nullptr;
    }
    size_t queued = 0, received = 0;
    bool ok = true;
    const TickType_t start = xTaskGetTickCount();
    std::vector<int16_t> frame;
    while (received < VoiceInputClip::kFrames) {
        while (auto packet = audio.PopPacketFromSendQueue()) {
            if (!clip->Append(*packet)) {
                ok = false;
                break;
            }
            received++;
        }
        if (!ok || audio.HasEncodingError() || !audio.IsRunning() ||
            xTaskGetTickCount() - start > pdMS_TO_TICKS(30000)) {
            ok = false;
            break;
        }
        if (queued < VoiceInputClip::kFrames) {
            if (frame.empty()) {
                frame.assign(kFrameSamples, 0);
                size_t offset = queued * kFrameSamples;
                size_t n = std::min(kFrameSamples, kSamples - offset);
                std::memcpy(frame.data(), pcm.get() + offset, n * sizeof(int16_t));
            }
            // A full queue returns false without consuming frame. Do not drop speech.
            if (audio.PushPcmToEncodeQueue(std::move(frame)))
                queued++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    while (ok && !audio.IsEncodingIdle())
        vTaskDelay(pdMS_TO_TICKS(1));
    ok = ok && !audio.HasEncodingError() && received == VoiceInputClip::kFrames;
    audio.Stop();  // Joins worker, releases encoder state; no output worker was created.
    // FreeRTOS idle reclaims a deleted dynamic task's stack.
    vTaskDelay(pdMS_TO_TICKS(20));
    pcm.reset();
    MemoryDiagnosticsStage("after_voice_encoding");
    ESP_LOGI("VOICE", "VOICE_ENCODE_%s: packets=%u; PCM and encoder released", ok ? "DONE" : "FAILED",
             static_cast<unsigned>(received));
    return ok ? std::move(clip) : nullptr;
}
