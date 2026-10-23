#include "ota_check_response.h"

namespace {
bool ReadString(const cJSON* object, const char* key, std::string& value) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    value = item->valuestring;
    return true;
}
}  // namespace

bool OtaCheckResponse::Parse(const char* data, size_t length) {
    root_.reset();
    activation_ = {};
    if (data == nullptr || length == 0) {
        return false;
    }
    const char* end = nullptr;
    std::unique_ptr<cJSON, JsonDeleter> parsed(
        cJSON_ParseWithLengthOpts(data, length, &end, false));
    if (!cJSON_IsObject(parsed.get()) || end == nullptr) {
        return false;
    }
    // Accept one JSON document only, with optional JSON whitespace after it.
    for (const char* cursor = end; cursor < data + length; ++cursor) {
        if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            return false;
        }
    }
    const cJSON* activation = cJSON_GetObjectItem(parsed.get(), "activation");
    if (cJSON_IsObject(activation)) {
        activation_.has_message = ReadString(activation, "message", activation_.message);
        activation_.has_code = ReadString(activation, "code", activation_.code);
        activation_.has_challenge = ReadString(activation, "challenge", activation_.challenge);
        const cJSON* timeout = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout)) {
            activation_.timeout_ms = timeout->valueint;
            activation_.has_timeout = true;
        }
    }
    root_ = std::move(parsed);
    return true;
}

bool OtaCheckResponse::HasMqttConfig() const {
    return cJSON_IsObject(cJSON_GetObjectItem(root_.get(), "mqtt"));
}

bool OtaCheckResponse::HasWebsocketConfig() const {
    return cJSON_IsObject(cJSON_GetObjectItem(root_.get(), "websocket"));
}
