#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_WORKER_STACK_WORDS 1536U
// One owner starts a job; that job must be joined before starting another.
int SpeakerWorkerStart(void (*job)(void*), void* context);
int SpeakerWorkerWait(void);

#ifdef __cplusplus
}
#endif
