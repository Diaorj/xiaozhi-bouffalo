#pragma once

#include <mutex>

#ifdef XIAOZHI_PLATFORM_BOUFFALO
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace xiaozhi {
// Construct the SDK native object in application code. The prebuilt libstdc++
// condition_variable constructor uses a different pthread_cond_t layout.
class ConditionVariable {
public:
    ConditionVariable() { Check("init", pthread_cond_init(&condition_, nullptr)); }
    ~ConditionVariable() { Check("destroy", pthread_cond_destroy(&condition_)); }
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    template <class Predicate>
    void wait(std::unique_lock<std::mutex>& lock, Predicate ready) {
        while (!ready()) {
            Check("wait", pthread_cond_wait(&condition_, lock.mutex()->native_handle()));
        }
    }

    void notify_all() noexcept { Check("broadcast", pthread_cond_broadcast(&condition_)); }

private:
    static_assert(std::is_same<std::mutex::native_handle_type, pthread_mutex_t*>::value,
                  "Audio queue mutex must use the same SDK pthread backend");
    pthread_cond_t condition_{};

    static void Check(const char* operation, int error) noexcept {
        if (error != 0) {
            // Preserve the actual SDK error instead of an opaque C++ terminate.
            std::fprintf(stderr, "[E][POSIX] CONDITION_VARIABLE_FAILED: %s ret=%d\n", operation,
                         error);
            std::abort();
        }
    }
};
}  // namespace xiaozhi
#else
#include <condition_variable>
namespace xiaozhi {
using ConditionVariable = std::condition_variable;
}  // namespace xiaozhi
#endif
