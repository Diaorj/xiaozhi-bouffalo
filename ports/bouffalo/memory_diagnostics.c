#include "memory_diagnostics.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "bflb_irq.h"
#include "mm.h"
#include "task.h"

// Keep the caller across the SDK malloc-failure hook without using the heap,
// changing task TLS slots, or relying on frame-pointer stack unwinding.
typedef struct {
    TaskHandle_t task;
    uintptr_t caller;
    size_t bytes;
    unsigned active;
} allocation_t;
static allocation_t allocations[32];
static const char* last_stage = "boot";

void* __real_pvPortMalloc(size_t bytes);
void* __real_kmalloc(size_t bytes, uint32_t flags);

static const char* current_name(TaskHandle_t task) {
    return task ? pcTaskGetName(task) : "before_scheduler";
}

static void print_heap(const char* stage, TaskHandle_t task) {
    printf(
        "[I][MEM] stage=%s task=%s ocram_free=%lu ocram_largest=%lu "
        "psram_free=%lu psram_largest=%lu stack_low_water_bytes=%lu\r\n",
        stage, current_name(task), (unsigned long)kfree_size(MM_HEAP_OCRAM_0),
        (unsigned long)kmax_free_size(MM_HEAP_OCRAM_0), (unsigned long)kfree_size(MM_HEAP_PSRAM_0),
        (unsigned long)kmax_free_size(MM_HEAP_PSRAM_0),
        task ? (unsigned long)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t) : 0UL);
}

void MemoryDiagnosticsStage(const char* stage) {
    if (!stage)
        return;
    __atomic_store_n(&last_stage, stage, __ATOMIC_RELEASE);  // Static string literals only.
    print_heap(stage, xTaskGetCurrentTaskHandle());
}

void* __wrap_pvPortMalloc(size_t bytes) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    allocation_t previous = {0};
    int slot = -1;
    uintptr_t irq = bflb_irq_save();
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].active && allocations[i].task == task) {
            slot = (int)i;
            break;
        }
        if (!allocations[i].active && slot < 0)
            slot = (int)i;
    }
    if (slot >= 0) {
        previous = allocations[slot];
        allocations[slot] = (allocation_t){task, (uintptr_t)__builtin_return_address(0), bytes, 1};
    }
    bflb_irq_restore(irq);
    void* result = __real_pvPortMalloc(bytes);
    irq = bflb_irq_save();
    if (slot >= 0)
        allocations[slot] = previous;
    bflb_irq_restore(irq);
    return result;
}

void* __wrap_kmalloc(size_t bytes, uint32_t flags) {
    void* result = __real_kmalloc(bytes, flags);
    if (!result) {
        printf("[E][MEM] KMALLOC_FAILED: bytes=%lu flags=0x%lx task=%s caller=0x%lx\r\n",
               (unsigned long)bytes, (unsigned long)flags,
               current_name(xTaskGetCurrentTaskHandle()),
               (unsigned long)(uintptr_t)__builtin_return_address(0));
        print_heap(__atomic_load_n(&last_stage, __ATOMIC_ACQUIRE), xTaskGetCurrentTaskHandle());
    }
    return result;
}

// Replace only the SDK weak application hook. Preserve its fatal behavior while
// reporting the real request and the caller *above* pvPortMalloc first.
void vApplicationMallocFailedHook(void) {
    static unsigned reporting;
    if (__atomic_exchange_n(&reporting, 1, __ATOMIC_RELAXED))
        abort();
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    allocation_t failed = {0};
    uintptr_t irq = bflb_irq_save();
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].active && allocations[i].task == task) {
            failed = allocations[i];
            break;
        }
    }
    bflb_irq_restore(irq);
    printf("[E][MEM] RTOS_ALLOC_FAILED: bytes=%lu task=%s caller=0x%lx stage=%s tracked=%u\r\n",
           (unsigned long)failed.bytes, current_name(task), (unsigned long)failed.caller,
           __atomic_load_n(&last_stage, __ATOMIC_ACQUIRE), failed.active);
    print_heap("malloc_failed_hook", task);
    abort();
}
