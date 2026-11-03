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
#if XIAOZHI_VOICE_INPUT_TEST
#include "voice_input.h"
#endif

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
    std::atomic<TickType_t> request_finished{0};
    std::atomic<const char*> failure{nullptr};
    void Fail(const char* reason) {
        if (!active)
            return;
        const char* empty = nullptr;
        failure.compare_exchange_strong(empty, reason);
    }
};
}  // namespace

bool RunOriginalWebsocketHandshake(unsigned max_rounds, bool (*network_available)()) {
    // Static lifetime: the original protocol retains its callbacks after startup.
    static AudioService audio;
    static ReplyState reply;
    static WebsocketProtocol protocol;
    reply.active = false;
    reply.ended = false;
    reply.failure = nullptr;
    const auto online = [&] { return !network_available || network_available(); };
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
        ESP_LOGW("WS", "ORIGINAL_WS_CLOSED: conversation connection ended");
    });
    // The owner closes/joins the session on a terminal error or finite test exit.
    const auto close_session = [&] {
        reply.active = false;
        reply.closing = true;
        protocol.CloseAudioChannel(online());
        reply.closing = false;
        ESP_LOGI("WS", "WS_SESSION_RELEASED: socket and receiver released");
        MemoryDiagnosticsStage("after_ws_session_close");
    };
    protocol.OnIncomingAudio([](std::unique_ptr<AudioStreamPacket> packet) {
        if (!reply.active)
            return;
        if (reply.ended || !audio.PushPacketToDecodeQueue(std::move(packet), false)) {
            reply.Fail("audio queue rejected packet");
            return;
        }
        const unsigned packets = ++reply.packets;
        const TickType_t now = xTaskGetTickCount();
        reply.last_packet = now;
        if (packets == 1 && reply.request_finished.load())
            ESP_LOGI("WS", "REPLY_FIRST_AUDIO: after_request_ms=%u",
                     unsigned((now - reply.request_finished.load()) * portTICK_PERIOD_MS));
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
        } else if (std::strcmp(type->valuestring, "stt") == 0) {
            auto* text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text))
                ESP_LOGI("WS", "RECOGNIZED: %.512s", text->valuestring);
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
    auto* codec = Board::GetInstance().GetAudioCodec();
    audio.Initialize(codec);
    bool decoder_ready = false;
    const auto start_playback = [&] {
        if (!decoder_ready) {
            MemoryDiagnosticsStage("before_opus_state");
            auto decoder = CreateBouffaloOpusDecoder();
            if (!decoder)
                return false;
            audio.Initialize(codec, std::move(decoder));
            decoder_ready = true;
        }
        audio.Start();
        if (!audio.IsRunning()) {
            ESP_LOGE("WS", "AUDIO_WORKER_START_FAILED");
            return false;
        }
        MemoryDiagnosticsStage("audio_service_tasks_created");
        return true;
    };
    unsigned completed = 0;
#if XIAOZHI_AUTO_CHAT && XIAOZHI_VOICE_INPUT_TEST
    const unsigned limit = max_rounds;
#else
    const unsigned limit = max_rounds ? max_rounds : 1;
#endif
    for (;;) {
        if (!online()) {
            close_session();
            return false;
        }
#if XIAOZHI_VOICE_INPUT_TEST
        VoiceInputSession input(audio, codec);
        if (!input.Prepare()) {
            ESP_LOGE("VOICE", "VOICE_INPUT_NOT_READY: returning to network owner");
            close_session();
            return false;
        }
#endif
        if (completed == 0) {
            const TickType_t connect_started = xTaskGetTickCount();
            if (!protocol.Start() || !protocol.OpenAudioChannel()) {
                protocol.CloseAudioChannel(false);
                ESP_LOGW("WS", "ORIGINAL_WS_NOT_READY: returning to network owner");
                return false;
            }
            ESP_LOGI("WS", "ORIGINAL_WS_OK: authenticated; server hello received; "
                           "rate=%d frame=%d ms connect_ms=%u",
                     protocol.server_sample_rate(), protocol.server_frame_duration(),
                     unsigned((xTaskGetTickCount() - connect_started) * portTICK_PERIOD_MS));
        }
        reply.ended = false;
        reply.failure = nullptr;
        reply.packets = 0;
        reply.json_messages = 0;
        reply.tts_started = false;
        reply.request_finished = 0;
        reply.active = true;
#if XIAOZHI_VOICE_INPUT_TEST
        TickType_t first_upload = 0;
        size_t sent = 0, sent_bytes = 0;
        bool listen_started = false;
        std::vector<int16_t> pending_frame;
        if (!online()) {
            close_session();
            return false;
        }
        if (!input.StartCapture())
            reply.Fail("capture worker start failed");
        for (;;) {
            if (!online()) {
                reply.Fail("Wi-Fi link/IP lost while listening; interrupted audio discarded");
                break;
            }
            if (input.Failure() || audio.HasEncodingError()) {
                reply.Fail(input.Failure() ? input.Failure() : "voice encoder/send queue failed");
                break;
            }
            if (reply.failure.load() || !protocol.Heartbeat() || !protocol.IsAudioChannelOpened()) {
                reply.Fail("voice connection closed while listening");
                break;
            }
            if (input.VoiceStarted() && !listen_started) {
                protocol.SendStartListening(kListeningModeManualStop);
                listen_started = true;
            }
            if (listen_started) {
                if (pending_frame.empty())
                    input.PopPcmFrame(pending_frame);
                if (!pending_frame.empty())
                    audio.PushPcmToEncodeQueue(std::move(pending_frame));
                while (auto packet = audio.PopPacketFromSendQueue()) {
                    const size_t bytes = packet->payload.size();
                    if (!protocol.SendAudio(std::move(packet))) {
                        reply.Fail("voice upload failed");
                        break;
                    }
                    if (sent++ == 0) {
                        first_upload = xTaskGetTickCount();
                        ESP_LOGI("VOICE", "VOICE_FIRST_UPLOAD: from_voice_start_ms=%u",
                                 unsigned((first_upload - input.RecordingStartedAt()) * portTICK_PERIOD_MS));
                    }
                    sent_bytes += bytes;
                }
            }
            if (reply.failure.load())
                break;
            if (input.CaptureDone() && pending_frame.empty() && audio.IsEncodingIdle()) {
                if (input.Failure() || audio.HasEncodingError()) {
                    reply.Fail("voice capture/encode incomplete");
                    break;
                }
                if (sent > 0 && sent == input.ExpectedPackets())
                    break;
            }
            // No idle deadline. Once speech starts, bound utterance + upload tail.
            if (input.VoiceStarted() &&
                xTaskGetTickCount() - input.RecordingStartedAt() > pdMS_TO_TICKS(15000)) {
                reply.Fail("voice capture/upload timeout");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        const size_t recorded_samples = input.SamplesRecorded();
        input.Stop();  // Producer and encoder are joined before playback begins.
        if (reply.failure.load()) {
            ESP_LOGE("VOICE", "VOICE_STREAM_FAILED: %s; sent_packets=%u", reply.failure.load(),
                     unsigned(sent));
            close_session();
            return false;
        }
        const TickType_t uploaded_at = xTaskGetTickCount();
        ESP_LOGI("VOICE", "VOICE_TIMING: audio_ms=%u first_upload_ms=%u upload_tail_ms=%u",
                 unsigned(recorded_samples * 1000 / 16000),
                 unsigned((first_upload - input.RecordingStartedAt()) * portTICK_PERIOD_MS),
                 unsigned((uploaded_at - input.RecordingEndedAt()) * portTICK_PERIOD_MS));
        if (!start_playback()) {
            close_session();
            return false;
        }
        reply.request_finished = xTaskGetTickCount();
        protocol.SendStopListening();
        ESP_LOGI("VOICE", "VOICE_UPLOADED: packets=%u bytes=%u; waiting for recognition and reply",
                 unsigned(sent), unsigned(sent_bytes));
#else
        if (!start_playback()) {
            close_session();
            return false;
        }
        reply.request_finished = xTaskGetTickCount();
        protocol.SendWakeWordDetected("你好小智");
        ESP_LOGI("WS", "WAKE_GREETING_REQUESTED: standard short wake phrase; first audio timeout=30s");
#endif
        const TickType_t started = xTaskGetTickCount();
        reply.last_packet = started;
        bool ok = false;
        for (;;) {
            if (!online()) {
                reply.Fail("Wi-Fi link/IP lost during reply");
                break;
            }
            if (reply.failure.load() || (!reply.ended && !protocol.Heartbeat()))
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
            const TickType_t silence_limit = pdMS_TO_TICKS(reply.packets.load() ? 15000 : 30000);
            if (!reply.ended && now - reply.last_packet.load() > silence_limit) {
                reply.Fail(reply.packets ? "audio stalled" : "no audio received within 30s");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        ESP_LOGI("WS", "REPLY_SUMMARY: round=%u audio_packets=%u json_messages=%u tts_started=%d",
                 completed + 1, reply.packets.load(), reply.json_messages.load(),
                 int(reply.tts_started.load()));
        ok = ok && !reply.failure.load();
        reply.active = false;
        MemoryDiagnosticsStage("before_audio_service_stop");
        audio.Stop();
        vTaskDelay(pdMS_TO_TICKS(20));  // Reclaim dynamic playback stacks before another round.
        if (!ok) {
            close_session();
            ESP_LOGE("Audio", "AUDIO_REPLY_FAILED: %s; returning to network owner",
                     reply.failure.load() ? reply.failure.load() : "connection or playback failed");
            return false;
        }
        ++completed;
        ESP_LOGI("Audio", "AUDIO_REPLY_DONE: round=%u packets=%u; speaker drained and muted",
                 completed, reply.packets.load());
        if (limit && completed >= limit) {
            close_session();
            return true;
        }
        ESP_LOGI("VOICE", "AUTO_LISTEN_RESUME: round=%u completed; same server session", completed);
        vTaskDelay(pdMS_TO_TICKS(300));  // Let the last speaker sound decay before ADC/VAD calibration.
    }
}
