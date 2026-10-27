#include "http_bouffalo.h"
#include "http_sdk_bridge.h"
#include "ota_ca.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>
void BouffaloHttp::SetHeader(const std::string& key, const std::string& value) {
    if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos || value.size() > 1024 || headers_.size() >= 16) {
        invalid_header_ = true; return;
    }
    headers_[key] = value;
}
NetworkResult<> BouffaloHttp::Open(const std::string& method, const std::string& url) {
    Close();
    if (invalid_header_ || timeout_ <= 0 || timeout_ > 60000 || (method != "POST" && method != "GET") ||
        (url != "https://api.tenclass.net/xiaozhi/ota/" && url != "https://api.tenclass.net/xiaozhi/ota/activate") || content_.size() > 4096) {
        return Fail(NetworkError::InvalidArgument());
    }
    std::vector<std::string> lines;
    lines.emplace_back("Connection: close\r\n");
    lines.emplace_back("Accept-Encoding: identity\r\n");
    for (const auto& [key, value] : headers_) {
        if (key == "Content-Type") {
            if (value != "application/json") return Fail(NetworkError::InvalidArgument());
        } else if (key != "Connection" && key != "Accept-Encoding") {
            lines.push_back(key + ": " + value + "\r\n");
        }
    }
    std::vector<const char*> pointers;
    for (const auto& line : lines) pointers.push_back(line.c_str());
    pointers.push_back(nullptr);
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[8193]);
    if (!buffer) return Fail(NetworkError::Unknown());
    const http_sdk_json_post_t request = {url.c_str(), ota_ca, sizeof(ota_ca), pointers.data(), content_.c_str(), content_.size()};
    http_sdk_response_t response = {};
    const int result = method == "POST" ? HttpSdkVerifiedPostJson(&request, timeout_, buffer.get(), 8193, &response)
                                        : HttpSdkVerifiedGetBody(&request, timeout_, buffer.get(), 8193, &response);
    if (result < 0 || response.overflow || !response.complete) return Fail(NetworkError::ReceiveFailed(result));
    status_ = response.status;
    body_.assign(buffer.get(), response.body_size);
    opened_ = true;
    return {};
}
NetworkResult<int> BouffaloHttp::GetStatusCode() {
    if (!opened_) return FailValue<int>(NetworkError::NotInitialized());
    return status_;
}
NetworkResult<int> BouffaloHttp::Read(char* output, size_t size) {
    if (!opened_) return FailValue<int>(NetworkError::NotInitialized());
    if (!output && size) return FailValue<int>(NetworkError::InvalidArgument());
    size_t count = std::min(size, body_.size() - offset_);
    if (count) std::memcpy(output, body_.data() + offset_, count);
    offset_ += count;
    return static_cast<int>(count);
}
std::string BouffaloHttp::ReadAll() {
    if (!opened_) return {};
    std::string remaining = body_.substr(offset_); offset_ = body_.size(); return remaining;
}
std::unique_ptr<Http> BouffaloNetwork::CreateHttp(int) {
    return std::unique_ptr<Http>(new (std::nothrow) BouffaloHttp);
}
const char* NetworkError::Name() const {
    static const char* names[] = {"InvalidArgument","NotInitialized","Timeout","DnsFailed","ConnectFailed","TlsFailed","ProtocolError","AuthRejected","ServerDisconnected","NetworkUnavailable","TransmitFailed","ReceiveFailed","AtCommandFailed","CmeError","HttpFailed","Unknown"};
    const size_t index = static_cast<size_t>(code);
    return index < sizeof(names)/sizeof(names[0]) ? names[index] : "Unknown";
}
const char* NetworkError::Message() const { return Name(); }
std::string NetworkError::ToString() const { return std::string(Name()) + " (" + std::to_string(native) + ")"; }
