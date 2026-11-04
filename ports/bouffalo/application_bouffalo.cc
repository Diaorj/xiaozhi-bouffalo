#include "application.h"
#include "platform/application_runtime.h"
#include "wifi_init.h"
#include "memory_diagnostics.h"
#include "protocols/websocket_protocol.h"

#include <atomic>
#include <new>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

namespace {
class BouffaloApplicationRuntime final : public ApplicationRuntime {
public:
    bool Start(Application& application, AudioService& audio,
               std::unique_ptr<Protocol>& protocol) override {
        application_ = &application;
        auto websocket = std::unique_ptr<WebsocketProtocol>(new (std::nothrow) WebsocketProtocol);
        if (!websocket)
            return false;
        hooks_.websocket = websocket.get();
        protocol = std::move(websocket);
        hooks_.audio_service = &audio;
        hooks_.context = this;
        hooks_.post_event = [](void* context, ApplicationRuntimeEvent event) {
            auto& self = *static_cast<BouffaloApplicationRuntime*>(context);
            self.voice_detected_ = event == ApplicationRuntimeEvent::VoiceStarted;
            self.application_->PostRuntimeEvent(event);
        };
        hooks_.take_listening_request = [](void* context) {
            auto& self = *static_cast<BouffaloApplicationRuntime*>(context);
            return self.enabled_.load() && self.permit_.exchange(false);
        };
        hooks_.listening_enabled = [](void* context) {
            return static_cast<BouffaloApplicationRuntime*>(context)->ListeningEnabled();
        };
        return StartWifiInitialization(XIAOZHI_WIFI_SSID, XIAOZHI_WIFI_PASSWORD, &hooks_);
    }
    void SetListeningEnabled(bool enabled) override {
        enabled_ = enabled;
        if (!enabled)
            permit_ = false;
    }
    bool ListeningEnabled() const override { return enabled_.load(); }
    void AllowListening() override {
        MemoryDiagnosticsStage("application_listen_grant");
        permit_ = true;
    }
    void DiscardListeningRequest() override { permit_ = false; }
    bool IsVoiceDetected() const override { return voice_detected_.load(); }
private:
    Application* application_ = nullptr;
    ApplicationSessionHooks hooks_;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> permit_{false};
    std::atomic<bool> voice_detected_{false};
};
}  // namespace

std::unique_ptr<ApplicationRuntime> CreateApplicationRuntime() {
    return std::unique_ptr<ApplicationRuntime>(new (std::nothrow) BouffaloApplicationRuntime);
}
