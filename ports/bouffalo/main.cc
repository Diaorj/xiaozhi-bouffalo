#include <cstdio>

#include <esp_log.h>

// These SDK and FreeRTOS functions are implemented in C.
extern "C" {
#include "FreeRTOS.h"
#include "board.h"
#include "task.h"
}

static const char* const TAG = "XIAOZHI_PORT";

static void heartbeat_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "TASK_RUNNING: heartbeat");
    ESP_LOGW(TAG, "LOG_ADAPTER_TEST: expected warning");

    unsigned int seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ++seconds;
        ESP_LOGI(TAG, "FreeRTOS alive=%u s", seconds);
    }
}

int main() {
    board_init();
    std::printf("\r\n[XIAOZHI_PORT] BOOT_OK: BL616CL C++ FreeRTOS entry\r\n");

    // Stack depth is in StackType_t units: 1024 * 4 bytes on BL616CL.
    if (xTaskCreate(heartbeat_task, "heartbeat", 1024, nullptr, 1, nullptr) != pdPASS) {
        std::printf("[XIAOZHI_PORT] TASK_CREATE_FAILED\r\n");
        while (true) {
        }
    }

    std::printf("[XIAOZHI_PORT] SCHEDULER_START\r\n");
    vTaskStartScheduler();

    // A running scheduler normally does not return here.
    std::printf("[XIAOZHI_PORT] SCHEDULER_START_FAILED\r\n");
    while (true) {
    }
}
