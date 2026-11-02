#include "speaker_worker.h"
#include <stdbool.h>
#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

// Reserve internal RAM at link time: late playback must not depend on finding
// a contiguous 6 KiB block in the fragmented Wi-Fi/TLS/application heap.
static StackType_t worker_stack[SPEAKER_WORKER_STACK_WORDS];
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

int SpeakerWorkerStart(void (*job)(void*), void* context) {
    if (!job || pending)
        return -1;
    if (!done)
        done = xEventGroupCreateStatic(&done_storage);
    if (!done)
        return -1;
    if (!worker) {
        worker = xTaskCreateStatic(worker_entry, "speaker_dma", SPEAKER_WORKER_STACK_WORDS,
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

int SpeakerWorkerWait(void) {
    if (!pending)
        return -1;
    xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, portMAX_DELAY);
    pending = false;
    return 0;
}
