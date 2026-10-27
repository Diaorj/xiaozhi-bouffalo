#include <cstdio>

#include <esp_log.h>

#include "device_state_machine.h"
#include "memory_diagnostics.h"
#include "wifi_init.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

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

static void heartbeat_task(void* arg) {
    (void)arg;
    ESP_LOGI(TAG, "TASK_RUNNING: heartbeat");
    // This is only a state-machine smoke test, not real activation or networking.
    DeviceStateMachine states;
    const int listener =
        states.AddStateChangeListener([](DeviceState old_state, DeviceState new_state) {
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

    if (!StartWifiInitialization(XIAOZHI_WIFI_SSID, XIAOZHI_WIFI_PASSWORD)) {
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
    std::printf("[XIAOZHI_PORT] BUILD_TAG: " XIAOZHI_BUILD_TAG " %s %s\r\n", __DATE__, __TIME__);
    // The SDK wraps main() to run global constructors. Do not run them again here.
    if (constructor_runs != 1) {
        std::printf("[XIAOZHI_PORT] CXX_RUNTIME_FAILED: constructors=%u\r\n", constructor_runs);
        while (true) {
        }
    }
    std::printf("[XIAOZHI_PORT] CXX_RUNTIME_OK: constructors=%u\r\n", constructor_runs);
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
