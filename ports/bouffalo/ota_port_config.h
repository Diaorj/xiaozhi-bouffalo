#pragma once
#include <string>
#include "network_time.h"
#define CONFIG_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
namespace Lang { inline constexpr char CODE[] = "zh-CN"; }
inline std::string BouffaloApplicationVersion() { return "0.0.1"; }
