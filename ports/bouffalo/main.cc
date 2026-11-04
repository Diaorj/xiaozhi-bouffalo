#include <cstdio>

#include <esp_log.h>

#include "application.h"
#include "memory_diagnostics.h"
#include "screen_test.h"

// These SDK and FreeRTOS functions are implemented in C.
extern "C" {
#include "FreeRTOS.h"
#include "sdk_board.h"
#include "task.h"
}

// A constructor probe also verifies the SDK's init-array path.
static unsigned int constructor_runs = 0;
struct ConstructorProbe {
    ConstructorProbe() { ++constructor_runs; }
};
static ConstructorProbe constructor_probe;

static const char* const TAG = "XIAOZHI_PORT";

static void application_task(void*) {
    ESP_LOGI(TAG, "TASK_RUNNING: original Application");
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();
    ESP_LOGE(TAG, "APPLICATION_EXITED: unexpected main loop return");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int main() {
    board_init();
    std::printf("[XIAOZHI_PORT] BUILD_TAG: " XIAOZHI_BUILD_TAG " %s %s\r\n", __DATE__, __TIME__);
    // The SDK wraps main() to run global constructors. Do not run them again here.
    if (constructor_runs != 1) {
        std::printf("[XIAOZHI_PORT] CXX_RUNTIME_FAILED: constructors=%u\r\n", constructor_runs);
        while (true) {
        }
    }
    std::printf("[XIAOZHI_PORT] CXX_RUNTIME_OK: constructors=%u\r\n", constructor_runs);
    std::printf("\r\n[XIAOZHI_PORT] BOOT_OK: BL616CL C++ FreeRTOS entry\r\n");

#if XIAOZHI_SCREEN_TEST
    if (RunScreenBringupTest() != 0) {
        std::printf("[W][XIAOZHI_PORT] SCREEN_TEST_FAILED: continuing voice startup\r\n");
    }
#endif

    // Stack depth is in StackType_t units: 1024 * 4 bytes on BL616CL.
    if (xTaskCreate(application_task, "application", 1024, nullptr, 1, nullptr) != pdPASS) {
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
