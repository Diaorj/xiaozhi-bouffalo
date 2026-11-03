#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <freertos/FreeRTOS.h>

class AudioService;
class AudioCodec;

class VoiceInputSession {
public:
    static constexpr size_t kMaxSamples = 16000 * 10;
    static constexpr size_t kFrameSamples = 960;
    static constexpr size_t kFifoSamples = 16000;  // Bounded 1-second PSRAM backlog.
    static constexpr size_t kPrerollSamples = 4000 + 2560;  // 250 ms + onset validation.
    VoiceInputSession(AudioService& audio, AudioCodec* codec) : audio_(audio), codec_(codec) {}
    ~VoiceInputSession();
    VoiceInputSession(const VoiceInputSession&) = delete;
    VoiceInputSession& operator=(const VoiceInputSession&) = delete;
    bool Prepare();
    bool StartCapture();
    bool VoiceStarted() const { return voice_started_.load(); }
    bool CaptureDone() const { return done_.load(); }
    bool PopPcmFrame(std::vector<int16_t>& frame);
    size_t SamplesRecorded() const { return published_.load(); }
    size_t ExpectedPackets() const { return (SamplesRecorded() + kFrameSamples - 1) / kFrameSamples; }
    const char* Failure() const { return failure_.load(); }
    TickType_t RecordingStartedAt() const { return recording_started_.load(); }
    TickType_t RecordingEndedAt() const { return recording_ended_.load(); }
    void Stop();
private:
    static void CaptureEntry(void* context);
    void Capture();
    bool Publish(const int16_t* pcm, size_t count);
    void Fail(const char* reason);
    AudioService& audio_;
    AudioCodec* codec_;
    int16_t* storage_ = nullptr;
    bool encoding_owned_ = false;
    bool capture_pending_ = false;
    std::atomic<bool> cancelled_{false}, done_{false}, voice_started_{false};
    std::atomic<uint32_t> published_{0}, consumed_{0};
    std::atomic<const char*> failure_{nullptr};
    std::atomic<TickType_t> recording_started_{0}, recording_ended_{0};
};
