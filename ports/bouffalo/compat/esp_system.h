#pragma once
#include <cstdlib>
#include <cstdio>
[[noreturn]] inline void esp_system_abort(const char* message) {
    std::printf("[FATAL] %s\r\n",message);std::abort();
}
