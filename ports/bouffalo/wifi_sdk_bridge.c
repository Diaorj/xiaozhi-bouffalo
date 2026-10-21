#include "wifi_sdk_bridge.h"

#include <stddef.h>

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
