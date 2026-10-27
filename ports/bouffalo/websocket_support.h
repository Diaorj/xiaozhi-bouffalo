#pragma once
#include <string>
#include "tls_sdk_bridge.h"
// Narrow transport helpers, not XiaoZhi authentication/hello business logic.
bool ParseSecureWebsocketUri(const char* uri, std::string& host, std::string& port, std::string& path);
bool VerifyWebsocketUpgrade(const std::string& response, const std::string& client_key);
