#include "audio_encoder_worker.h"
#include <stdbool.h>
#include "event_groups.h"
#include "mm.h"
#define DBG_TAG "ENCODER_WORKER"
#include "log.h"

#define STACK_BYTES 32768U
static StackType_t* stack;
static StaticTask_t tcb;
static StaticEventGroup_t done_storage;
static EventGroupHandle_t done;
static TaskHandle_t worker;
static void (*current_job)(void*);
static void* current_context;
static bool pending;

static void worker_entry(void* unused) {
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        current_job(current_context);
        xEventGroupSetBits(done, 1);
    }
}
int AudioEncoderWorkerStart(void (*job)(void*), void* context, TaskHandle_t* handle) {
    if (!job || !handle || pending)
        return -1;
    *handle = NULL;
    if (!stack) {
        // BL616CL BSP registers unused Wi-Fi RAM as an independent internal heap.
        // Keep 16 KiB available there; never silently take the OCRAM playback heap.
        if (kfree_size(MM_HEAP_WRAM_0) < STACK_BYTES + 16384U ||
            kmax_free_size(MM_HEAP_WRAM_0) < STACK_BYTES) {
            LOG_E("WRAM_STACK_UNAVAILABLE: free=%lu largest=%lu need=%u\r\n",
                  (unsigned long)kfree_size(MM_HEAP_WRAM_0),
                  (unsigned long)kmax_free_size(MM_HEAP_WRAM_0), STACK_BYTES);
            return -1;
        }
        stack = kmalloc(STACK_BYTES, MM_FLAG_HEAP_WRAM_0 | MM_FLAG_ALIGN_16BYTE);
        if (!stack)
            return -1;
        LOG_I("ENCODER_STACK_RESERVED: internal WRAM bytes=%u ptr=%p\r\n", STACK_BYTES, stack);
    }
    if (!done)
        done = xEventGroupCreateStatic(&done_storage);
    if (!done)
        return -1;
    if (!worker)
        worker = xTaskCreateStatic(worker_entry, "opus_encode", STACK_BYTES / sizeof(StackType_t),
                                   NULL, 6, stack, &tcb);
    if (!worker)
        return -1;
    xEventGroupClearBits(done, 1);
    current_job = job;
    current_context = context;
    pending = true;
    *handle = worker;
    xTaskNotifyGive(worker);
    return 0;
}
int AudioEncoderWorkerWait(void) {
    if (!pending)
        return -1;
    xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, portMAX_DELAY);
    pending = false;
    return 0;
}
