#include "audio_dma_worker.h"
#include <stdbool.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

// One fixed internal stack serves recording and playback in turn. Late audio
// must not depend on finding a contiguous 6 KiB block in the fragmented heap.
static StackType_t worker_stack[AUDIO_DMA_WORKER_STACK_WORDS];
static StaticTask_t worker_tcb;
static StaticEventGroup_t done_storage;
static EventGroupHandle_t done;
static TaskHandle_t worker;
static void (*current_job)(void*);
static void* current_context;
static bool pending;

static void worker_entry(void* context) {
    (void)context;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        current_job(current_context);
        // The job must not access its session buffers after publishing done.
        xEventGroupSetBits(done, 1);
        // Keep the task alive; never reuse a static TCB awaiting idle cleanup.
    }
}

int AudioDmaWorkerStart(void (*job)(void*), void* context) {
    if (!job || pending)
        return -1;
    if (!done)
        done = xEventGroupCreateStatic(&done_storage);
    if (!done)
        return -1;
    if (!worker) {
        worker = xTaskCreateStatic(worker_entry, "audio_dma", AUDIO_DMA_WORKER_STACK_WORDS,
                                   NULL, 8, worker_stack, &worker_tcb);
    }
    if (!worker)
        return -1;
    xEventGroupClearBits(done, 1);
    current_job = job;
    current_context = context;
    pending = true;
    xTaskNotifyGive(worker);
    return 0;
}

int AudioDmaWorkerWait(void) {
    if (!pending)
        return -1;
    xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, portMAX_DELAY);
    pending = false;
    return 0;
}
