#pragma once

#define XIAOZHI_BUILD_TAG "screen-20261104-v10"

#ifdef __cplusplus
extern "C" {
#endif
// Bounded, allocation-free checkpoints; sizes and stack watermarks are in bytes.
void MemoryDiagnosticsStage(const char* stage);
#ifdef __cplusplus
}
#endif
