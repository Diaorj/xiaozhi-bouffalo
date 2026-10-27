#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// One SSL context is serialized by BouffaloSsl. No SDK headers leak into C++.
void* TlsSdkConnect(const char* host, int port);
int TlsSdkRead(void* handle, void* output, size_t capacity);  // -2: retry
int TlsSdkWrite(void* handle, const void* data, size_t size);
void TlsSdkClose(void* handle);
int WebsocketRandom(void* output, size_t size);
int WebsocketSha1(const void* data, size_t size, unsigned char output[20]);
#ifdef __cplusplus
}
#endif
