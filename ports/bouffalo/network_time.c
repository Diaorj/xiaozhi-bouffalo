/* Minimal SNTP clock bootstrap for certificate validity checks.
 * SNTP here is unauthenticated; production needs a trusted time source. */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "bflb_mtimer.h"
#include "network_time.h"

#define DBG_TAG "NETWORK_TIME"
#include "log.h"

#define NTP_UNIX_OFFSET 2208988800UL
#define NTP_PACKET_SIZE 48

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Decode only a matching, synchronized server reply from NTP era 0. */
static int decode_sntp_reply(const uint8_t *response, size_t length,
                             const uint8_t *request, uint32_t *unix_seconds)
{
    uint32_t seconds;
    if (length != NTP_PACKET_SIZE || (response[0] & 7) != 4 ||
        (response[0] >> 6) == 3 ||
        (((response[0] >> 3) & 7) != 3 && ((response[0] >> 3) & 7) != 4) ||
        response[1] == 0 || response[1] > 15 ||
        memcmp(response + 24, request + 40, 8) != 0) {
        return -1;
    }
    seconds = read_be32(response + 40);
    if (seconds < NTP_UNIX_OFFSET) {
        return -1;
    }
    seconds -= NTP_UNIX_OFFSET;
    if (seconds < NETWORK_TIME_MIN || seconds > NETWORK_TIME_MAX) {
        return -1;
    }
    *unix_seconds = seconds;
    return 0;
}

/* Read the SDK RTC-backed clock without depending on calendar conversions. */
static time_t network_time_now(void)
{
    struct timeval now = {0};
    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }
    return now.tv_sec;
}

static int network_set_time(uint32_t seconds)
{
    struct timeval now = {0};
    if (seconds < NETWORK_TIME_MIN || seconds > NETWORK_TIME_MAX) {
        LOG_E("Clock must be in the supported 2024-2036 range\r\n");
        return -1;
    }
    now.tv_sec = (time_t)seconds;
    if (settimeofday(&now, NULL) != 0) {
        LOG_E("Could not set the system clock\r\n");
        return -1;
    }
    LOG_I("TIME_READY: Unix UTC seconds=%lu\r\n", (unsigned long)network_time_now());
    return 0;
}

static int network_sntp(const char *server)
{
    struct addrinfo hints = {0};
    struct addrinfo *address = NULL;
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    uint8_t request[NTP_PACKET_SIZE] = {0};
    uint8_t response[NTP_PACKET_SIZE];
    uint64_t cookie = bflb_mtimer_get_time_us();
    uint32_t seconds;
    fd_set readable;
    int socket_fd = -1;
    int ret = -1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    LOG_I("SNTP request: %s\r\n", server);
    if (getaddrinfo(server, "123", &hints, &address) != 0 || address == NULL) {
        LOG_W("SNTP DNS lookup failed\r\n");
        goto done;
    }
    socket_fd = socket(address->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0 || socket_fd >= FD_SETSIZE ||
        connect(socket_fd, address->ai_addr, address->ai_addrlen) != 0) {
        goto done;
    }
    request[0] = 0x23; /* LI=0, version=4, client mode=3 */
    /* An opaque transmit timestamp is echoed as the response's origin.
     * Match it before accepting a reply; this does not authenticate SNTP. */
    for (unsigned int i = 0; i < 8; i++) {
        request[40 + i] = (uint8_t)(cookie >> (56 - i * 8));
    }
    if (send(socket_fd, request, sizeof(request), 0) != sizeof(request)) {
        goto done;
    }
    FD_ZERO(&readable);
    FD_SET(socket_fd, &readable);
    if (select(socket_fd + 1, &readable, NULL, NULL, &timeout) <= 0) {
        LOG_W("SNTP reply timeout\r\n");
        goto done;
    }
    int received = recv(socket_fd, response, sizeof(response), MSG_DONTWAIT);
    if (received < 0 || decode_sntp_reply(response, (size_t)received, request, &seconds) != 0) {
        LOG_W("Invalid or unsynchronized SNTP reply\r\n");
        goto done;
    }
    ret = network_set_time(seconds);

done:
    if (socket_fd >= 0) {
        closesocket(socket_fd);
    }
    if (address != NULL) {
        freeaddrinfo(address);
    }
    return ret;
}

int NetworkPrepareTime(void)
{
    const char *servers[] = {"ntp1.aliyun.com", "time.cloudflare.com"};
    time_t now = network_time_now();
    if (now >= (time_t)NETWORK_TIME_MIN && now <= (time_t)NETWORK_TIME_MAX) {
        LOG_I("TIME_READY: existing system clock\r\n");
        return 0;
    }
    LOG_I("TIME_SYNC_BEGIN: SNTP clock bootstrap\r\n");
    for (unsigned int i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        if (network_sntp(servers[i]) == 0) {
            return 0;
        }
    }
    LOG_E("TIME_SYNC_FAILED: no valid clock; HTTPS check skipped\r\n");
    return -1;
}
