#include "ota_bootstrap.h"
#include "ota.h"
#include "settings.h"
#include "settings_storage.h"
#include <esp_log.h>
bool RunOriginalOtaCheck() {
    if (!InitializeSettingsStorage()) return false;
    const bool cached_mqtt = !Settings("mqtt").GetString("endpoint").empty();
    const bool cached_ws = !Settings("websocket").GetString("url").empty();
    ESP_LOGI("OTA", "SETTINGS_LOADED: mqtt=%u websocket=%u", cached_mqtt ? 1u : 0u, cached_ws ? 1u : 0u);
    Ota ota;
    ESP_LOGI("OTA", "ORIGINAL_OTA_BEGIN: calling Ota::CheckVersion()");
    const auto result = ota.CheckVersion();
    if (!result || !SettingsStorageHealthy()) {
        ESP_LOGE("OTA", "ORIGINAL_OTA_FAILED: %s", result ? "persistent settings" : result.error().ToString().c_str());
        return false;
    }
    ESP_LOGI("OTA", "ORIGINAL_OTA_OK: activation=%u mqtt=%u websocket=%u server_time=%u",
        ota.HasActivationCode() ? 1u : 0u, ota.HasMqttConfig() ? 1u : 0u,
        ota.HasWebsocketConfig() ? 1u : 0u, ota.HasServerTime() ? 1u : 0u);
    if (ota.HasActivationCode()) {
        const auto& code = ota.GetActivationCode();
        if (!code.empty() && code.size() <= 16 && code.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos)
            ESP_LOGI("OTA", "ACTIVATION_REQUIRED: code=%s", code.c_str());
    }
    ESP_LOGI("OTA", "SETTINGS_SAVED: SDK LittleFS KV; firmware upgrade disabled");
    // Do not use stale credentials while this OTA response requests activation.
    return !ota.HasActivationCode();
}
