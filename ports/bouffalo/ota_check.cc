#include "ota_check.h"
#include "ota_check_response.h"
#include "ota_ca.h"
#include "http_sdk_bridge.h"
#include "wifi_sdk_bridge.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <esp_log.h>

extern "C" {
#include "mbedtls/sha1.h"
#include "mbedtls/version.h"
}

namespace {
const char* const TAG = "OTA_CHECK";
// Same check endpoint as main/Kconfig.projbuild; this port has its own board identity.
constexpr char kUrl[] = "https://api.tenclass.net/xiaozhi/ota/";
constexpr char kBoard[] = "bouffalo-bl616cl-es8388";
constexpr char kVersion[] = "0.0.1";
constexpr size_t kResponseCapacity = 8193;
OtaCheckResponse response;

bool ReadIdentity(char (&mac_text)[18], char (&uuid)[37]) {
    uint8_t mac[6] = {};
    if (WifiSdkGetStationMac(mac) != 0 || (mac[0] & 1) != 0) {
        return false;
    }
    bool any_set = false;
    for (uint8_t byte : mac) {
        any_set = any_set || byte != 0;
    }
    if (!any_set) {
        return false;
    }
    std::snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // RFC 9562 UUIDv5: URL namespace plus a name derived from the real MAC.
    // Stable across reboot without flash writes; this is an identifier, not a secret.
    constexpr uint8_t url_namespace[16] = {
        0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    unsigned char input[96] = {};
    std::memcpy(input, url_namespace, sizeof(url_namespace));
    const int name_length = std::snprintf(reinterpret_cast<char*>(input + 16), sizeof(input) - 16,
                                           "xiaozhi-bouffalo/bl616cl/%s", mac_text);
    unsigned char hash[20] = {};
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(input) - 16) {
        return false;
    }
#if MBEDTLS_VERSION_MAJOR >= 3
    const int digest_result = mbedtls_sha1(input, 16 + name_length, hash);
#else
    const int digest_result = mbedtls_sha1_ret(input, 16 + name_length, hash);
#endif
    if (digest_result != 0) {
        return false;
    }
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    std::snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return true;
}

bool PrintableCode(const std::string& code) {
    if (code.empty() || code.size() > 16) {
        return false;
    }
    for (char ch : code) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z'))) {
            return false;
        }
    }
    return true;
}
}  // namespace

bool RunOtaCheck() {
    char mac[18] = {}, uuid[37] = {};
    if (!ReadIdentity(mac, uuid)) {
        ESP_LOGW(TAG, "OTA_CHECK_FAILED: real device identity unavailable");
        return false;
    }
    char device_header[40], client_header[56], agent_header[80];
    std::snprintf(device_header, sizeof(device_header), "Device-Id: %s\r\n", mac);
    std::snprintf(client_header, sizeof(client_header), "Client-Id: %s\r\n", uuid);
    std::snprintf(agent_header, sizeof(agent_header), "User-Agent: %s/%s\r\n", kBoard, kVersion);
    const char* headers[] = {
        "Connection: close\r\n", "Accept-Encoding: identity\r\n", "Activation-Version: 1\r\n",
        device_header, client_header, agent_header, "Accept-Language: zh-CN\r\n", nullptr};
    char payload[512];
    const int length = std::snprintf(payload, sizeof(payload),
        R"({"version":2,"language":"zh-CN","mac_address":"%s","uuid":"%s",)"
        R"("chip_model_name":"bl616cl","application":{"name":"xiaozhi-bouffalo","version":"%s"},)"
        R"("board":{"type":"%s","name":"%s","manufacturer":"Bouffalo Lab"}})",
        mac, uuid, kVersion, kBoard, kBoard);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload)) {
        ESP_LOGW(TAG, "OTA_CHECK_FAILED: device description too long");
        return false;
    }
    std::unique_ptr<char[]> body(new (std::nothrow) char[kResponseCapacity]);
    if (!body) {
        ESP_LOGW(TAG, "OTA_CHECK_FAILED: cannot allocate response buffer");
        return false;
    }
    const http_sdk_json_post_t request = {
        kUrl, ota_ca, sizeof(ota_ca), headers, payload, static_cast<size_t>(length)};
    http_sdk_response_t http = {};
    ESP_LOGI(TAG, "OTA_CHECK_BEGIN: %s; board=%s; version=%s", kUrl, kBoard, kVersion);
    const int result = HttpSdkVerifiedPostJson(&request, 10000, body.get(), kResponseCapacity, &http);
    if (result < 0 || !http.complete || http.overflow || http.status != 200) {
        ESP_LOGW(TAG, "OTA_CHECK_FAILED: ret=%d status=%u complete=%d overflow=%d",
                 result, http.status, http.complete, http.overflow);
        return false;
    }
    if (!response.Parse(body.get(), http.body_size)) {
        ESP_LOGW(TAG, "OTA_CHECK_FAILED: invalid JSON object");
        return false;
    }
    // Credentials are retained in RAM for the next transport step, never printed.
    const auto& activation = response.activation();
    ESP_LOGI(TAG, "OTA_CHECK_OK: activation=%u mqtt=%u websocket=%u",
             activation.has_code ? 1u : 0u, response.HasMqttConfig() ? 1u : 0u,
             response.HasWebsocketConfig() ? 1u : 0u);
    if (activation.has_code) {
        if (PrintableCode(activation.code)) {
            ESP_LOGI(TAG, "ACTIVATION_REQUIRED: code=%s; device is not activated yet",
                     activation.code.c_str());
        } else {
            ESP_LOGW(TAG, "ACTIVATION_REQUIRED: server code has an unsupported format");
        }
    }
    if (activation.has_challenge) {
        ESP_LOGI(TAG, "ACTIVATION_CHALLENGE_RECEIVED: activation polling is not started in this step");
    }
    ESP_LOGI(TAG, "OTA_CHECK_ONLY: no firmware download, flash update or conversation connection");
    return true;
}

const OtaCheckResponse& GetOtaCheckResponse() {
    return response;
}
