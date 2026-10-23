#ifndef XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H
#define XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int status;
    int complete;
    size_t body_size;
    int overflow;
} http_sdk_response_t;

// Explicit CA is required. This synchronous normal-task call never disables TLS verification.
int HttpSdkVerifiedGet(const char* url, const char* ca_pem, size_t ca_len,
                       unsigned int timeout_ms, http_sdk_response_t* response);

typedef struct {
    const char* url;
    const char* ca_pem;
    size_t ca_len;
    const char** headers;  // NULL-terminated; each line includes CRLF.
    const char* payload;
    size_t payload_len;
} http_sdk_json_post_t;

// Body is NUL-terminated. Oversized responses are cancelled, never truncated as success.
int HttpSdkVerifiedPostJson(const http_sdk_json_post_t* request, unsigned int timeout_ms,
                            char* body, size_t capacity, http_sdk_response_t* response);

#ifdef __cplusplus
}
#endif
#endif  // XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H
