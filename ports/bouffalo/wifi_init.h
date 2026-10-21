#ifndef XIAOZHI_BOUFFALO_WIFI_INIT_H
#define XIAOZHI_BOUFFALO_WIFI_INIT_H

// Called once from the bootstrap task, after the scheduler starts.
// Success means the init task was created; WIFI_INIT_OK confirms completion.
bool StartWifiInitialization();

#endif  // XIAOZHI_BOUFFALO_WIFI_INIT_H
