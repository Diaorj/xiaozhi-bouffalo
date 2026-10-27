#include "tls_sdk_bridge.h"
#include "https_wrapper.h"
#include "ota_ca.h"
#include "bflb_sec_trng.h"
#include "lwip/sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/version.h"
#include <errno.h>
#include <limits.h>
#include <sys/time.h>

void* TlsSdkConnect(const char* host, int port) {
    if (!host || !*host || port <= 0 || port > 65535) return NULL;
    http_wrapper_ssl_param_t parameters = {0};
    // Explicit trust anchor: SDK's null-CA fallback can disable verification.
    parameters.ca_cert = ota_ca;
    parameters.ca_cert_len = sizeof(ota_ca);
    parameters.sni = (char*)host;
    return https_wrapper_connect(host, (uint16_t)port, &parameters);
}
int TlsSdkRead(void* handle, void* output, size_t capacity) {
    if (!handle || !output || !capacity || capacity > UINT32_MAX) return -1;
    // The SDK leaves this socket nonblocking after the TLS handshake. A partial
    // TLS record must yield WANT_READ, never hold the I/O mutex waiting for bytes.
    int result = https_wrapper_recv(handle, output, (uint32_t)capacity, 0);
    return result == -EAGAIN ? -2 : result;
}
int TlsSdkWrite(void* handle, const void* data, size_t size) {
    if (!handle || !data || !size || size > 4096) return -1;
    const int fd = https_wrapper_socketfd_get(handle);
    struct timeval timeout = {5, 0};
    int blocking = 0, nonblocking = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ||
        ioctlsocket(fd, FIONBIO, &blocking) < 0) return -1;
    // SDK BIO send maps EAGAIN to an error; bounded blocking avoids that path.
    const int result = https_wrapper_send(handle, data, (uint16_t)size, 0);
    if (ioctlsocket(fd, FIONBIO, &nonblocking) < 0) return -1;
    return result;
}
void TlsSdkClose(void* handle) { if (handle) https_wrapper_destroy(handle); }
int WebsocketRandom(void* output, size_t size) {
    if (!output || size > UINT32_MAX) return -1;
    return bflb_trng_readlen(output, (uint32_t)size);
}
int WebsocketSha1(const void* data, size_t size, unsigned char output[20]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_sha1(data, size, output);
#else
    return mbedtls_sha1_ret(data, size, output);
#endif
}
