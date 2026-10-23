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


typedef struct {
    http_sdk_response_t* result;
    char* body;
    size_t capacity;
} post_context_t;

static void OnJsonResponse(struct http_response* response, enum http_final_call final,
                            void* context) {
    post_context_t* post = context;
    http_sdk_response_t* result = post->result;
    result->status = response->http_status_code;
    if (response->body_frag_len > 0 && !result->overflow) {
        if (response->body_frag_start == NULL ||
            response->body_frag_len > post->capacity - 1 - result->body_size) {
            result->overflow = 1;
        } else {
            memcpy(post->body + result->body_size, response->body_frag_start,
                   response->body_frag_len);
            result->body_size += response->body_frag_len;
            post->body[result->body_size] = '\0';
        }
    }
    if (final == HTTP_DATA_FINAL) {
        result->complete = !result->overflow && response->message_complete != 0;
    }
}

static int CancelOversizedResponse(void* context) {
    return ((post_context_t*)context)->result->overflow;
}

int HttpSdkVerifiedPostJson(const http_sdk_json_post_t* input, unsigned int timeout_ms,
                            char* body, size_t capacity, http_sdk_response_t* response) {
    if (response == NULL) {
        return -1;
    }
    memset(response, 0, sizeof(*response));
    if (input == NULL || input->url == NULL || strncmp(input->url, "https://", 8) != 0 ||
        input->ca_pem == NULL || input->ca_len == 0 || input->payload == NULL ||
        input->payload_len == 0 || timeout_ms == 0 || body == NULL || capacity < 2) {
        return -1;
    }
    body[0] = '\0';
    post_context_t context = {response, body, capacity};
    struct https_client_request request = {0};
    request.method = HTTP_POST;
    request.url = input->url;
    request.protocol = "HTTP/1.1";
    request.header_fields = input->headers;
    request.content_type_value = "application/json";
    request.payload = input->payload;
    request.payload_len = input->payload_len;
    request.response = OnJsonResponse;
    request.cancel = CancelOversizedResponse;
    request.ca_pem = input->ca_pem;
    request.ca_len = input->ca_len;
    request.buffer_size = 1024;
    const int result = https_client_request(&request, timeout_ms, &context);
    return response->overflow ? -1 : result;
}
