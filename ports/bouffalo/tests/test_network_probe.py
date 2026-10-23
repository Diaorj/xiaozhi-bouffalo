"""Compile the real C++ probe and C HTTP adapter with deterministic SDK mocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]
HTTP = r"""
#ifndef MOCK_HTTPS_CLIENT_H
#define MOCK_HTTPS_CLIENT_H
#include <stddef.h>
#include <stdint.h>
enum http_method { HTTP_GET, HTTP_POST };
enum http_final_call { HTTP_DATA_MORE, HTTP_DATA_FINAL };
struct http_response { unsigned http_status_code; int message_complete; unsigned char* body_frag_start; size_t body_frag_len; };
typedef void (*http_response_cb_t)(struct http_response*, enum http_final_call, void*);
struct https_client_request {
    enum http_method method;
    const char *url, *protocol;
    const char **header_fields;
    http_response_cb_t response;
    const char* ca_pem;
    size_t ca_len, buffer_size;
    const char *content_type_value, *payload;
    size_t payload_len;
    int (*cancel)(void*);
};
int https_client_request(const struct https_client_request*, uint32_t, void*);
#endif
"""
X509 = r"""
#include <stddef.h>
typedef struct { int initialized; } mbedtls_x509_crt;
void mbedtls_x509_crt_init(mbedtls_x509_crt*);
int mbedtls_x509_crt_parse(mbedtls_x509_crt*, const unsigned char*, size_t);
void mbedtls_x509_crt_free(mbedtls_x509_crt*);
"""
HARNESS = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "network_probe.h"
#include "network_time.h"
#include "http_sdk_bridge.h"
extern "C" {
#include "https_client.h"
#include "mbedtls/x509_crt.h"
#include <netdb.h>
#include <arpa/inet.h>
}
static int mode, queries, clocks, parses, requests;
static sockaddr_in ip;
static addrinfo address;
extern "C" {
int getaddrinfo(const char* host, const char* service, const addrinfo* hints, addrinfo** out) {
    ++queries;
    assert(std::strcmp(host, "example.com") == 0 && std::strcmp(service, "443") == 0);
    assert(hints->ai_family == AF_INET && hints->ai_socktype == SOCK_STREAM);
    *out = nullptr;
    if (mode == 1) return EAI_FAIL;
    if (mode == 10) return 0;
    ip.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "93.184.216.34", &ip.sin_addr) == 1);
    address.ai_family = AF_INET;
    address.ai_addrlen = sizeof(ip);
    address.ai_addr = mode == 12 ? nullptr : reinterpret_cast<sockaddr*>(&ip);
    *out = &address;
    return 0;
}
void freeaddrinfo(addrinfo* ptr) { assert(ptr == &address); }
int NetworkPrepareTime() { ++clocks; return mode == 2 ? -1 : 0; }
void mbedtls_x509_crt_init(mbedtls_x509_crt* ca) { ca->initialized = 1; }
int mbedtls_x509_crt_parse(mbedtls_x509_crt* ca, const unsigned char* pem, size_t length) {
    ++parses;
    assert(ca->initialized && std::strstr(reinterpret_cast<const char*>(pem), "BEGIN CERTIFICATE"));
    assert(length == std::strlen(reinterpret_cast<const char*>(pem)) + 1);
    return mode == 3 ? -1 : 0;
}
void mbedtls_x509_crt_free(mbedtls_x509_crt* ca) { assert(ca->initialized); }
int https_client_request(const struct https_client_request* req, uint32_t timeout, void* ctx) {
    ++requests;
    assert(req->method == HTTP_GET && std::strcmp(req->url, "https://example.com/") == 0);
    assert(req->ca_pem && req->ca_len > 0 && timeout == 10000);
    assert(std::strstr(req->ca_pem, "BEGIN CERTIFICATE"));
    assert(req->buffer_size == 1024 && std::strcmp(req->protocol, "HTTP/1.1") == 0);
    assert(std::strcmp(req->header_fields[0], "Connection: close\r\n") == 0);
    if (mode == 4) return -1;
    if (mode == 8) return 0;
    http_response response{200, 1, nullptr, 0};
    if (mode == 5) response.message_complete = 0;
    if (mode == 6) response.http_status_code = 500;
    if (mode == 7) response.http_status_code = 301;
    if (mode == 9) req->response(&response, HTTP_DATA_MORE, ctx);
    req->response(&response, HTTP_DATA_FINAL, ctx);
    return mode == 11 ? -1 : 10;
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    mode = std::atoi(argv[1]);
    if (mode == 13) {
        http_sdk_response_t response;
        assert(HttpSdkVerifiedGet("http://example.com/", "CA", 3, 10000, &response) < 0);
        assert(HttpSdkVerifiedGet("https://example.com/", nullptr, 0, 10000, &response) < 0);
        assert(HttpSdkVerifiedGet(nullptr, "CA", 3, 10000, &response) < 0);
        assert(HttpSdkVerifiedGet("https://example.com/", "CA", 3, 10000, nullptr) < 0);
        assert(requests == 0);
        return 0;
    }
    const bool ok = RunNetworkProbe();
    assert(ok == (mode == 0 || mode == 9));
    assert(queries == 1);
    const bool dns_ok = mode != 1 && mode != 10 && mode != 12;
    assert(clocks == (dns_ok ? 1 : 0));
    assert(parses == (dns_ok && mode != 2 ? 1 : 0));
    assert(requests == (dns_ok && mode != 2 && mode != 3 ? 1 : 0));
}
"""


class NetworkProbeTests(unittest.TestCase):
    def test_verified_https_and_failure_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "lwip").mkdir()
            (work / "mbedtls").mkdir()
            (work / "lwip/netdb.h").write_text('#include <netdb.h>\n')
            (work / "lwip/inet.h").write_text('#include <arpa/inet.h>\n')
            (work / "https_client.h").write_text(HTTP)
            (work / "mbedtls/build_info.h").write_text('#define MBEDTLS_HAVE_TIME_DATE 1\n')
            (work / "mbedtls/x509_crt.h").write_text(X509)
            (work / "esp_log.h").write_text(
                '#include <stdio.h>\n'
                '#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\\n", tag, ##__VA_ARGS__)\n'
                '#define ESP_LOGW ESP_LOGI\n')
            (work / "check.cc").write_text(HARNESS)
            includes = ["-I" + str(work), "-I" + str(PORT)]
            subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", *includes,
                            "-c", str(PORT / "http_sdk_bridge.c"), "-o", str(work / "http.o")],
                           check=True)
            executable = work / "check"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *includes,
                            str(PORT / "network_probe.cc"), str(work / "check.cc"),
                            str(work / "http.o"), "-o", str(executable)], check=True)
            for mode in range(14):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), str(mode)], check=True,
                                            text=True, capture_output=True, timeout=5)
                    self.assertEqual("HTTPS_OK:" in result.stdout, mode in (0, 9))


class NetworkTimeTests(unittest.TestCase):
    def test_sntp_reply_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "lwip").mkdir()
            (work / "lwip/netdb.h").write_text('#include <netdb.h>\n')
            (work / "lwip/sockets.h").write_text(
                '#include <sys/socket.h>\n#include <sys/select.h>\n#include <unistd.h>\n'
                '#define closesocket close\n')
            (work / "bflb_mtimer.h").write_text(
                '#include <stdint.h>\nuint64_t bflb_mtimer_get_time_us(void);\n')
            (work / "log.h").write_text(
                '#define LOG_I(...) ((void)0)\n#define LOG_W(...) ((void)0)\n'
                '#define LOG_E(...) ((void)0)\n')
            (work / "time_check.c").write_text(r"""
#include <assert.h>
#include "network_time.c"
static void put32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24); out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8); out[3] = (uint8_t)value;
}
int main(void) {
    uint8_t request[48] = {0}, valid[48] = {0}, reply[48];
    uint32_t seconds = 0;
    for (unsigned i = 0; i < 8; ++i) request[40+i] = (uint8_t)(i+1);
    valid[0] = 0x24; valid[1] = 2;
    memcpy(valid + 24, request + 40, 8);
    put32(valid + 40, 1780000000UL + NTP_UNIX_OFFSET);
    assert(decode_sntp_reply(valid, 48, request, &seconds) == 0 && seconds == 1780000000UL);
    for (unsigned mode = 0; mode < 9; ++mode) {
        memcpy(reply, valid, 48);
        size_t length = 48;
        switch (mode) {
            case 0: length = 47; break;
            case 1: reply[0] = 0x23; break; /* client, not server */
            case 2: reply[0] |= 0xc0; break; /* unsynchronized */
            case 3: reply[0] = 0x14; break; /* unsupported version */
            case 4: reply[1] = 0; break; /* kiss-of-death */
            case 5: reply[1] = 16; break;
            case 6: reply[24] ^= 1; break; /* wrong request */
            case 7: put32(reply + 40, 1); break; /* era wrap */
            case 8: put32(reply + 40, NTP_UNIX_OFFSET + NETWORK_TIME_MIN - 1); break;
        }
        seconds = 123;
        assert(decode_sntp_reply(reply, length, request, &seconds) != 0 && seconds == 123);
    }
    put32(valid + 40, NTP_UNIX_OFFSET + NETWORK_TIME_MIN);
    assert(decode_sntp_reply(valid, 48, request, &seconds) == 0 && seconds == NETWORK_TIME_MIN);
    put32(valid + 40, NTP_UNIX_OFFSET + NETWORK_TIME_MAX);
    assert(decode_sntp_reply(valid, 48, request, &seconds) == 0 && seconds == NETWORK_TIME_MAX);
    return 0;
}
""")
            executable = work / "time_check"
            subprocess.run(["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I" + str(work), "-I" + str(PORT), str(work / "time_check.c"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
