#ifndef XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H
#define XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WIFI_SDK_DRIVER_READY, WIFI_SDK_MANAGER_READY } wifi_sdk_event_t;
typedef void (*wifi_sdk_callback_t)(wifi_sdk_event_t event, void* context);

// Single bootstrap subscriber. Its callback must not block.
int WifiSdkRegisterEvents(wifi_sdk_callback_t callback, void* context);
int WifiSdkStartDriver(void);
int WifiSdkStartManager(void);

#ifdef __cplusplus
}
#endif
#endif  // XIAOZHI_BOUFFALO_WIFI_SDK_BRIDGE_H
