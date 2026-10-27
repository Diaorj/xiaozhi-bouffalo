#pragma once

#define XIAOZHI_BUILD_TAG "audio-memdiag-20261027-v1"

#ifdef __cplusplus
extern "C" {
#endif
// Bounded, allocation-free checkpoints; sizes and stack watermarks are in bytes.
void MemoryDiagnosticsStage(const char* stage);
#ifdef __cplusplus
}
#endif
