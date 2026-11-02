#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A platform encoder consumes mono PCM at 16 kHz in complete Opus frames.
class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    virtual size_t FrameSamples() const = 0;
    virtual bool Encode(const std::vector<int16_t>& pcm, std::vector<uint8_t>& opus) = 0;
};
