#ifndef XIAOZHI_OTA_CHECK_RESPONSE_H
#define XIAOZHI_OTA_CHECK_RESPONSE_H

#include <cstddef>
#include <memory>
#include <string>
#include <cJSON.h>

// Platform-independent portion of Ota::CheckVersion's response handling.
struct OtaActivationInfo {
    std::string message;
    std::string code;
    std::string challenge;
    int timeout_ms = 30000;
    bool has_message = false;
    bool has_code = false;
    bool has_challenge = false;
    bool has_timeout = false;
};

class OtaCheckResponse {
public:
    bool Parse(const char* data, size_t length);
    const cJSON* root() const { return root_.get(); }
    const OtaActivationInfo& activation() const { return activation_; }
    bool HasMqttConfig() const;
    bool HasWebsocketConfig() const;

private:
    struct JsonDeleter {
        void operator()(cJSON* value) const { cJSON_Delete(value); }
    };
    std::unique_ptr<cJSON, JsonDeleter> root_;
    OtaActivationInfo activation_;
};

#endif  // XIAOZHI_OTA_CHECK_RESPONSE_H
