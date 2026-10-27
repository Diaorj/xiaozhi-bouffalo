#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t PcmRingPack(const int16_t* ring, size_t capacity, uint32_t published,
                   uint32_t offset, uint32_t* stereo, size_t frames, unsigned volume);
#ifdef __cplusplus
}
#endif
