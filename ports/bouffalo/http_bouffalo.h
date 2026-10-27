#pragma once
#include <http.h>
#include <network_interface.h>
#include <map>
class BouffaloHttp final : public Http {
public:
    void SetTimeout(int ms) override { timeout_ = ms; }
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override { content_ = std::move(content); }
    void SetKeepAlive(bool) override {}  // This synchronous backend always closes its transport.
    NetworkResult<> Open(const std::string& method, const std::string& url) override;
    void Close() override { opened_ = false; body_.clear(); offset_ = 0; }
    NetworkResult<int> Read(char* output, size_t size) override;
    NetworkResult<int> Write(const char*, size_t) override { return FailValue<int>(NetworkError::InvalidArgument()); }
    NetworkResult<int> GetStatusCode() override;
    std::string GetResponseHeader(const std::string&) const override { return {}; }
    size_t GetBodyLength() override { return body_.size(); }
    std::string ReadAll() override;
private:
    std::map<std::string, std::string> headers_;
    std::string content_, body_;
    size_t offset_ = 0;
    int timeout_ = 10000, status_ = 0;
    bool opened_ = false, invalid_header_ = false;
};
class BouffaloNetwork final : public NetworkInterface {
public:
    std::unique_ptr<Http> CreateHttp(int = -1) override;
    std::unique_ptr<Tcp> CreateTcp(int = -1) override { return nullptr; }
    std::unique_ptr<Tcp> CreateSsl(int = -1) override;
    std::unique_ptr<Udp> CreateUdp(int = -1) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int = -1) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int = -1) override;
};
