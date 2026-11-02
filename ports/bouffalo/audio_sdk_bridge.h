#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// CPU-only audio buffers/state live in PSRAM; DMA buffers and task stacks remain internal.
void* AudioSdkAllocateBuffer(size_t bytes);
void AudioSdkFreeBuffer(void* buffer);
// Half duplex. The owner closes capture before starting output.
int AudioSdkRecordOpen(void);
int AudioSdkRead(int16_t* pcm, size_t frames);
void AudioSdkRecordClose(void);
void* AudioSdkAllocateOpusState(size_t bytes);
void AudioSdkFreeOpusState(void* state);
int AudioSdkOpen(unsigned volume);
int AudioSdkWrite(const int16_t* pcm, size_t frames);
int AudioSdkFinish(void);
void AudioSdkAbort(void);
#ifdef __cplusplus
}
#endif
