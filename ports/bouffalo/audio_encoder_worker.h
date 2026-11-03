#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "FreeRTOS.h"
#include "task.h"
// One owner starts/joins jobs. The internal-WRAM stack/task is retained for reuse.
int AudioEncoderWorkerStart(void (*job)(void*), void* context, TaskHandle_t* handle);
int AudioEncoderWorkerWait(void);
#ifdef __cplusplus
}
#endif
