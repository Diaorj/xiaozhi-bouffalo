#pragma once
#include <tcp.h>
#include <atomic>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

class BouffaloSsl final : public Tcp {
public:
    BouffaloSsl();
    ~BouffaloSsl() override;
    NetworkResult<> Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;
private:
    static void ReceiveEntry(void* context);
    void ReceiveLoop();
    std::mutex io_mutex_;
    void* tls_ = nullptr;
    EventGroupHandle_t finished_ = nullptr;
    std::atomic<uint32_t> stop_{1};
    bool receiver_started_ = false;
    TaskHandle_t receiver_task_ = nullptr;
};
