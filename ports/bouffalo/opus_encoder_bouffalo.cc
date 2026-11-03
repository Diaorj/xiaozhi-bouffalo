#include "opus_encoder_bouffalo.h"
#include <esp_log.h>
#include <new>
#include <opus/opus.h>
#include "audio_sdk_bridge.h"
#include "memory_diagnostics.h"

namespace {
class BouffaloOpusEncoder final : public AudioEncoder {
public:
    explicit BouffaloOpusEncoder(OpusEncoder* encoder) : encoder_(encoder) {}
    ~BouffaloOpusEncoder() override { AudioSdkFreeOpusState(encoder_); }
    size_t FrameSamples() const override { return 960; }  // 16 kHz, 60 ms
    bool Encode(const std::vector<int16_t>& pcm, std::vector<uint8_t>& packet) override {
        if (pcm.size() != FrameSamples())
            return false;
        // Fixed 16 kbps / 60 ms produces 120 bytes; keep a bounded margin.
        packet.resize(256);
        int bytes = opus_encode(encoder_, pcm.data(), pcm.size(), packet.data(), packet.size());
        if (bytes <= 0) {
            ESP_LOGE("Audio", "OPUS_ENCODE_FAILED: %d", bytes);
            packet.clear();
            return false;
        }
        if (!logged_) {
            MemoryDiagnosticsStage("voice_encoder_first_frame");
            logged_ = true;
        }
        packet.resize(bytes);
        return true;
    }
private:
    OpusEncoder* encoder_;
    bool logged_ = false;
};
}

std::unique_ptr<AudioEncoder> CreateBouffaloOpusEncoder() {
    int bytes = opus_encoder_get_size(1);
    if (bytes <= 0)
        return nullptr;
    auto* encoder = static_cast<OpusEncoder*>(AudioSdkAllocateOpusState(bytes));
    if (!encoder)
        return nullptr;
    int error = opus_encoder_init(encoder, 16000, 1, OPUS_APPLICATION_VOIP);
    if (error == OPUS_OK)
        error = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(16000));
    if (error == OPUS_OK)
        error = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));
    if (error == OPUS_OK)
        error = opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    if (error == OPUS_OK)
        error = opus_encoder_ctl(encoder, OPUS_SET_DTX(0));
    if (error == OPUS_OK)
        error = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    if (error != OPUS_OK) {
        AudioSdkFreeOpusState(encoder);
        ESP_LOGE("Audio", "OPUS_ENCODER_INIT_FAILED: %d", error);
        return nullptr;
    }
    auto result = std::unique_ptr<AudioEncoder>(new (std::nothrow) BouffaloOpusEncoder(encoder));
    if (!result)
        AudioSdkFreeOpusState(encoder);
    return result;
}
