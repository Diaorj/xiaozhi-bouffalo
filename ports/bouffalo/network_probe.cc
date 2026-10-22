#include "network_probe.h"
#include "network_ca.h"
#include "http_sdk_bridge.h"
#include "network_time.h"

#include <esp_log.h>

extern "C" {
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "mbedtls/build_info.h"
#include "mbedtls/x509_crt.h"
}

#if !defined(MBEDTLS_HAVE_TIME_DATE)
#error "The HTTPS probe requires certificate date verification"
#endif

namespace {
const char* const TAG = "NETWORK_PROBE";
const char* const kHost = "example.com";
const char* const kUrl = "https://example.com/";
constexpr unsigned int kHttpTimeoutMs = 10000;

bool ResolveHost() {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* address = nullptr;
    ESP_LOGI(TAG, "DNS_BEGIN: %s", kHost);
    const int result = getaddrinfo(kHost, "443", &hints, &address);
    char ip[16] = {};
    const bool valid = result == 0 && address != nullptr && address->ai_family == AF_INET &&
                       address->ai_addr != nullptr && address->ai_addrlen >= sizeof(sockaddr_in);
    const bool converted = valid && inet_ntop(AF_INET,
        &reinterpret_cast<const sockaddr_in*>(address->ai_addr)->sin_addr, ip, sizeof(ip)) != nullptr;
    if (address != nullptr) {
        freeaddrinfo(address);
    }
    if (!converted) {
        ESP_LOGW(TAG, "DNS_FAILED: %s ret=%d", kHost, result);
        return false;
    }
    ESP_LOGI(TAG, "DNS_OK: %s -> %s", kHost, ip);
    return true;
}
}  // namespace

bool RunNetworkProbe() {
    if (!ResolveHost() || NetworkPrepareTime() != 0) {
        return false;
    }
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    const int parse = mbedtls_x509_crt_parse(&ca,
        reinterpret_cast<const unsigned char*>(network_ca), sizeof(network_ca));
    mbedtls_x509_crt_free(&ca);
    if (parse != 0) {
        ESP_LOGW(TAG, "HTTPS_FAILED: CA parse ret=%d", parse);
        return false;
    }

    http_sdk_response_t response = {};
    ESP_LOGI(TAG, "HTTPS_BEGIN: %s; verify CA, hostname and certificate dates", kUrl);
    // The SDK timeout covers HTTP transfer; DNS/TCP/TLS use their own limits.
    const int result = HttpSdkVerifiedGet(kUrl, network_ca, sizeof(network_ca),
                                          kHttpTimeoutMs, &response);
    if (result < 0 || !response.complete || response.status < 200 || response.status >= 300) {
        ESP_LOGW(TAG, "HTTPS_FAILED: ret=%d status=%u complete=%u; inspect SDK TLS/HTTP logs",
                 result, response.status, response.complete ? 1u : 0u);
        return false;
    }
    ESP_LOGI(TAG, "HTTPS_OK: status=%u; verified response received", response.status);
    return true;
}
