#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "protocols/protocol.h"

class AudioService;
class AudioCodec;
// A single bounded utterance. Compressed payloads, like recorded PCM, use PSRAM.
class VoiceInputClip {
public:
    static constexpr size_t kFrames = 84;  // 5 seconds + final frame zero padding
    static constexpr size_t kPacketBytes = 512;
    VoiceInputClip();
    ~VoiceInputClip();
    VoiceInputClip(const VoiceInputClip&) = delete;
    VoiceInputClip& operator=(const VoiceInputClip&) = delete;
    bool valid() const { return payload_ != nullptr; }
    bool Append(const AudioStreamPacket& packet);
    std::unique_ptr<AudioStreamPacket> Packet(size_t index) const;
    size_t size() const { return count_; }
private:
    uint8_t* payload_ = nullptr;
    uint16_t lengths_[kFrames]{};
    size_t count_ = 0;
};

// Owner/network task only. Capture, then encode using AudioService queues.
// No TLS connection or playback workers exist during these phases.
std::unique_ptr<VoiceInputClip> PrepareVoiceInput(AudioService& audio, AudioCodec* codec);
