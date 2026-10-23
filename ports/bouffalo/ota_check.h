#ifndef XIAOZHI_BOUFFALO_OTA_CHECK_H
#define XIAOZHI_BOUFFALO_OTA_CHECK_H

class OtaCheckResponse;
// Called once from the normal network task. No upgrade or activation POST.
bool RunOtaCheck();
// RAM-only response; use from the network task after a successful check.
const OtaCheckResponse& GetOtaCheckResponse();

#endif  // XIAOZHI_BOUFFALO_OTA_CHECK_H
