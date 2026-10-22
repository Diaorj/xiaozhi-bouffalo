#include "wifi_sdk_bridge.h"

#include <stddef.h>
#include <string.h>

#include "async_event.h"
#include "fhost_api.h"
#include "macsw_plat.h"
#include "wifi_mgmr.h"
#include "wifi_mgmr_ext.h"

// Keep SDK headers that are not C++ compatible inside this C translation unit.
static wifi_sdk_callback_t callback;
static void* callback_context;

static void ForwardEvent(async_input_event_t event, void* context) {
    (void)context;
    if (event == NULL) {
        return;
    }
    switch (event->code) {
        case CODE_WIFI_ON_INIT_DONE:
            callback(WIFI_SDK_DRIVER_READY, callback_context);
            break;
        case CODE_WIFI_ON_MGMR_DONE:
            callback(WIFI_SDK_MANAGER_READY, callback_context);
            break;
        case CODE_WIFI_ON_CONNECTED:
            callback(WIFI_SDK_ASSOCIATED, callback_context);
            break;
        case CODE_WIFI_ON_GOT_IP:
            callback(WIFI_SDK_GOT_IPV4, callback_context);
            break;
        case CODE_WIFI_ON_DISCONNECT:
            callback(WIFI_SDK_DISCONNECTED, callback_context);
            break;
        case CODE_WIFI_ON_LOST_IP:
            callback(WIFI_SDK_LOST_IPV4, callback_context);
            break;
        case CODE_WIFI_ON_GOT_IP_TIMEOUT:
        case CODE_WIFI_ON_GOT_IP_ABORT:
            callback(WIFI_SDK_DHCP_TIMEOUT, callback_context);
            break;
        case CODE_WIFI_ON_PARAMS_ERROR:
            callback(WIFI_SDK_PARAMETER_ERROR, callback_context);
            break;
        default:
            break;
    }
}

int WifiSdkRegisterEvents(wifi_sdk_callback_t listener, void* context) {
    if (listener == NULL || callback != NULL) {
        return -1;
    }
    callback = listener;
    callback_context = context;
    const int result = async_register_event_filter(EV_WIFI, ForwardEvent, NULL);
    if (result != 0) {
        callback = NULL;
        callback_context = NULL;
    }
    return result;
}

int WifiSdkStartDriver(void) {
    wifi_task_create();
    return fhost_init();
}

int WifiSdkStartManager(void) {
    return wifi_mgmr_task_start();
}

int WifiSdkConnect(const char* ssid, const char* password) {
    // Keep storage valid while the SDK processes its asynchronous request.
    static wifi_mgmr_sta_connect_params_t params;
    if (ssid == NULL || password == NULL) {
        return -1;
    }
    const size_t ssid_len = strlen(ssid);
    const size_t key_len = strlen(password);
    if (ssid_len == 0 || ssid_len > sizeof(params.ssid) || key_len > sizeof(params.key)) {
        return -1;
    }
    memset(&params, 0, sizeof(params));
    memcpy(params.ssid, ssid, ssid_len);
    memcpy(params.key, password, key_len);
    params.ssid_len = ssid_len;
    params.key_len = key_len;
    params.use_dhcp = 1;
    // Match the working SDK Shell defaults: 0 disabled, 1 optional, 2 required.
    params.pmf_cfg = 1;
    params.timeout_ms = 30000;
    params.dhcp_timeout_event_only = 1;
    const int result = wifi_mgmr_sta_autoconnect_disable();
    return result == 0 ? wifi_mgmr_sta_connect(&params) : result;
}
