#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// CPU-only Opus state lives in PSRAM; DMA buffers and task stacks remain internal.
void* AudioSdkAllocateOpusState(size_t bytes);
void AudioSdkFreeOpusState(void* state);
int AudioSdkOpen(unsigned volume);
int AudioSdkWrite(const int16_t* pcm, size_t frames);
int AudioSdkFinish(void);
void AudioSdkAbort(void);
#ifdef __cplusplus
}
#endif
