#ifndef XIAOZHI_BOUFFALO_FREERTOS_MUTEX_H
#define XIAOZHI_BOUFFALO_FREERTOS_MUTEX_H

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
}

// BasicLockable backend for std::lock_guard. Construct/use only from task context.
// Static storage avoids allocation and provides FreeRTOS priority inheritance.
class BouffaloMutex {
public:
    BouffaloMutex() : handle_(xSemaphoreCreateMutexStatic(&storage_)) {
        configASSERT(handle_ != nullptr);
    }
    ~BouffaloMutex() { vSemaphoreDelete(handle_); }

    BouffaloMutex(const BouffaloMutex&) = delete;
    BouffaloMutex& operator=(const BouffaloMutex&) = delete;

    void lock() {
        const BaseType_t result = xSemaphoreTake(handle_, portMAX_DELAY);
        configASSERT(result == pdTRUE);
        (void)result;
    }
    void unlock() {
        const BaseType_t result = xSemaphoreGive(handle_);
        configASSERT(result == pdTRUE);
        (void)result;
    }

private:
    StaticSemaphore_t storage_;
    SemaphoreHandle_t handle_;
};

#endif  // XIAOZHI_BOUFFALO_FREERTOS_MUTEX_H
