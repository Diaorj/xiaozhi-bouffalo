#pragma once

#include <cstdint>
#include <vector>

struct AudioStreamPacket;

// A platform decoder produces PCM at the AudioCodec output rate.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    virtual bool Decode(const AudioStreamPacket& packet, std::vector<int16_t>& pcm) = 0;
    virtual void Reset() = 0;
};
