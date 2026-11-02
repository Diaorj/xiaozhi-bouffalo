#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Diagnostic only: fail on every call, including before the scheduler starts.
static _Noreturn void unsupported_key_call(const char* api, uintptr_t caller) {
    printf("[E][PTHREAD_KEY] UNSUPPORTED_CALL: %s caller=0x%lx\r\n", api,
           (unsigned long)caller);
    assert(0 && "pthread key API is not implemented; see PTHREAD_KEY log");
    // An NDEBUG build must also stop instead of silently reporting success.
    abort();
}

int pthread_key_create(pthread_key_t* key, void (*destructor)(void*)) {
    (void)key;
    (void)destructor;
    unsupported_key_call("pthread_key_create", (uintptr_t)__builtin_return_address(0));
}

int pthread_key_delete(pthread_key_t key) {
    (void)key;
    unsupported_key_call("pthread_key_delete", (uintptr_t)__builtin_return_address(0));
}

void* pthread_getspecific(pthread_key_t key) {
    (void)key;
    unsupported_key_call("pthread_getspecific", (uintptr_t)__builtin_return_address(0));
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    (void)key;
    (void)value;
    unsupported_key_call("pthread_setspecific", (uintptr_t)__builtin_return_address(0));
}
