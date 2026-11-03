#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DMA_WORKER_STACK_WORDS 1536U
// One owner starts a job; that job must be joined before starting another.
int AudioDmaWorkerStart(void (*job)(void*), void* context);
int AudioDmaWorkerWait(void);

#ifdef __cplusplus
}
#endif
