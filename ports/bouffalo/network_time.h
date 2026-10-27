#ifndef XIAOZHI_BOUFFALO_NETWORK_TIME_H
#define XIAOZHI_BOUFFALO_NETWORK_TIME_H

#include <stdint.h>

// SNTP era 0 and a plausible development-clock range (2024 through early 2036).
#define NETWORK_TIME_MIN 1704067200UL
#define NETWORK_TIME_MAX 2085978495UL

#ifdef __cplusplus
extern "C" {
#endif
// Blocking normal-task API. Returns 0 when certificate date checks can use the clock.
int NetworkPrepareTime(void);
int NetworkSetServerTime(double timestamp_ms);

#ifdef __cplusplus
}
#endif
#endif  // XIAOZHI_BOUFFALO_NETWORK_TIME_H
