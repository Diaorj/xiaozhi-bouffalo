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
#include "wifi_init.h"
extern "C" {
#include "task.h"
#include "sdk_stubs.h"
}
struct TaskBlocked {};
static int mode, driver_calls, manager_calls, creates, waits;
static uint32_t pending;
static async_event_cb listener;
static void* listener_context;
static void (*tcpip_callback)(void*);
static void* tcpip_context;
extern "C" {
TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<void*>(1); }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bit, int action) {
    assert(task == xTaskGetCurrentTaskHandle() && action == eSetBits);
    pending |= bit;
    return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t enter, uint32_t clear, uint32_t* bits, TickType_t timeout) {
    assert(enter == 0);
    if (timeout == portMAX_DELAY) throw TaskBlocked{};
    ++waits;
    *bits = pending;
    pending &= ~clear;
    return *bits ? pdTRUE : pdFALSE;
}
BaseType_t xTaskCreate(void (*entry)(void*), const char*, unsigned, void* context,
                      unsigned, TaskHandle_t*) {
    ++creates;
    if (mode == 8) return pdFALSE;
    try { entry(context); } catch (const TaskBlocked&) {}
    return pdPASS;
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
    assert(StartWifiInitialization() == (mode != 8));
    if (mode != 8) {
        assert(StartWifiInitialization());
        assert(creates == 1); // a second call must not reinitialize the stack
    }
    if (mode >= 1 && mode <= 5) assert(manager_calls == 0);
    if (mode == 0) assert(driver_calls == 1 && manager_calls == 1 && waits == 3);
    if (mode == 2) tcpip_callback(tcpip_context); // late callback keeps a valid task owner
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
                           "wifi_mgmr_ext.h", "rfparam_adapter.h", "lwip/tcpip.h"):
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
            for mode in range(9):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), str(mode)], check=True,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual("WIFI_INIT_OK:" in result.stdout, mode == 0)
                    self.assertEqual("WIFI_INIT_FAILED:" in result.stdout, mode != 0)


if __name__ == "__main__":
    unittest.main()
