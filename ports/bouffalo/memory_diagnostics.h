#pragma once

#define XIAOZHI_BUILD_TAG "voice-stream-20261103-v5"

#ifdef __cplusplus
extern "C" {
#endif
// Bounded, allocation-free checkpoints; sizes and stack watermarks are in bytes.
void MemoryDiagnosticsStage(const char* stage);
#ifdef __cplusplus
}
#endif
