#include "audio_vad.h"
#include <string.h>

static uint32_t block_level(const int16_t *pcm, size_t frames)
{
    int32_t sum = 0;
    for (size_t n = 0; n < frames; n++) sum += pcm[n];
    int32_t mean = sum / (int32_t)frames;
    uint32_t absolute_sum = 0;
    for (size_t n = 0; n < frames; n++) {
        int32_t value = (int32_t)pcm[n] - mean;
        absolute_sum += (uint32_t)(value < 0 ? -value : value);
    }
    return absolute_sum / (uint32_t)frames;
}

static void update_thresholds(audio_vad_t *vad)
{
    vad->start_threshold = vad->noise * 3U + 20U;
    if (vad->start_threshold < 40U) vad->start_threshold = 40U;
    vad->hold_threshold = vad->noise * 2U + 10U;
    if (vad->hold_threshold < 25U) vad->hold_threshold = 25U;
}

void audio_vad_init(audio_vad_t *vad, uint32_t limit_frames)
{
    memset(vad, 0, sizeof(*vad));
    vad->limit = limit_frames;
    update_thresholds(vad);
}

void audio_vad_init_listener(audio_vad_t *vad, uint32_t clip_frames)
{
    audio_vad_init(vad, clip_frames > AUDIO_VAD_PREROLL_FRAMES ?
                        clip_frames - AUDIO_VAD_PREROLL_FRAMES : 1U);
    vad->continuous = true;
}

audio_vad_event_t audio_vad_process(audio_vad_t *vad, const int16_t *pcm, size_t frames)
{
    if (!vad || !pcm || !frames || frames > AUDIO_VAD_BLOCK || vad->finished) return AUDIO_VAD_CONTINUE;
    uint32_t level = block_level(pcm, frames);
    uint32_t before = vad->total;
    vad->total += (uint32_t)frames;
    audio_vad_event_t event = AUDIO_VAD_CONTINUE;
    if (before < AUDIO_VAD_CALIBRATION_FRAMES) {
        vad->noise_sum += level * (uint32_t)frames;
        if (vad->total >= AUDIO_VAD_CALIBRATION_FRAMES) {
            vad->noise = vad->noise_sum / AUDIO_VAD_CALIBRATION_FRAMES;
            update_thresholds(vad);
            event = AUDIO_VAD_READY;
        }
    } else if (!vad->speaking) {
        if (level >= vad->start_threshold) {
            if (!vad->run) vad->candidate = before;
            vad->run += (uint32_t)frames;
            if (vad->run >= AUDIO_VAD_START_FRAMES) {
                vad->speaking = true;
                vad->voice_frames = vad->run;
                vad->quiet_frames = 0;
                vad->trim_start = vad->candidate > AUDIO_VAD_PREROLL_FRAMES ?
                                  vad->candidate - AUDIO_VAD_PREROLL_FRAMES : 0;
                if (vad->trim_start < AUDIO_VAD_CALIBRATION_FRAMES)
                    vad->trim_start = AUDIO_VAD_CALIBRATION_FRAMES;
                event = AUDIO_VAD_STARTED;
            }
        } else {
            vad->run = 0;
            /* Slowly follow a falling/quiet noise floor, never learn a
             * candidate utterance as background noise while it is hot. */
            vad->noise = (vad->noise * 31U + level) / 32U;
            update_thresholds(vad);
        }
    } else {
        if (level >= vad->hold_threshold) {
            vad->voice_frames += (uint32_t)frames;
            vad->quiet_frames = 0;
        } else vad->quiet_frames += (uint32_t)frames;
        if (vad->voice_frames >= AUDIO_VAD_MIN_VOICE_FRAMES) vad->valid_voice = true;
        if (vad->quiet_frames >= AUDIO_VAD_SILENCE_FRAMES) {
            if (vad->valid_voice) {
                vad->finished = true;
                return AUDIO_VAD_END_SILENCE;
            }
            /* A click/brief burst must not terminate listening or upload. */
            vad->speaking = false;
            vad->run = vad->voice_frames = vad->quiet_frames = 0;
        }
    }
    if (vad->continuous && before >= AUDIO_VAD_CALIBRATION_FRAMES && !vad->speaking) {
        /* Keep idle counters bounded even after days without an utterance.
         * The capture owner keeps preroll in a separate fixed-size ring. */
        vad->total = AUDIO_VAD_CALIBRATION_FRAMES + vad->run;
        vad->candidate = AUDIO_VAD_CALIBRATION_FRAMES;
        return event;
    }
    if (!vad->continuous && !vad->speaking && vad->total >= AUDIO_VAD_CALIBRATION_FRAMES + AUDIO_VAD_WAIT_FRAMES) {
        vad->finished = true;
        return AUDIO_VAD_NO_SPEECH;
    }
    if ((!vad->continuous && vad->total >= vad->limit) ||
        (vad->continuous && vad->speaking && vad->total - AUDIO_VAD_CALIBRATION_FRAMES >= vad->limit)) {
        vad->finished = true;
        return vad->valid_voice ? AUDIO_VAD_END_LIMIT : AUDIO_VAD_NO_SPEECH;
    }
    return event;
}
