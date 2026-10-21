#include "wifi_init.h"
#include "wifi_sdk_bridge.h"

#include <esp_log.h>

extern "C" {
#include "FreeRTOS.h"
#include "lwip/tcpip.h"
#include "rfparam_adapter.h"
#include "task.h"
}

namespace {
const char* const TAG = "WIFI_INIT";
constexpr uint32_t kTcpipReady = 1u << 0;
constexpr uint32_t kDriverReady = 1u << 1;
constexpr uint32_t kManagerReady = 1u << 2;
constexpr unsigned kStageTimeoutMs = 15000;

void Notify(TaskHandle_t task, uint32_t bit) {
    // eSetBits does not wait, including when the SDK suspends scheduling.
    const BaseType_t result = xTaskNotify(task, bit, eSetBits);
    configASSERT(result == pdPASS);
    (void)result;
}

void OnTcpipReady(void* context) {
    Notify(static_cast<TaskHandle_t>(context), kTcpipReady);
}

void OnWifiEvent(wifi_sdk_event_t event, void* context) {
    switch (event) {
        case WIFI_SDK_DRIVER_READY:
            Notify(static_cast<TaskHandle_t>(context), kDriverReady);
            break;
        case WIFI_SDK_MANAGER_READY:
            Notify(static_cast<TaskHandle_t>(context), kManagerReady);
            break;
        default:
            break;
    }
}

bool WaitFor(uint32_t bit, const char* stage) {
    uint32_t events = 0;
    if (xTaskNotifyWait(0, bit, &events, pdMS_TO_TICKS(kStageTimeoutMs)) != pdTRUE ||
        (events & bit) == 0) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: timeout waiting for %s; reset to retry", stage);
        return false;
    }
    return true;
}

bool InitializeNetwork(TaskHandle_t task) {
    ESP_LOGI(TAG, "WIFI_INIT_BEGIN: initialize only; hotspot connection is not requested");
    const int rf_result = rfparam_init(0, nullptr, 0);
    if (rf_result != 0) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: rfparam_init ret=%d", rf_result);
        return false;
    }
    ESP_LOGI(TAG, "RF_INIT_OK");

    tcpip_init(OnTcpipReady, task);
    if (!WaitFor(kTcpipReady, "lwIP core")) {
        return false;
    }
    ESP_LOGI(TAG, "LWIP_INIT_OK: TCP/IP core thread is running");

    const int register_result = WifiSdkRegisterEvents(OnWifiEvent, task);
    if (register_result != 0) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: event registration ret=%d", register_result);
        return false;
    }
    const int host_result = WifiSdkStartDriver();
    if (host_result != 0) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: fhost_init ret=%d", host_result);
        return false;
    }
    if (!WaitFor(kDriverReady, "Wi-Fi driver")) {
        return false;
    }
    ESP_LOGI(TAG, "WIFI_DRIVER_READY");

    // Start the manager from this task, rather than the SDK's event callback.
    const int manager_result = WifiSdkStartManager();
    if (manager_result != 0) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: manager start ret=%d", manager_result);
        return false;
    }
    if (!WaitFor(kManagerReady, "Wi-Fi manager")) {
        return false;
    }
    ESP_LOGI(TAG, "WIFI_INIT_OK: driver and manager ready; no connection requested");
    return true;
}

void NetworkTask(void*) {
    InitializeNetwork(xTaskGetCurrentTaskHandle());
    // Callbacks retain this handle, including a possibly late lwIP init callback.
    // Keep the task alive and blocked, so a timeout cannot leave a dangling handle.
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
    }
}
}  // namespace

bool StartWifiInitialization() {
    static bool started = false;
    if (started) {
        return true;
    }
    started = true;
    // SDK Wi-Fi examples use priority 10 and 1024 StackType_t units (4 KiB).
    if (xTaskCreate(NetworkTask, "network_init", 1024, nullptr, 10, nullptr) != pdPASS) {
        started = false;
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: cannot create network initialization task");
        return false;
    }
    return true;
}
