#include "wifi_init.h"
#include "network_probe.h"
#include "wifi_sdk_bridge.h"

#include <cstring>

#include <esp_log.h>

extern "C" {
#include "FreeRTOS.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "rfparam_adapter.h"
#include "task.h"
}

namespace {
const char* const TAG = "WIFI_INIT";
constexpr uint32_t kTcpipReady = 1u << 0;
constexpr uint32_t kDriverReady = 1u << 1;
constexpr uint32_t kManagerReady = 1u << 2;
constexpr uint32_t kAssociated = 1u << 3;
constexpr uint32_t kGotIpv4 = 1u << 4;
constexpr uint32_t kDisconnected = 1u << 5;
constexpr uint32_t kLostIpv4 = 1u << 6;
constexpr uint32_t kDhcpTimeout = 1u << 7;
constexpr uint32_t kParameterError = 1u << 8;
constexpr uint32_t kInfoReady = 1u << 9;
constexpr uint32_t kInfoFailed = 1u << 10;
constexpr unsigned kStageTimeoutMs = 15000;
constexpr unsigned kConnectTimeoutMs = 30000;
char configured_ssid[33];
char configured_password[65];

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
        case WIFI_SDK_ASSOCIATED:
            Notify(static_cast<TaskHandle_t>(context), kAssociated);
            break;
        case WIFI_SDK_GOT_IPV4:
            Notify(static_cast<TaskHandle_t>(context), kGotIpv4);
            break;
        case WIFI_SDK_DISCONNECTED:
            Notify(static_cast<TaskHandle_t>(context), kDisconnected);
            break;
        case WIFI_SDK_LOST_IPV4:
            Notify(static_cast<TaskHandle_t>(context), kLostIpv4);
            break;
        case WIFI_SDK_DHCP_TIMEOUT:
            Notify(static_cast<TaskHandle_t>(context), kDhcpTimeout);
            break;
        case WIFI_SDK_PARAMETER_ERROR:
            Notify(static_cast<TaskHandle_t>(context), kParameterError);
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
    ESP_LOGI(TAG, "WIFI_INIT_BEGIN: RF, lwIP, Wi-Fi driver and manager");
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
    ESP_LOGI(TAG, "WIFI_INIT_OK: driver and manager ready");
    return true;
}

// This callback runs in the lwIP core thread, where netif and DNS can be read.
void PrintNetworkInfo(void* context) {
    struct netif* iface;
    NETIF_FOREACH(iface) {
        if (!netif_is_up(iface) || !netif_is_link_up(iface) ||
            ip4_addr_isany_val(*netif_ip4_addr(iface)) ||
            ip4_addr_isloopback(netif_ip4_addr(iface))) {
            continue;
        }
        if (iface->hwaddr_len == 6) {
            ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", iface->hwaddr[0],
                     iface->hwaddr[1], iface->hwaddr[2], iface->hwaddr[3], iface->hwaddr[4],
                     iface->hwaddr[5]);
        }
        ESP_LOGI(TAG, "IP address: %s", ip4addr_ntoa(netif_ip4_addr(iface)));
        ESP_LOGI(TAG, "Netmask: %s", ip4addr_ntoa(netif_ip4_netmask(iface)));
        ESP_LOGI(TAG, "Gateway: %s", ip4addr_ntoa(netif_ip4_gw(iface)));
        ESP_LOGI(TAG, "DNS server: %s", ipaddr_ntoa(dns_getserver(0)));
        Notify(static_cast<TaskHandle_t>(context), kInfoReady);
        return;
    }
    Notify(static_cast<TaskHandle_t>(context), kInfoFailed);
}

bool ReportAddress(TaskHandle_t task) {
    if (tcpip_try_callback(PrintNetworkInfo, task) != ERR_OK) {
        ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: cannot schedule address inspection");
        return false;
    }
    uint32_t events = 0;
    if (xTaskNotifyWait(0, UINT32_MAX, &events, pdMS_TO_TICKS(kStageTimeoutMs)) != pdTRUE ||
        (events & kInfoReady) == 0 ||
        (events & (kDisconnected | kLostIpv4 | kDhcpTimeout | kParameterError | kInfoFailed)) != 0) {
        ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: no usable IPv4 address; reset to retry");
        return false;
    }
    ESP_LOGI(TAG, "WIFI_READY: IPv4 acquired; DNS/Internet/server not tested");
    return true;
}

bool ConnectHotspot(TaskHandle_t task) {
    if (configured_ssid[0] == '\0') {
        ESP_LOGW(TAG, "WIFI_NOT_CONFIGURED: edit ports/bouffalo/wifi_config.h and rebuild");
        return false;
    }
    ESP_LOGI(TAG, "WIFI_CONNECT_BEGIN: configured 2.4 GHz hotspot; one attempt");
    const int result = WifiSdkConnect(configured_ssid, configured_password);
    if (result != 0) {
        ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: request ret=%d; reset to retry", result);
        return false;
    }
    bool associated = false;
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t timeout = pdMS_TO_TICKS(kConnectTimeoutMs);
        uint32_t events = 0;
        if (elapsed >= timeout ||
            xTaskNotifyWait(0, UINT32_MAX, &events, timeout - elapsed) != pdTRUE) {
            ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: timeout waiting for %s; reset to retry",
                     associated ? "DHCP" : "association");
            return false;
        }
        if ((events & (kDisconnected | kLostIpv4 | kDhcpTimeout | kParameterError)) != 0) {
            ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: %s; reset to retry",
                     (events & kParameterError) ? "parameters rejected" :
                     (events & kDhcpTimeout) ? "DHCP timeout" : "connection lost");
            return false;
        }
        if ((events & kAssociated) != 0 && !associated) {
            associated = true;
            started = xTaskGetTickCount();
            ESP_LOGI(TAG, "WIFI_ASSOCIATED: waiting for DHCP");
        }
        if ((events & kGotIpv4) != 0) {
            return ReportAddress(task);
        }
    }
}

void NetworkTask(void*) {
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    if (InitializeNetwork(task) && ConnectHotspot(task)) {
        RunNetworkProbe();
    }
    // Callbacks retain this handle, including a possibly late lwIP init callback.
    // Keep the task alive and blocked, so a timeout cannot leave a dangling handle.
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        if ((events & (kDisconnected | kLostIpv4)) != 0) {
            ESP_LOGW(TAG, "WIFI_OFFLINE: no automatic retry in this step; reset to reconnect");
        }
    }
}
}  // namespace

bool StartWifiInitialization(const char* ssid, const char* password) {
    static bool started = false;
    if (started) {
        return true;
    }
    if (ssid == nullptr || password == nullptr || std::strlen(ssid) > 32 ||
        std::strlen(password) > 64) {
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: invalid credential length (SSID <=32, key <=64)");
        return false;
    }
    std::strcpy(configured_ssid, ssid);
    std::strcpy(configured_password, password);
    started = true;
    // HTTPS adds a deeper call stack: 3072 StackType_t units (12 KiB), priority 10.
    if (xTaskCreate(NetworkTask, "network_init", 3072, nullptr, 10, nullptr) != pdPASS) {
        started = false;
        ESP_LOGW(TAG, "WIFI_INIT_FAILED: cannot create network initialization task");
        return false;
    }
    return true;
}
