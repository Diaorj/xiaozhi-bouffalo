#pragma once
#include "audio/audio_codec.h"
class BouffaloAudioCodec final : public AudioCodec {
public:
    BouffaloAudioCodec();
    void Start() override;
    void EnableOutput(bool enable) override;
    void EnableInput(bool enable) override;
    bool healthy() const { return healthy_; }
    bool FinishOutput() override;
    bool OutputHealthy() const override { return healthy_; }
    void Abort();

protected:
    int Read(int16_t*, int) override;
    int Write(const int16_t*, int) override;

private:
    bool healthy_ = true;
};
