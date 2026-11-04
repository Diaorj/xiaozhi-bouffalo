#pragma once
struct ApplicationSessionHooks;
// max_rounds=0 selects continuous mode when XIAOZHI_AUTO_CHAT is enabled.
// network_available is an optional nonblocking owner-task check. On link/IP loss,
// stop capture/playback and release the session before returning false.
bool RunOriginalWebsocketHandshake(unsigned max_rounds = 0,
                                   bool (*network_available)() = nullptr,
                                   const ApplicationSessionHooks* hooks = nullptr);
