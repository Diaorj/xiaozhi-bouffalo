"""Host checks: original transitions and mutex adaptation (not an RTOS hardware test)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

FREERTOS = r"""
#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <assert.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define portMAX_DELAY (~0u)
#define configASSERT(x) assert(x)
#endif
"""
SEMPHR = r"""
#include "FreeRTOS.h"
typedef struct { void* native; } StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
"""
HARNESS = r"""
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>
#include "device_state_machine.h"
#ifdef XIAOZHI_PLATFORM_BOUFFALO
extern "C" {
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
    storage->native = new std::mutex;
    return storage;
}
void vSemaphoreDelete(SemaphoreHandle_t handle) {
    delete static_cast<std::mutex*>(handle->native);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t wait) {
    assert(wait == portMAX_DELAY);
    static_cast<std::mutex*>(handle->native)->lock();
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
    static_cast<std::mutex*>(handle->native)->unlock();
    return pdTRUE;
}
}
#endif
int main() {
    DeviceStateMachine states;
    assert(states.GetState() == kDeviceStateUnknown);
    assert(!states.TransitionTo(kDeviceStateIdle));
    int calls = 0;
    int id = states.AddStateChangeListener([&](DeviceState, DeviceState) { ++calls; });
    assert(states.TransitionTo(kDeviceStateStarting));
    assert(states.TransitionTo(kDeviceStateStarting)); // no notification on no-op
    assert(calls == 1);
    assert(states.TransitionTo(kDeviceStateActivating));
    assert(states.TransitionTo(kDeviceStateIdle));
    assert(calls == 3);
    assert(!states.TransitionTo(kDeviceStateStarting));
    assert(states.GetState() == kDeviceStateIdle);
    states.RemoveStateChangeListener(id);
    // A callback can remove itself: notification must happen outside the lock.
    int self = -1;
    self = states.AddStateChangeListener([&](DeviceState, DeviceState) {
        states.RemoveStateChangeListener(self);
        ++calls;
    });
    assert(states.TransitionTo(kDeviceStateListening));
    assert(states.TransitionTo(kDeviceStateSpeaking));
    assert(calls == 4);
    // Lock contention checks the port wrapper does not silently skip locking.
    PlatformMutex lock;
    int counter = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            for (int n = 0; n < 2000; ++n) {
                std::lock_guard<PlatformMutex> guard(lock);
                int previous = counter;
                std::this_thread::yield();
                counter = previous + 1;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(counter == 8000);
}
"""


class StateMachineTests(unittest.TestCase):
    def test_native_and_bouffalo_mutex_backends(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "FreeRTOS.h").write_text(FREERTOS)
            (work / "semphr.h").write_text(SEMPHR)
            (work / "esp_log.h").write_text(
                "#define ESP_LOGI(tag, ...) ((void)(tag))\n#define ESP_LOGW(tag, ...) ((void)(tag))\n")
            (work / "check.cc").write_text(HARNESS)
            for backend in ("native", "bouffalo"):
                with self.subTest(backend=backend):
                    output = work / backend
                    command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                               "-I" + str(work), "-I" + str(ROOT / "main"),
                               "-I" + str(ROOT / "ports/bouffalo"),
                               str(work / "check.cc"), str(ROOT / "main/device_state_machine.cc"),
                               "-o", str(output)]
                    if backend == "bouffalo":
                        command += ["-DXIAOZHI_PLATFORM_BOUFFALO=1"]
                    subprocess.run(command, check=True)
                    subprocess.run([str(output)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
