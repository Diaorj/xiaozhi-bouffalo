#ifndef XIAOZHI_BOUFFALO_PTHREAD_KEY_ASSERT_H
#define XIAOZHI_BOUFFALO_PTHREAD_KEY_ASSERT_H

// Preserve the SDK's pthread types, mutexes and other supported APIs.
#include_next <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// Diagnostic declarations only. These APIs deliberately have no functionality.
int pthread_key_create(pthread_key_t* key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
void* pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void* value);

#ifdef __cplusplus
}
#endif

#endif  // XIAOZHI_BOUFFALO_PTHREAD_KEY_ASSERT_H
