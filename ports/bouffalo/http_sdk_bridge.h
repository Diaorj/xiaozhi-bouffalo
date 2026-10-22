#ifndef XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H
#define XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int status;
    int complete;
} http_sdk_response_t;

// Explicit CA is required. This synchronous normal-task call never disables TLS verification.
int HttpSdkVerifiedGet(const char* url, const char* ca_pem, size_t ca_len,
                       unsigned int timeout_ms, http_sdk_response_t* response);

#ifdef __cplusplus
}
#endif
#endif  // XIAOZHI_BOUFFALO_HTTP_SDK_BRIDGE_H
