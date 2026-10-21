#ifndef XIAOZHI_PLATFORM_MUTEX_H
#define XIAOZHI_PLATFORM_MUTEX_H

#include <mutex>

#if defined(XIAOZHI_PLATFORM_BOUFFALO)
#include "freertos_mutex.h"
using PlatformMutex = BouffaloMutex;
#else
using PlatformMutex = std::mutex;
#endif

#endif  // XIAOZHI_PLATFORM_MUTEX_H
