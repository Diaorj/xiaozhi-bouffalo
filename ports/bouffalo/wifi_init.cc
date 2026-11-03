#include "wifi_init.h"
#include "network_probe.h"
#include "ota_bootstrap.h"
#include "websocket_bootstrap.h"
#include "wifi_sdk_bridge.h"

#include <algorithm>
#include <atomic>
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
constexpr unsigned kRetryFirstMs = 3000;
constexpr unsigned kRetryMaxMs = 30000;
std::atomic<bool> link_failed{true};
std::atomic<bool> parameters_rejected{false};
std::atomic<uint32_t> info_completed{0};
std::atomic<bool> info_valid{false};
TaskHandle_t network_task;
uint32_t info_sequence = 0;
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
            link_failed = true;
            Notify(static_cast<TaskHandle_t>(context), kDisconnected);
            break;
        case WIFI_SDK_LOST_IPV4:
            link_failed = true;
            Notify(static_cast<TaskHandle_t>(context), kLostIpv4);
            break;
        case WIFI_SDK_DHCP_TIMEOUT:
            link_failed = true;
            Notify(static_cast<TaskHandle_t>(context), kDhcpTimeout);
            break;
        case WIFI_SDK_PARAMETER_ERROR:
            link_failed = true;
            parameters_rejected = true;
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
    const auto sequence = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context));
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
        info_valid = true;
        info_completed.store(sequence, std::memory_order_release);
        Notify(network_task, kInfoReady);
        return;
    }
    info_valid = false;
    info_completed.store(sequence, std::memory_order_release);
    Notify(network_task, kInfoFailed);
}

bool NetworkAvailable() {
    return !link_failed.load();
}

bool ReportAddress() {
    // Tag each request so a late lwIP callback from a timed-out attempt cannot
    // validate the next attempt. The callback's task owner has static lifetime.
    const uint32_t sequence = ++info_sequence;
    if (tcpip_try_callback(PrintNetworkInfo,
                           reinterpret_cast<void*>(static_cast<uintptr_t>(sequence))) != ERR_OK) {
        ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: cannot schedule address inspection");
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    while (NetworkAvailable()) {
        if (info_completed.load(std::memory_order_acquire) == sequence) {
            if (!info_valid.load())
                break;
            ESP_LOGI(TAG, "WIFI_READY: IPv4 acquired; DNS/Internet/server not tested");
            return true;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= pdMS_TO_TICKS(kStageTimeoutMs))
            break;
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, pdMS_TO_TICKS(kStageTimeoutMs) - elapsed);
    }
    ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: no usable IPv4 address");
    return false;
}

enum class ConnectResult { Ready, Retry, BadParameters };

ConnectResult ConnectHotspot() {
    // Consume previous-attempt notifications only after its cleanup/backoff.
    uint32_t stale = 0;
    xTaskNotifyWait(0, UINT32_MAX, &stale, 0);
    parameters_rejected = false;
    link_failed = false;
    ESP_LOGI(TAG, "WIFI_CONNECT_BEGIN: configured 2.4 GHz hotspot; PMF optional");
    const int result = WifiSdkConnect(configured_ssid, configured_password);
    if (result != 0) {
        ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: request ret=%d", result);
        return parameters_rejected ? ConnectResult::BadParameters : ConnectResult::Retry;
    }
    bool associated = false;
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t timeout = pdMS_TO_TICKS(kConnectTimeoutMs);
        uint32_t events = 0;
        if (elapsed >= timeout ||
            xTaskNotifyWait(0, UINT32_MAX, &events, timeout - elapsed) != pdTRUE) {
            ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: timeout waiting for %s",
                     associated ? "DHCP" : "association");
            return ConnectResult::Retry;
        }
        if ((events & (kDisconnected | kLostIpv4 | kDhcpTimeout | kParameterError)) != 0 ||
            !NetworkAvailable()) {
            ESP_LOGW(TAG, "WIFI_CONNECT_FAILED: %s",
                     parameters_rejected ? "parameters rejected" :
                     (events & kDhcpTimeout) ? "DHCP timeout" : "connection lost");
            return parameters_rejected ? ConnectResult::BadParameters : ConnectResult::Retry;
        }
        if ((events & kAssociated) != 0 && !associated) {
            associated = true;
            started = xTaskGetTickCount();
            ESP_LOGI(TAG, "WIFI_ASSOCIATED: waiting for DHCP");
        }
        if ((events & kGotIpv4) != 0)
            return ReportAddress() ? ConnectResult::Ready :
                   parameters_rejected ? ConnectResult::BadParameters : ConnectResult::Retry;
    }
}

bool ResetStation() {
    // A DHCP timeout can leave the station associated. Tear it down before a
    // fresh DHCP attempt; never overlap this with a live audio/TLS session.
    link_failed = true;
    const int result = WifiSdkDisconnect();
    if (result != 0) {
        ESP_LOGW(TAG, "WIFI_RESET_FAILED: disconnect ret=%d", result);
        return false;
    }
    const TickType_t started = xTaskGetTickCount();
    while (WifiSdkStationConnected()) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(kStageTimeoutMs)) {
            ESP_LOGW(TAG, "WIFI_RESET_FAILED: station still connected");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

void RetryAfter(const char* stage, unsigned& delay_ms) {
    ESP_LOGW(TAG, "NETWORK_RETRY: stage=%s in=%u ms", stage, delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    delay_ms = std::min(delay_ms * 2, kRetryMaxMs);
}

void WaitForLinkLoss() {
    while (NetworkAvailable()) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
    }
}

void ParkTask() {
    // SDK/lwIP callbacks retain the handle, even after a fatal initialization
    // error. Never delete it and leave a dangling callback context.
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
    }
}

void NetworkTask(void*) {
    network_task = xTaskGetCurrentTaskHandle();
    if (!InitializeNetwork(network_task)) {
        ParkTask();
        return;
    }
    if (configured_ssid[0] == '\0') {
        ESP_LOGW(TAG, "WIFI_NOT_CONFIGURED: edit ports/bouffalo/wifi_config.h and rebuild");
        ParkTask();
        return;
    }
    bool need_address = true, reset_station = false;
    bool probe_ready = false, ota_ready = false;
    TickType_t last_ota = 0;
    unsigned wifi_delay = kRetryFirstMs, service_delay = kRetryFirstMs;
    for (;;) {
        if (need_address) {
            if (parameters_rejected) {
                ESP_LOGW(TAG, "WIFI_RETRY_STOPPED: invalid parameters; fix wifi_config.h and rebuild");
                ParkTask();
                return;
            }
            if (reset_station && !ResetStation()) {
                RetryAfter("Wi-Fi cleanup", wifi_delay);
                continue;
            }
            const auto result = ConnectHotspot();
            if (result == ConnectResult::BadParameters) {
                ESP_LOGW(TAG, "WIFI_RETRY_STOPPED: invalid parameters; fix wifi_config.h and rebuild");
                ParkTask();
                return;
            }
            if (result != ConnectResult::Ready) {
                reset_station = true;
                RetryAfter("Wi-Fi association/DHCP", wifi_delay);
                continue;
            }
            need_address = false;
            reset_station = true;
            probe_ready = ota_ready = false;
            wifi_delay = service_delay = kRetryFirstMs;
        }
        if (!NetworkAvailable()) {
            ESP_LOGW(TAG, "WIFI_OFFLINE: old audio/session stopped; reconnecting hotspot");
            need_address = true;
            RetryAfter("Wi-Fi lost", wifi_delay);
            continue;
        }
        // Keep a healthy Wi-Fi connection when only the Internet/server fails.
        // Refresh OTA credentials no more than once a minute on session retries.
        if (!probe_ready)
            probe_ready = RunNetworkProbe();
        if (probe_ready && NetworkAvailable() &&
            (!ota_ready || xTaskGetTickCount() - last_ota >= pdMS_TO_TICKS(60000))) {
            ota_ready = RunOriginalOtaCheck();
            last_ota = xTaskGetTickCount();
        }
        if (probe_ready && ota_ready && NetworkAvailable()) {
            const TickType_t session_started = xTaskGetTickCount();
            const bool complete = RunOriginalWebsocketHandshake(0, NetworkAvailable);
            if (xTaskGetTickCount() - session_started >= pdMS_TO_TICKS(60000))
                service_delay = kRetryFirstMs;
            if (complete) {
                // Finite debug mode finishes normally. Do not start it repeatedly.
                WaitForLinkLoss();
            }
        }
        if (!NetworkAvailable())
            continue;
        RetryAfter(probe_ready ? (ota_ready ? "server session" : "OTA/activation") :
                   "DNS/time/HTTPS", service_delay);
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
