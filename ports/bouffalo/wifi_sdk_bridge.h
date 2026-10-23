#ifndef XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H
#define XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_SDK_DRIVER_READY,
    WIFI_SDK_MANAGER_READY,
    WIFI_SDK_ASSOCIATED,
    WIFI_SDK_GOT_IPV4,
    WIFI_SDK_DISCONNECTED,
    WIFI_SDK_LOST_IPV4,
    WIFI_SDK_DHCP_TIMEOUT,
    WIFI_SDK_PARAMETER_ERROR
} wifi_sdk_event_t;
typedef void (*wifi_sdk_callback_t)(wifi_sdk_event_t event, void* context);

// Single bootstrap subscriber. Its callback must not block.
int WifiSdkRegisterEvents(wifi_sdk_callback_t callback, void* context);
int WifiSdkStartDriver(void);
int WifiSdkStartManager(void);
// Submit one DHCP connection attempt with PMF optional and auto-reconnect disabled.
int WifiSdkConnect(const char* ssid, const char* password);

int WifiSdkGetStationMac(uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
#endif  // XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H
