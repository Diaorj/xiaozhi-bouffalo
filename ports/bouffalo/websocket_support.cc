#include "websocket_support.h"
#include <algorithm>
#include <cctype>
#include <map>

bool ParseSecureWebsocketUri(const char* uri, std::string& host, std::string& port, std::string& path) {
    if (!uri) return false;
    const std::string url(uri);
    if (url.size() > 2048 || url.compare(0, 6, "wss://") != 0 ||
        url.find_first_of("\r\n\t #") != std::string::npos) return false;
    const auto end = url.find_first_of("/?", 6);
    const auto authority = url.substr(6, end == std::string::npos ? end : end - 6);
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    port = colon == std::string::npos ? "443" : authority.substr(colon + 1);
    if (host.empty() || host.size() > 253 || host.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos ||
        port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
    const auto number = std::stoul(port);  // <=5 validated decimal digits, cannot throw.
    if (number == 0 || number > 65535) return false;
    path = end == std::string::npos ? "/" : url.substr(end);
    if (path.front() == '?') path.insert(0, "/");
    return true;
}
namespace {
std::string Lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t") - first + 1);
}
bool HasToken(const std::string& value, const std::string& token) {
    size_t start = 0;
    do {
        const auto end = value.find(',', start);
        if (Lower(Trim(value.substr(start, end - start))) == token) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start < value.size());
    return false;
}
}
bool VerifyWebsocketUpgrade(const std::string& response, const std::string& client_key) {
    auto line_end = response.find("\r\n");
    if (line_end == std::string::npos) return false;
    const auto status = response.substr(0, line_end);
    if (status != "HTTP/1.1 101" && status.compare(0, 13, "HTTP/1.1 101 ") != 0) return false;
    std::map<std::string, std::string> headers;
    for (size_t start = line_end + 2; start < response.size();) {
        line_end = response.find("\r\n", start);
        if (line_end == std::string::npos) return false;
        if (line_end == start) break;
        const auto line = response.substr(start, line_end - start);
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        auto name = Lower(line.substr(0, colon));
        if (headers.count(name)) return false;
        headers.emplace(name, Trim(line.substr(colon + 1)));
        start = line_end + 2;
    }
    if (!HasToken(headers["upgrade"], "websocket") || !HasToken(headers["connection"], "upgrade")) return false;
    auto input = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20];
    if (WebsocketSha1(input.data(), input.size(), hash) != 0) return false;
    constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (size_t i = 0; i < sizeof(hash); i += 3) {
        unsigned v = unsigned(hash[i]) << 16;
        if (i + 1 < sizeof(hash)) v |= unsigned(hash[i + 1]) << 8;
        if (i + 2 < sizeof(hash)) v |= hash[i + 2];
        encoded += table[(v >> 18) & 63]; encoded += table[(v >> 12) & 63];
        encoded += i + 1 < sizeof(hash) ? table[(v >> 6) & 63] : '=';
        encoded += i + 2 < sizeof(hash) ? table[v & 63] : '=';
    }
    return headers["sec-websocket-accept"] == encoded;
}
