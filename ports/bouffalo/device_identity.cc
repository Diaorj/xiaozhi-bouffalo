#include "device_identity.h"
#include "wifi_sdk_bridge.h"
#include <cstdio>
#include <cstring>
extern "C" {
#include "mbedtls/sha1.h"
#include "mbedtls/version.h"
}
bool ReadIdentity(char (&mac_text)[18], char (&uuid)[37]) {
    uint8_t mac[6] = {};
    if (WifiSdkGetStationMac(mac) != 0 || (mac[0] & 1) != 0) {
        return false;
    }
    bool any_set = false;
    for (uint8_t byte : mac) {
        any_set = any_set || byte != 0;
    }
    if (!any_set) {
        return false;
    }
    std::snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // RFC 9562 UUIDv5: URL namespace plus a name derived from the real MAC.
    // Stable across reboot without flash writes; this is an identifier, not a secret.
    constexpr uint8_t url_namespace[16] = {
        0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1,
        0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    unsigned char input[96] = {};
    std::memcpy(input, url_namespace, sizeof(url_namespace));
    const int name_length = std::snprintf(reinterpret_cast<char*>(input + 16), sizeof(input) - 16,
                                           "xiaozhi-bouffalo/bl616cl/%s", mac_text);
    unsigned char hash[20] = {};
    if (name_length < 0 || static_cast<size_t>(name_length) >= sizeof(input) - 16) {
        return false;
    }
#if MBEDTLS_VERSION_MAJOR >= 3
    const int digest_result = mbedtls_sha1(input, 16 + name_length, hash);
#else
    const int digest_result = mbedtls_sha1_ret(input, 16 + name_length, hash);
#endif
    if (digest_result != 0) {
        return false;
    }
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    std::snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
        hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    return true;
}
