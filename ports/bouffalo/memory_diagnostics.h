#pragma once

#define XIAOZHI_BUILD_TAG "ws-clean-close-20261102-v3"

#ifdef __cplusplus
extern "C" {
#endif
// Bounded, allocation-free checkpoints; sizes and stack watermarks are in bytes.
void MemoryDiagnosticsStage(const char* stage);
#ifdef __cplusplus
}
#endif
