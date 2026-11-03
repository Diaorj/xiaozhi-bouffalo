#ifndef XIAOZHI_AUDIO_VAD_H
#define XIAOZHI_AUDIO_VAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_VAD_RATE 16000U
#define AUDIO_VAD_BLOCK 160U
#define AUDIO_VAD_CALIBRATION_FRAMES 4800U
#define AUDIO_VAD_WAIT_FRAMES 80000U
#define AUDIO_VAD_SILENCE_FRAMES 12800U
#define AUDIO_VAD_START_FRAMES 1280U
#define AUDIO_VAD_MIN_VOICE_FRAMES 2560U
#define AUDIO_VAD_PREROLL_FRAMES 4000U
#define AUDIO_VAD_LISTEN_RING_FRAMES (AUDIO_VAD_PREROLL_FRAMES + AUDIO_VAD_START_FRAMES)
typedef enum {
    AUDIO_VAD_CONTINUE, AUDIO_VAD_READY, AUDIO_VAD_STARTED,
    AUDIO_VAD_END_SILENCE, AUDIO_VAD_END_LIMIT, AUDIO_VAD_NO_SPEECH
} audio_vad_event_t;
typedef struct {
    uint32_t total, limit, noise_sum, noise, start_threshold, hold_threshold;
    uint32_t candidate, run, voice_frames, quiet_frames, trim_start;
    bool speaking, finished, valid_voice, continuous;
} audio_vad_t;
void audio_vad_init(audio_vad_t *vad, uint32_t limit_frames);
/* No idle timeout. Limit applies to the utterance including retained preroll. */
void audio_vad_init_listener(audio_vad_t *vad, uint32_t clip_frames);
/* One 10-ms mono block. Work is bounded, DC is removed, no float/heap/RTOS. */
audio_vad_event_t audio_vad_process(audio_vad_t *vad, const int16_t *pcm, size_t frames);
#endif
