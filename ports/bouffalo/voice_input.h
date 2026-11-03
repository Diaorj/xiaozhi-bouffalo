#pragma once
#include <atomic>
#include <cstddef>
#include <freertos/FreeRTOS.h>

class AudioService;
class AudioCodec;

// Owner creates/joins the session; the shared DMA worker captures PCM and pushes
// complete frames into AudioService. The owner concurrently drains its send queue.
class VoiceInputSession {
public:
    static constexpr size_t kSamples = 16000 * 5;
    static constexpr size_t kFrameSamples = 960;
    static constexpr size_t kFrames = (kSamples + kFrameSamples - 1) / kFrameSamples;
    VoiceInputSession(AudioService& audio, AudioCodec* codec) : audio_(audio), codec_(codec) {}
    ~VoiceInputSession();
    VoiceInputSession(const VoiceInputSession&) = delete;
    VoiceInputSession& operator=(const VoiceInputSession&) = delete;
    bool Prepare();
    bool StartCapture();
    bool CaptureDone() const { return done_.load(); }
    const char* Failure() const { return failure_.load(); }
    TickType_t RecordingStartedAt() const { return recording_started_.load(); }
    TickType_t RecordingEndedAt() const { return recording_ended_.load(); }
    // Cancels/joins capture before stopping its encoder. Never called by callbacks.
    void Stop();
private:
    static void CaptureEntry(void* context);
    void Capture();
    void Fail(const char* reason);
    AudioService& audio_;
    AudioCodec* codec_;
    bool encoding_owned_ = false;
    bool capture_pending_ = false;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> done_{false};
    std::atomic<const char*> failure_{nullptr};
    std::atomic<TickType_t> recording_started_{0};
    std::atomic<TickType_t> recording_ended_{0};
};
