#ifndef XIAOZHI_BOUFFALO_WIFI_INIT_H
#define XIAOZHI_BOUFFALO_WIFI_INIT_H

struct ApplicationSessionHooks;

// Called once from the bootstrap task, after the scheduler starts.
// Credentials are copied. Empty SSID initializes only; otherwise retry connection with bounded backoff.
// Success means the task was created; WIFI_READY confirms an acquired IPv4 address.
bool StartWifiInitialization(const char* ssid = "", const char* password = "",
                             const ApplicationSessionHooks* hooks = nullptr);

#endif  // XIAOZHI_BOUFFALO_WIFI_INIT_H
