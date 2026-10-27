#include "opus_decoder_bouffalo.h"
#include <esp_log.h>
#include <opus/opus.h>
#include <new>
#include "audio_sdk_bridge.h"
#include "protocols/protocol.h"

namespace {
class BouffaloOpusDecoder final : public AudioDecoder {
public:
    explicit BouffaloOpusDecoder(OpusDecoder* decoder) : decoder_(decoder) {}
    ~BouffaloOpusDecoder() override { AudioSdkFreeOpusState(decoder_); }
    bool Decode(const AudioStreamPacket& packet, std::vector<int16_t>& pcm) override {
        if (packet.payload.empty() || packet.payload.size() > 4096 ||
            (packet.sample_rate != 8000 && packet.sample_rate != 12000 &&
             packet.sample_rate != 16000 && packet.sample_rate != 24000 &&
             packet.sample_rate != 48000) ||
            packet.frame_duration <= 0 || packet.frame_duration > 120)
            return false;
        // Opus can directly decode a 24 kHz stream at our verified 16 kHz output rate.
        pcm.resize(1920);  // 120 ms maximum, independent of the server's frame hint.
        const int n = opus_decode(decoder_, packet.payload.data(), packet.payload.size(),
                                  pcm.data(), pcm.size(), 0);
        if (n <= 0)
            return false;
        pcm.resize(n);
        return true;
    }
    void Reset() override { opus_decoder_ctl(decoder_, OPUS_RESET_STATE); }

private:
    OpusDecoder* decoder_;
};
}  // namespace

std::unique_ptr<AudioDecoder> CreateBouffaloOpusDecoder() {
    const int bytes = opus_decoder_get_size(1);
    if (bytes <= 0)
        return nullptr;
    auto* decoder = static_cast<OpusDecoder*>(AudioSdkAllocateOpusState(bytes));
    if (!decoder) {
        ESP_LOGE("Audio", "OPUS_PSRAM_ALLOC_FAILED: bytes=%d", bytes);
        return nullptr;
    }
    const int error = opus_decoder_init(decoder, 16000, 1);
    if (error != OPUS_OK) {
        AudioSdkFreeOpusState(decoder);
        ESP_LOGE("Audio", "OPUS_INIT_FAILED: %d", error);
        return nullptr;
    }
    auto result = std::unique_ptr<AudioDecoder>(new (std::nothrow) BouffaloOpusDecoder(decoder));
    if (!result) {
        AudioSdkFreeOpusState(decoder);
        ESP_LOGE("Audio", "OPUS_WRAPPER_ALLOC_FAILED");
    }
    return result;
}
