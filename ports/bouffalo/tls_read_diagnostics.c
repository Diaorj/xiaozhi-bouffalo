#include <errno.h>
#include <stdio.h>
#include "mbedtls/ssl.h"
#include "memory_diagnostics.h"

int __real_mbedtls_ssl_read(mbedtls_ssl_context* ssl, unsigned char* data, size_t size);

// The SDK HTTPS wrapper collapses fatal TLS read errors to -1. Record the real
// cause before that conversion, without changing the TLS result or errno.
int __wrap_mbedtls_ssl_read(mbedtls_ssl_context* ssl, unsigned char* data, size_t size) {
    const int result = __real_mbedtls_ssl_read(ssl, data, size);
    const int saved_errno = errno;
    if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        printf("[W][TLS] TLS_READ_CLOSED: ret=%d errno=%d\r\n", result, saved_errno);
    } else if (result < 0 && result != MBEDTLS_ERR_SSL_WANT_READ) {
        printf("[E][TLS] TLS_READ_ERROR: ret=%d hex=0x%x errno=%d\r\n", result,
               (unsigned)(-result), saved_errno);
        MemoryDiagnosticsStage("tls_read_error");
    }
    errno = saved_errno;
    return result;
}
