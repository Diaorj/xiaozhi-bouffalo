#include "websocket_bootstrap.h"
#include <esp_log.h>
#include <atomic>
#include <cstring>
#include "audio/audio_service.h"
#include "board.h"
#include "memory_diagnostics.h"
#include "opus_decoder_bouffalo.h"
#include "protocols/websocket_protocol.h"
#include "settings.h"

namespace {
// Receiver callbacks only enqueue data/set flags. The network task owns cleanup.
struct ReplyState {
    std::atomic<bool> active{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> closing{false};
    std::atomic<unsigned> packets{0};
    std::atomic<unsigned> json_messages{0};
    std::atomic<bool> tts_started{false};
    std::atomic<TickType_t> last_packet{0};
    std::atomic<const char*> failure{nullptr};
    void Fail(const char* reason) {
        if (!active)
            return;
        const char* empty = nullptr;
        failure.compare_exchange_strong(empty, reason);
    }
};
}  // namespace

bool RunOriginalWebsocketHandshake() {
    // Static lifetime: the original protocol retains its callbacks after startup.
    static AudioService audio;
    static ReplyState reply;
    static WebsocketProtocol protocol;
    if (Settings("websocket").GetString("url").empty()) {
        ESP_LOGW("WS", "ORIGINAL_WS_SKIPPED: OTA supplied no WebSocket URL");
        return false;
    }
    protocol.OnNetworkError([](const std::string& error) {
        if (!reply.ended)
            reply.Fail("server or network error; inspect preceding message");
        ESP_LOGE("WS", "ORIGINAL_WS_FAILED: %s", error.c_str());
    });
    protocol.OnAudioChannelClosed([] {
        if (reply.closing) {
            ESP_LOGI("WS", "WS_CHANNEL_CLOSED: local cleanup in progress");
            return;
        }
        if (!reply.ended)
            reply.Fail("connection closed before TTS stop");
        ESP_LOGW("WS", "ORIGINAL_WS_CLOSED: reset to reconnect");
    });
    // This stage runs exactly one greeting. Do not leave an idle server session
    // behind after it finishes; close from the owner and join all callbacks.
    const auto close_session = [&] {
        reply.active = false;
        reply.closing = true;
        protocol.CloseAudioChannel(true);
        reply.closing = false;
        ESP_LOGI("WS", "WS_SESSION_RELEASED: socket and receiver released; reset test is ready");
        MemoryDiagnosticsStage("after_ws_session_close");
    };
    protocol.OnIncomingAudio([](std::unique_ptr<AudioStreamPacket> packet) {
        if (!reply.active)
            return;
        if (reply.ended || !audio.PushPacketToDecodeQueue(std::move(packet), false)) {
            reply.Fail("audio queue rejected packet");
            return;
        }
        ++reply.packets;
        reply.last_packet = xTaskGetTickCount();
    });
    protocol.OnIncomingJson([](const cJSON* root) {
        auto* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type))
            return;
        auto* state = cJSON_GetObjectItem(root, "state");
        if (reply.active) {
            const unsigned index = ++reply.json_messages;
            if (index <= 16) {
                ESP_LOGI("WS", "REPLY_JSON: type=%.48s state=%.48s", type->valuestring,
                         cJSON_IsString(state) ? state->valuestring : "none");
            }
            if (std::strcmp(type->valuestring, "tts") == 0 && cJSON_IsString(state) &&
                std::strcmp(state->valuestring, "start") == 0) {
                reply.tts_started = true;
            }
        }
        if (std::strcmp(type->valuestring, "tts") == 0) {
            if (cJSON_IsString(state) && std::strcmp(state->valuestring, "stop") == 0) {
                ESP_LOGI("WS", "TTS_STOP: drain queued audio before muting");
                reply.ended = true;
            }
            auto* text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text))
                ESP_LOGI("WS", "XIAOZHI: %.512s", text->valuestring);
        } else if (std::strcmp(type->valuestring, "alert") == 0) {
            auto* status = cJSON_GetObjectItem(root, "status");
            auto* message = cJSON_GetObjectItem(root, "message");
            ESP_LOGE("WS", "SERVER_ALERT: status=%.32s message=%.512s",
                     cJSON_IsString(status) ? status->valuestring : "unknown",
                     cJSON_IsString(message) ? message->valuestring : "missing message");
            reply.Fail("server or network error; inspect preceding message");
        } else if (std::strcmp(type->valuestring, "error") == 0) {
            reply.Fail("server or network error; inspect preceding message");
            ESP_LOGE("WS", "SERVER_REPLY_ERROR");
        }
    });
    if (!protocol.Start() || !protocol.OpenAudioChannel()) {
        protocol.CloseAudioChannel(false);
        ESP_LOGW("WS", "ORIGINAL_WS_NOT_READY: reset to retry");
        return false;
    }
    ESP_LOGI("WS", "ORIGINAL_WS_OK: authenticated; server hello received; rate=%d frame=%d ms",
             protocol.server_sample_rate(), protocol.server_frame_duration());
    MemoryDiagnosticsStage("before_opus_state");
    auto decoder = CreateBouffaloOpusDecoder();
    if (!decoder) {
        close_session();
        return false;
    }
    audio.Initialize(Board::GetInstance().GetAudioCodec(), std::move(decoder));
    audio.Start();
    if (!audio.IsRunning()) {
        ESP_LOGE("WS", "AUDIO_WORKER_START_FAILED");
        close_session();
        return false;
    }
    MemoryDiagnosticsStage("audio_service_tasks_created");
    reply.ended = false;
    reply.failure = nullptr;
    reply.packets = 0;
    reply.json_messages = 0;
    reply.tts_started = false;
    const TickType_t started = xTaskGetTickCount();
    reply.last_packet = started;
    reply.active = true;
    // Official listen/detect accepts a short wake phrase, not an arbitrary question.
    protocol.SendWakeWordDetected("你好小智");
    ESP_LOGI("WS", "WAKE_GREETING_REQUESTED: standard short wake phrase; first audio timeout=30s");

    bool ok = false;
    for (;;) {
        if (reply.failure.load())
            break;
        if (audio.HasPlaybackError()) {
            reply.Fail("decoder or speaker open/write failed");
            break;
        }
        if (reply.ended && audio.IsPlaybackIdle()) {
            ok = reply.packets > 0 && audio.FinishPlayback();
            if (!ok)
                reply.Fail(reply.packets ? "speaker DMA drain failed" : "TTS ended without audio");
            break;
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - started > pdMS_TO_TICKS(120000)) {
            reply.Fail("reply total timeout");
            break;
        }
        // Allow cloud synthesis more time before the first audio packet.
        const TickType_t silence_limit = pdMS_TO_TICKS(reply.packets.load() ? 15000 : 30000);
        if (!reply.ended && now - reply.last_packet.load() > silence_limit) {
            reply.Fail(reply.packets ? "audio stalled" : "no audio received within 30s");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI("WS", "REPLY_SUMMARY: audio_packets=%u json_messages=%u tts_started=%d channel_open=%d",
             reply.packets.load(), reply.json_messages.load(), int(reply.tts_started.load()),
             int(protocol.IsAudioChannelOpened()));
    ok = ok && !reply.failure.load();
    reply.active = false;
    MemoryDiagnosticsStage("before_audio_service_stop");
    audio.Stop();  // Joins both original workers before abort/free on a failure.
    close_session();
    if (ok) {
        ESP_LOGI("Audio", "AUDIO_REPLY_DONE: AudioService packets=%u; speaker drained and muted",
                 reply.packets.load());
    } else {
        ESP_LOGE("Audio", "AUDIO_REPLY_FAILED: %s; reset to retry", reply.failure.load());
    }
    return ok;
}
