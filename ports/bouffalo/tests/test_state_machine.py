"""Host checks of original transitions and std::mutex contention, not SDK hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

HARNESS = r"""
#include <cassert>
#include <mutex>
#include <thread>
#include <vector>
#include "device_state_machine.h"
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
    // Exercise concurrent accesses protected by the standard mutex.
    std::mutex lock;
    int counter = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            for (int n = 0; n < 2000; ++n) {
                std::lock_guard<std::mutex> guard(lock);
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
    def test_state_machine_and_mutex_contention(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "esp_log.h").write_text(
                "#define ESP_LOGI(tag, ...) ((void)(tag))\n#define ESP_LOGW(tag, ...) ((void)(tag))\n")
            (work / "check.cc").write_text(HARNESS)
            output = work / "check"
            command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I" + str(work), "-I" + str(ROOT / "main"),
                       str(work / "check.cc"), str(ROOT / "main/device_state_machine.cc"),
                       "-o", str(output)]
            subprocess.run(command, check=True)
            subprocess.run([str(output)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
