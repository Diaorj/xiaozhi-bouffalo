#include "tcp_bouffalo.h"
#include "tls_sdk_bridge.h"
#include <algorithm>
#include <climits>
#include <esp_log.h>

BouffaloSsl::BouffaloSsl() : finished_(xEventGroupCreate()) {}
BouffaloSsl::~BouffaloSsl() {
    Disconnect();
    if (finished_) vEventGroupDelete(finished_);
}
NetworkResult<> BouffaloSsl::Connect(const std::string& host, int port) {
    Disconnect();
    if (!finished_) return Fail(NetworkError::Unknown());
    if (host.empty() || host.size() > 253 || host.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos ||
        port < 1 || port > 65535) return Fail(NetworkError::InvalidArgument());
    ESP_LOGI("TLS", "WSS_TLS_BEGIN: %s:%d; verify CA, hostname and dates", host.c_str(), port);
    tls_ = TlsSdkConnect(host.c_str(), port);
    if (!tls_) return Fail(NetworkError::TlsFailed());
    stop_ = false;
    connected_ = true;
    ESP_LOGI("TLS", "WSS_TLS_OK");
    return {};
}
int BouffaloSsl::Send(const std::string& data) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!tls_ || stop_ || data.size() > INT_MAX) return -1;
    size_t sent = 0;
    while (sent < data.size()) {
        int result = TlsSdkWrite(tls_, data.data() + sent, std::min(size_t(4096), data.size() - sent));
        if (result <= 0 || size_t(result) > data.size() - sent) {
            stop_ = true;
            return -1;  // Tcp::Send succeeds only after the entire frame is sent.
        }
        sent += result;
    }
    // Upstream WebSocket registers OnStream after Connect, before its first Send.
    if (!receiver_started_) {
        xEventGroupClearBits(finished_, 1);
        if (xTaskCreate(ReceiveEntry, "xiaozhi_wss", 3072, this, 10, &receiver_task_) != pdPASS) {
            stop_ = true;
            return -1;
        }
        receiver_started_ = true;
    }
    return static_cast<int>(sent);
}
void BouffaloSsl::ReceiveEntry(void* context) {
    auto self = static_cast<BouffaloSsl*>(context);
    self->ReceiveLoop();
    xEventGroupSetBits(self->finished_, 1);
    vTaskDelete(nullptr);
}
void BouffaloSsl::ReceiveLoop() {
    char buffer[1024];
    while (!stop_) {
        int count;
        {
            std::lock_guard<std::mutex> lock(io_mutex_);
            count = stop_ ? -1 : TlsSdkRead(tls_, buffer, sizeof(buffer));
        }
        // Release the TLS lock before calling WebSocket: Ping may send Pong here.
        if (count > 0) {
            if (stream_callback_) stream_callback_(std::string(buffer, count));
        } else if (count != -2) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    const bool remote = !stop_.exchange(true);
    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        connected_ = false;
        TlsSdkClose(tls_);
        tls_ = nullptr;
    }
    if (remote && disconnect_callback_) disconnect_callback_();
}
void BouffaloSsl::Disconnect() {
    stop_ = true;
    // A frame error/close can request stop from inside OnStream. The receiver
    // must not join itself; it closes TLS on exit. Only the owner destroys it.
    if (receiver_started_ && xTaskGetCurrentTaskHandle() == receiver_task_) return;
    if (receiver_started_) {
        xEventGroupWaitBits(finished_, 1, pdFALSE, pdTRUE, portMAX_DELAY);
        receiver_started_ = false;
        receiver_task_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(io_mutex_);
    connected_ = false;
    TlsSdkClose(tls_);
    tls_ = nullptr;
}
