#pragma once
#include "websocket_support.h"
// Handshake declares the original protocol format. Audio tasks are not started.
inline constexpr int OPUS_FRAME_DURATION_MS = 60;
namespace Lang::Strings {
inline constexpr char SERVER_ERROR[] = "Server error";
inline constexpr char SERVER_NOT_CONNECTED[] = "Server not connected";
inline constexpr char SERVER_TIMEOUT[] = "Server hello timeout";
}
