#ifndef XIAOZHI_BOUFFALO_ESP_LOG_H
#define XIAOZHI_BOUFFALO_ESP_LOG_H

#include <stdio.h>

#include "log.h"

// Only the two macros needed by the XiaoZhi state machine are adapted here.
// SDK raw logging preserves its level filtering; the ESP caller supplies the tag.
#define ESP_LOGI(tag, format, ...) \
    LOG_RI("[I][%s] " format "\r\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
    LOG_RW("[W][%s] " format "\r\n", (tag), ##__VA_ARGS__)

#endif  // XIAOZHI_BOUFFALO_ESP_LOG_H

#ifndef ESP_LOGE
#define ESP_LOGE(tag, format, ...) LOG_RE("[E][%s] " format "\r\n", tag, ##__VA_ARGS__)
#endif

#ifndef ESP_LOGD
#define ESP_LOGD(tag, format, ...) ((void)0)
#endif
