#include "application.h"

#include <esp_log.h>

#ifndef XIAOZHI_APPLICATION_PLATFORM_BACKEND
#error "Build this file only for the platform Application runtime"
#endif

Application::Application() {
    event_group_ = xEventGroupCreate();
    runtime_ = CreateApplicationRuntime();
}

Application::~Application() {
    runtime_.reset();
    if (event_group_)
        vEventGroupDelete(event_group_);
}

void Application::Initialize() {
    if (runtime_started_)
        return;
    if (!event_group_ || !runtime_) {
        ESP_LOGE("Application", "APP_INIT_FAILED: event group or platform runtime unavailable");
        return;
    }
    state_machine_.AddStateChangeListener([this](DeviceState, DeviceState) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });
    SetDeviceState(kDeviceStateStarting);
    ESP_LOGI("Application", "APPLICATION_READY: original Run/Schedule/state machine; "
                           "platform I/O worker");
    runtime_started_ = runtime_->Start(*this, audio_service_, protocol_);
    if (!runtime_started_)
        ESP_LOGE("Application", "APP_INIT_FAILED: cannot start network worker");
}

void Application::PostRuntimeEvent(ApplicationRuntimeEvent event) {
    Schedule([this, event] { HandleRuntimeEvent(event); });
}

void Application::HandleRuntimeEvent(ApplicationRuntimeEvent event) {
    // This runs in Application::Run, never in a Wi-Fi/receiver/audio callback.
    const auto idle = [this] {
        const auto state = GetDeviceState();
        if (state != kDeviceStateUnknown && state != kDeviceStateStarting &&
            state != kDeviceStateWifiConfiguring)
            SetDeviceState(kDeviceStateIdle);
    };
    switch (event) {
        case ApplicationRuntimeEvent::NetworkReady:
            idle();
            SetDeviceState(kDeviceStateActivating);
            break;
        case ApplicationRuntimeEvent::ActivationReady:
            SetDeviceState(kDeviceStateIdle);
            break;
        case ApplicationRuntimeEvent::Connecting:
            runtime_session_ready_ = false;
            runtime_->DiscardListeningRequest();
            idle();
            SetDeviceState(kDeviceStateConnecting);
            break;
        case ApplicationRuntimeEvent::SessionReady:
            runtime_session_ready_ = true;
            idle();
#if XIAOZHI_VOICE_INPUT_TEST
            if (runtime_->ListeningEnabled())
                StartListening();
#endif
            break;
        case ApplicationRuntimeEvent::VoiceStarted:
            ESP_LOGI("Application", "APP_VOICE_STARTED");
            break;
        case ApplicationRuntimeEvent::WaitingReply:
            idle();
            ESP_LOGI("Application", "APP_WAITING_REPLY");
            break;
        case ApplicationRuntimeEvent::Speaking:
            if (runtime_->ListeningEnabled())
                SetDeviceState(kDeviceStateSpeaking);
            break;
        case ApplicationRuntimeEvent::PlaybackFinished:
            // Worker has joined playback and drained DMA, not just received TTS stop.
            xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
            break;
        case ApplicationRuntimeEvent::Offline:
        case ApplicationRuntimeEvent::Failed:
            runtime_session_ready_ = false;
            runtime_->DiscardListeningRequest();
            idle();
            ESP_LOGW("Application", "APP_RECOVERING: waiting for platform network/session recovery");
            break;
    }
}

void Application::HandlePlaybackDrainedEvent() {
    if (GetDeviceState() == kDeviceStateSpeaking)
        SetDeviceState(kDeviceStateIdle);
#if XIAOZHI_AUTO_CHAT && XIAOZHI_VOICE_INPUT_TEST
    if (runtime_->ListeningEnabled()) {
        ESP_LOGI("Application", "APP_RESUME_LISTENING: playback and DMA finished");
        StartListening();
    }
#endif
}

void Application::HandleStateChangedEvent() {
    clock_ticks_ = 0;
    ESP_LOGI("Application", "APP_STATE: %s", DeviceStateMachine::GetStateName(GetDeviceState()));
}

void Application::HandleToggleChatEvent() {
    if (runtime_->ListeningEnabled())
        StopListening();
    else
        StartListening();
}

void Application::HandleStartListeningEvent() {
    runtime_->SetListeningEnabled(true);
    const auto state = GetDeviceState();
    if (runtime_session_ready_ && (state == kDeviceStateIdle || state == kDeviceStateConnecting)) {
        if (SetDeviceState(kDeviceStateListening)) {
            runtime_->AllowListening();
            ESP_LOGI("Application", "APP_LISTEN_GRANTED");
        }
    }
}

void Application::HandleStopListeningEvent() {
    runtime_->SetListeningEnabled(false);
    if (GetDeviceState() == kDeviceStateListening || GetDeviceState() == kDeviceStateSpeaking)
        SetDeviceState(kDeviceStateIdle);
}

void Application::HandleNetworkConnectedEvent() {
    HandleRuntimeEvent(ApplicationRuntimeEvent::NetworkReady);
}
void Application::HandleNetworkDisconnectedEvent() {
    HandleRuntimeEvent(ApplicationRuntimeEvent::Offline);
}
void Application::HandleActivationDoneEvent() {
    HandleRuntimeEvent(ApplicationRuntimeEvent::ActivationReady);
}
