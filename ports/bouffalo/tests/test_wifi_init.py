"""Exercise the real C bridge/C++ init flow using host SDK mocks, not radio hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]
FREERTOS = r"""
#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H
#include <assert.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
#endif
"""
TASK = r"""
#include "FreeRTOS.h"
enum { eSetBits };
BaseType_t xTaskNotify(TaskHandle_t, uint32_t, int);
BaseType_t xTaskNotifyWait(uint32_t, uint32_t, uint32_t*, TickType_t);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*);
"""
SDK = r"""
#ifndef MOCK_SDK_H
#define MOCK_SDK_H
#include <stddef.h>
#include <stdint.h>
typedef struct { unsigned code; } mock_event;
typedef mock_event* async_input_event_t;
typedef void (*async_event_cb)(async_input_event_t, void*);
#define EV_WIFI 2
#define CODE_WIFI_ON_INIT_DONE 1
#define CODE_WIFI_ON_MGMR_DONE 2
#define CODE_WIFI_ON_CONNECTED 4
#define CODE_WIFI_ON_DISCONNECT 5
#define CODE_WIFI_ON_GOT_IP 7
#define CODE_WIFI_ON_LOST_IP 26
#define CODE_WIFI_ON_GOT_IP_TIMEOUT 28
#define CODE_WIFI_ON_GOT_IP_ABORT 29
#define CODE_WIFI_ON_PARAMS_ERROR 32
#define ERR_OK 0
#define ERR_MEM (-1)
typedef struct {
    char ssid[32];
    char ssid_tail[1];
    char key[64];
    char key_tail[1];
    uint8_t ssid_len, key_len, pmf_cfg, use_dhcp, dhcp_timeout_event_only;
    int timeout_ms;
} wifi_mgmr_sta_connect_params_t;
int wifi_mgmr_sta_autoconnect_disable(void);
int wifi_mgmr_sta_connect(const wifi_mgmr_sta_connect_params_t*);
typedef struct { const char* text; } ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
struct netif {
    struct netif* next;
    int up, link;
    ip4_addr_t ip, mask, gw;
    unsigned char hwaddr_len, hwaddr[6];
};
extern struct netif* netif_list;
#define NETIF_FOREACH(n) for ((n) = netif_list; (n) != NULL; (n) = (n)->next)
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
#define netif_ip4_addr(n) (&(n)->ip)
#define netif_ip4_netmask(n) (&(n)->mask)
#define netif_ip4_gw(n) (&(n)->gw)
#define ip4_addr_isany_val(a) ((a).text[0] == '0')
#define ip4_addr_isloopback(a) ((a)->text[0] == '1' && (a)->text[1] == '2' && (a)->text[2] == '7')
#define ip4addr_ntoa(a) ((a)->text)
#define ipaddr_ntoa(a) ((a)->text)
const ip_addr_t* dns_getserver(unsigned);
int tcpip_try_callback(void (*)(void*), void*);
int async_register_event_filter(uintptr_t, async_event_cb, void*);
void wifi_task_create(void);
int fhost_init(void);
int wifi_mgmr_task_start(void);
int rfparam_init(unsigned, void*, unsigned);
void tcpip_init(void (*)(void*), void*);
#endif
"""
HARNESS = r"""
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include "wifi_init.h"
extern "C" {
#include "task.h"
#include "sdk_stubs.h"
}
struct TaskBlocked {};
static int mode, driver_calls, manager_calls, creates, waits, connect_calls, disables, probes;
static bool in_event_callback, monitor_delivered;
bool RunNetworkProbe() { assert(!in_event_callback); ++probes; return true; }
static TickType_t ticks;
static uint32_t pending;
static async_event_cb listener;
static void* listener_context;
static void (*tcpip_callback)(void*);
static void* tcpip_context;
static void (*info_callback)(void*);
static void* info_context;
static netif station{nullptr, 1, 1, {"192.168.1.23"}, {"255.255.255.0"},
                     {"192.168.1.1"}, 6, {0, 1, 2, 3, 4, 5}};
static void Emit(unsigned code) {
    mock_event event{code};
    in_event_callback = true;
    listener(&event, listener_context);
    in_event_callback = false;
}
extern "C" {
struct netif* netif_list = &station;
TickType_t xTaskGetTickCount() { return ticks; }
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bit, int action) {
    assert(task == xTaskGetCurrentTaskHandle() && action == eSetBits);
    pending |= bit;
    return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t enter, uint32_t clear, uint32_t* bits, TickType_t timeout) {
    assert(enter == 0 && !in_event_callback);
    if (timeout == portMAX_DELAY) {
        if (mode != 18 || monitor_delivered) throw TaskBlocked{};
        monitor_delivered = true;
        Emit(CODE_WIFI_ON_DISCONNECT);
    } else {
        ++waits;
    }
    *bits = pending;
    pending &= ~clear;
    ticks += *bits ? 1 : timeout;
    return *bits ? pdTRUE : pdFALSE;
}
BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned, void* context,
                      unsigned, TaskHandle_t*) {
    ++creates;
    if (mode == 8) return pdFALSE;
    try { entry(context); } catch (const TaskBlocked&) {}
    return pdPASS;
}
int wifi_mgmr_sta_autoconnect_disable() {
    assert(!in_event_callback);
    ++disables;
    return mode == 21 ? -1 : 0;
}
int wifi_mgmr_sta_connect(const wifi_mgmr_sta_connect_params_t* params) {
    assert(!in_event_callback);
    ++connect_calls;
    assert(params->pmf_cfg == 1 && params->use_dhcp == 1);
    assert(params->timeout_ms == 30000 && params->dhcp_timeout_event_only == 1);
    if (mode == 22) {
        assert(params->ssid_len == 32 && params->key_len == 64);
    } else {
        assert(params->ssid_len == 9 && params->key_len == 8);
        assert(std::memcmp(params->ssid, "test-ssid", 9) == 0);
        assert(std::memcmp(params->key, "test-key", 8) == 0);
    }
    if (mode == 10) return -1;
    if (mode == 11) return 0;
    if (mode == 13) Emit(CODE_WIFI_ON_GOT_IP_TIMEOUT);
    else if (mode == 14) Emit(CODE_WIFI_ON_PARAMS_ERROR);
    else if (mode == 15) Emit(CODE_WIFI_ON_DISCONNECT);
    else if (mode == 26) Emit(CODE_WIFI_ON_LOST_IP);
    else {
        Emit(CODE_WIFI_ON_CONNECTED);
        if (mode != 12) Emit(CODE_WIFI_ON_GOT_IP);
    }
    return 0;
}
const ip_addr_t* dns_getserver(unsigned index) {
    assert(index == 0 && !in_event_callback);
    static const ip_addr_t dns{"192.168.1.1"};
    return &dns;
}
int tcpip_try_callback(void (*callback)(void*), void* context) {
    assert(!in_event_callback);
    if (mode == 16) return ERR_MEM;
    if (mode == 17) netif_list = nullptr;
    if (mode == 27) Emit(CODE_WIFI_ON_GOT_IP_TIMEOUT);
    if (mode == 25) station.link = 0;
    if (mode == 20) {
        info_callback = callback;
        info_context = context;
    } else {
        callback(context);
    }
    return ERR_OK;
}
int rfparam_init(unsigned, void*, unsigned) { return mode == 1 ? -1 : 0; }
void tcpip_init(void (*callback)(void*), void* context) {
    tcpip_callback = callback;
    tcpip_context = context;
    if (mode != 2) callback(context); // readiness may arrive before the task waits
}
int async_register_event_filter(uintptr_t type, async_event_cb callback, void* context) {
    assert(type == EV_WIFI);
    if (mode == 3) return -1;
    listener = callback;
    listener_context = context;
    return 0;
}
void wifi_task_create() { ++driver_calls; }
int fhost_init() {
    assert(waits == 1 && listener);
    if (mode == 4) return -1;
    listener(nullptr, listener_context); // SDK can deliver a null event on removal
    mock_event ignored{99};
    listener(&ignored, listener_context);
    if (mode != 5) {
        mock_event ready{CODE_WIFI_ON_INIT_DONE};
        listener(&ready, listener_context);
    }
    return 0;
}
int wifi_mgmr_task_start() {
    ++manager_calls;
    assert(waits == 2 && driver_calls == 1);
    if (mode == 6) return -1;
    if (mode != 7) {
        mock_event ready{CODE_WIFI_ON_MGMR_DONE};
        listener(&ready, listener_context);
    }
    return 0;
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    mode = std::atoi(argv[1]);
    bool started;
    const bool invalid = mode == 19 || mode == 23 || mode == 24;
    if (mode == 19) started = StartWifiInitialization(std::string(33, 's').c_str(), "test-key");
    else if (mode == 23) started = StartWifiInitialization("test-ssid", std::string(65, 'k').c_str());
    else if (mode == 24) started = StartWifiInitialization("test-ssid", nullptr);
    else if (mode == 22) started = StartWifiInitialization(std::string(32, 's').c_str(),
                                                         std::string(64, 'a').c_str());
    else if (mode >= 9) started = StartWifiInitialization("test-ssid", "test-key");
    else started = StartWifiInitialization();
    assert(started == (mode != 8 && !invalid));
    if (mode != 8 && !invalid) {
        assert(StartWifiInitialization());
        assert(creates == 1); // a second call must not reinitialize or reconnect
    }
    if (invalid) assert(creates == 0);
    if (mode >= 1 && mode <= 5) assert(manager_calls == 0);
    if (mode == 0) assert(driver_calls == 1 && manager_calls == 1 && waits == 3);
    if (mode >= 9 && !invalid) {
        assert(disables == 1);
        assert(connect_calls == (mode == 21 ? 0 : 1));
    } else assert(connect_calls == 0);
    if (mode == 2) tcpip_callback(tcpip_context); // late callback keeps a valid task owner
    if (mode == 20) info_callback(info_context); // late address callback also remains safe
    assert(probes == ((mode == 9 || mode == 18 || mode == 22) ? 1 : 0));
}
"""


class WifiInitializationTests(unittest.TestCase):
    def test_ready_events_and_failure_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "lwip").mkdir()
            (work / "FreeRTOS.h").write_text(FREERTOS)
            (work / "task.h").write_text(TASK)
            (work / "sdk_stubs.h").write_text(SDK)
            for header in ("async_event.h", "fhost_api.h", "macsw_plat.h", "wifi_mgmr.h",
                           "wifi_mgmr_ext.h", "rfparam_adapter.h", "lwip/tcpip.h", "lwip/dns.h", "lwip/ip_addr.h",
                           "lwip/netif.h"):
                (work / header).write_text('#include "sdk_stubs.h"\n')
            (work / "esp_log.h").write_text(
                '#include <stdio.h>\n'
                '#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\\n", tag, ##__VA_ARGS__)\n'
                '#define ESP_LOGW ESP_LOGI\n')
            (work / "check.cc").write_text(HARNESS)
            includes = ["-I" + str(work), "-I" + str(PORT)]
            subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", *includes,
                            "-c", str(PORT / "wifi_sdk_bridge.c"), "-o", str(work / "bridge.o")],
                           check=True)
            executable = work / "check"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", *includes,
                            str(PORT / "wifi_init.cc"), str(work / "check.cc"),
                            str(work / "bridge.o"), "-o", str(executable)], check=True)
            for mode in range(28):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), str(mode)], check=True,
                                            capture_output=True, text=True, timeout=5)
                    invalid = mode in (19, 23, 24)
                    init_ok = mode == 0 or (mode >= 9 and not invalid)
                    self.assertEqual("WIFI_INIT_OK:" in result.stdout, init_ok)
                    self.assertEqual("WIFI_INIT_FAILED:" in result.stdout, not init_ok)
                    ready = mode in (9, 18, 22)
                    self.assertEqual("WIFI_READY:" in result.stdout, ready)
                    self.assertEqual("WIFI_OFFLINE:" in result.stdout, mode == 18)
                    if mode == 11:
                        self.assertIn("timeout waiting for association", result.stdout)
                    if mode == 12:
                        self.assertIn("timeout waiting for DHCP", result.stdout)
                    if mode == 0:
                        self.assertIn("WIFI_NOT_CONFIGURED", result.stdout)


if __name__ == "__main__":
    unittest.main()
