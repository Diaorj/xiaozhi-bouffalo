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
    std::atomic<unsigned> packets{0};
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
        if (!reply.ended)
            reply.Fail("connection closed before TTS stop");
        ESP_LOGW("WS", "ORIGINAL_WS_CLOSED: reset to reconnect");
    });
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
        if (std::strcmp(type->valuestring, "tts") == 0) {
            auto* state = cJSON_GetObjectItem(root, "state");
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
        protocol.CloseAudioChannel(false);
        return false;
    }
    audio.Initialize(Board::GetInstance().GetAudioCodec(), std::move(decoder));
    audio.Start();
    if (!audio.IsRunning()) {
        ESP_LOGE("WS", "AUDIO_WORKER_START_FAILED");
        protocol.CloseAudioChannel(false);
        return false;
    }
    MemoryDiagnosticsStage("audio_service_tasks_created");
    reply.ended = false;
    reply.failure = nullptr;
    reply.packets = 0;
    const TickType_t started = xTaskGetTickCount();
    reply.last_packet = started;
    reply.active = true;
    // Official listen/detect accepts a short wake phrase, not an arbitrary question.
    protocol.SendWakeWordDetected("你好小智");
    ESP_LOGI("WS", "WAKE_GREETING_SENT: standard short wake phrase; volume=15; AudioService");

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
        if (!reply.ended && now - reply.last_packet.load() > pdMS_TO_TICKS(15000)) {
            reply.Fail(reply.packets ? "audio stalled" : "no audio received within 15s");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ok = ok && !reply.failure.load();
    reply.active = false;
    MemoryDiagnosticsStage("before_audio_service_stop");
    audio.Stop();  // Joins both original workers before abort/free on a failure.
    if (ok) {
        ESP_LOGI("Audio", "AUDIO_REPLY_DONE: AudioService packets=%u; speaker drained and muted",
                 reply.packets.load());
    } else {
        ESP_LOGE("Audio", "AUDIO_REPLY_FAILED: %s; reset to retry", reply.failure.load());
    }
    return ok;
}
