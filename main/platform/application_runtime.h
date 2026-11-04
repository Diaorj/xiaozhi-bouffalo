#pragma once

#include <memory>

class Application;
class AudioService;
class Protocol;
class WebsocketProtocol;

enum class ApplicationRuntimeEvent {
    NetworkReady,
    ActivationReady,
    Connecting,
    SessionReady,
    VoiceStarted,
    WaitingReply,
    Speaking,
    PlaybackFinished,
    Offline,
    Failed,
};

// Long-running I/O stays in a platform worker. Events are posted to Application;
// the worker may start another capture only after Application grants permission.
struct ApplicationSessionHooks {
    AudioService* audio_service = nullptr;
    WebsocketProtocol* websocket = nullptr;
    void* context = nullptr;
    void (*post_event)(void*, ApplicationRuntimeEvent) = nullptr;
    bool (*take_listening_request)(void*) = nullptr;
    bool (*listening_enabled)(void*) = nullptr;
};

class ApplicationRuntime {
public:
    virtual ~ApplicationRuntime() = default;
    // Called only by Initialize(), before Run(). Application retains ownership.
    virtual bool Start(Application& application, AudioService& audio,
                       std::unique_ptr<Protocol>& protocol) = 0;
    virtual void SetListeningEnabled(bool enabled) = 0;
    virtual bool ListeningEnabled() const = 0;
    virtual void AllowListening() = 0;
    virtual void DiscardListeningRequest() = 0;
    virtual bool IsVoiceDetected() const = 0;
};

std::unique_ptr<ApplicationRuntime> CreateApplicationRuntime();
