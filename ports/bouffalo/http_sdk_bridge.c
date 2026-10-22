#include "http_sdk_bridge.h"

#include <string.h>

// SDK http/misc/misc.h has an unmatched C++ linkage closing brace.
// Keep those SDK headers in C until that header is corrected upstream.
#include "https_client.h"

static void OnResponse(struct http_response* response, enum http_final_call final, void* context) {
    http_sdk_response_t* result = context;
    result->status = response->http_status_code;
    if (final == HTTP_DATA_FINAL) {
        result->complete = response->message_complete != 0;
    }
}

int HttpSdkVerifiedGet(const char* url, const char* ca_pem, size_t ca_len,
                       unsigned int timeout_ms, http_sdk_response_t* response) {
    if (response == NULL) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (url == NULL || strncmp(url, "https://", 8) != 0 || ca_pem == NULL || ca_len == 0 ||
        timeout_ms == 0) {
        return -1;
    }
    const char* headers[] = {"Connection: close\r\n", "Accept-Encoding: identity\r\n", NULL};
    struct https_client_request request = {0};
    request.method = HTTP_GET;
    request.url = url;
    request.protocol = "HTTP/1.1";
    request.header_fields = headers;
    request.response = OnResponse;
    request.ca_pem = ca_pem;
    request.ca_len = ca_len;
    request.buffer_size = 1024;
    return https_client_request(&request, timeout_ms, response);
}
