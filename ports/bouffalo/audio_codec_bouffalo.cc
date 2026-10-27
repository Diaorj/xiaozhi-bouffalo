#include "audio_codec_bouffalo.h"
#include <esp_log.h>
#include "audio_sdk_bridge.h"
BouffaloAudioCodec::BouffaloAudioCodec() {
    input_sample_rate_ = output_sample_rate_ = 16000;
    output_volume_ = 15;
}
void BouffaloAudioCodec::Start() {
    // Playback-only stage: do not load/write persistent volume or enable ADC.
    ESP_LOGI("AudioCodec", "BL616CL_ES8388_READY: playback only, volume=%d", output_volume_);
}
void BouffaloAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_)
        return;
    if (enable) {
        healthy_ = AudioSdkOpen(output_volume_) == 0;
        output_enabled_ = healthy_;
    } else
        Abort();
}
int BouffaloAudioCodec::Write(const int16_t* pcm, int frames) {
    if (!output_enabled_ || !healthy_ || frames <= 0)
        return -1;
    const int sent = AudioSdkWrite(pcm, frames);
    if (sent != frames)
        healthy_ = false;
    return sent;
}
bool BouffaloAudioCodec::FinishOutput() {
    bool ok = healthy_ && output_enabled_;
    if (output_enabled_)
        ok = (AudioSdkFinish() == 0) && ok;
    output_enabled_ = false;
    healthy_ = ok;
    return ok;
}
void BouffaloAudioCodec::Abort() {
    if (output_enabled_)
        AudioSdkAbort();
    output_enabled_ = false;
}
