#include <cstdio>

#include <esp_log.h>

#include "device_state_machine.h"
#include "wifi_init.h"

// These SDK and FreeRTOS functions are implemented in C.
extern "C" {
#include "FreeRTOS.h"
#include "board.h"
#include "task.h"
void __libc_init_array(void);
}

// A constructor probe also verifies the SDK's init-array path.
static bool constructor_ran = false;
struct ConstructorProbe {
    ConstructorProbe() { constructor_ran = true; }
};
static ConstructorProbe constructor_probe;

static const char* const TAG = "XIAOZHI_PORT";

static void heartbeat_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "TASK_RUNNING: heartbeat");
    // This is only a state-machine smoke test, not real activation or networking.
    DeviceStateMachine states;
    const int listener = states.AddStateChangeListener(
        [](DeviceState old_state, DeviceState new_state) {
            ESP_LOGI(TAG, "STATE_CALLBACK: %s -> %s", DeviceStateMachine::GetStateName(old_state),
                     DeviceStateMachine::GetStateName(new_state));
        });
    bool ok = states.TransitionTo(kDeviceStateStarting);
    ok = states.TransitionTo(kDeviceStateActivating) && ok;
    ok = states.TransitionTo(kDeviceStateIdle) && ok;
    // idle -> starting is forbidden: it must fail and leave the state unchanged.
    ok = !states.TransitionTo(kDeviceStateStarting) && ok;
    ok = states.GetState() == kDeviceStateIdle && ok;
    states.RemoveStateChangeListener(listener);
    ESP_LOGI(TAG, "STATE_MACHINE_%s: current=%s (demo only)", ok ? "OK" : "FAILED",
             DeviceStateMachine::GetStateName(states.GetState()));

    if (!StartWifiInitialization()) {
        ESP_LOGW(TAG, "NETWORK_INIT_NOT_STARTED: heartbeat will continue");
    }

    unsigned int seconds = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ++seconds;
        ESP_LOGI(TAG, "FreeRTOS alive=%u s", seconds);
    }
}

int main() {
    board_init();
    // BL616CL startup does not call this; run global C++ constructors once,
    // after board/heap initialization and before creating any application task.
    __libc_init_array();
    if (!constructor_ran) {
        std::printf("[XIAOZHI_PORT] CXX_RUNTIME_FAILED\r\n");
        while (true) {
        }
    }
    std::printf("[XIAOZHI_PORT] CXX_RUNTIME_OK\r\n");
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
