#ifndef XIAOZHI_BOUFFALO_NETWORK_PROBE_H
#define XIAOZHI_BOUFFALO_NETWORK_PROBE_H

// One blocking DNS/time/HTTPS check from a normal task after IPv4 is ready.
// Returns true only for a complete, certificate-verified HTTPS 2xx response.
bool RunNetworkProbe();

#endif  // XIAOZHI_BOUFFALO_NETWORK_PROBE_H
