"""Generate a bounded C++17 WebSocket backend from the pinned, pristine source.
Only used by the Bouffalo build. Anchor checks stop on dependency drift.
"""
from pathlib import Path
import sys

def replace_once(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError("WebSocket upstream changed near: " + old[:90])
    return text.replace(old, new)

def prepare(source, destination):
    text = Path(source).read_text()
    text = replace_once(text, '#include <esp_pthread.h>', '#include "websocket_support.h"\n#include <freertos/task.h>')
    text = replace_once(text, '    if (connected_) {\n        tcp_->Disconnect();\n    }',
        '    if (tcp_) {\n        tcp_->Disconnect();\n    }')
    begin = text.index('    std::string uri_str(uri);')
    end = text.index('    uint32_t t_connect', begin)
    text = text[:begin] + r'''    std::string protocol = "wss", host, port, path;
    if (!handshake_event_group_ || !ParseSecureWebsocketUri(uri, host, port, path)) {
        return Fail(NetworkError::InvalidArgument());
    }
    if (tcp_) tcp_->Disconnect();
    receive_buffer_.clear();
    current_message_.clear();
    handshake_completed_ = false;
    is_fragmented_ = continuation_ = false;
''' + text[end:]
    text = replace_once(text, '    for (int i = 0; i < 16; ++i) {\n        key[i] = rand() % 256;\n    }',
        '    if (WebsocketRandom(key, 16) != 0) return Fail(NetworkError::Unknown());')
    text = replace_once(text, '    connected_ = false;\n    // 使用 tcp 建立连接',
        '    connected_ = false;\n    if (!tcp_) return Fail(NetworkError::NotInitialized());\n    // 使用 tcp 建立连接')
    text = replace_once(text, 'request += "Host: " + host + "\\r\\n";',
        'request += "Host: " + host + (port == "443" ? "" : ":" + port) + "\\r\\n";')
    text = replace_once(text, '    if (bits & HANDSHAKE_SUCCESS_BIT) {',
        '    if ((bits & HANDSHAKE_SUCCESS_BIT) && !(bits & HANDSHAKE_FAILED_BIT) && connected_) {')
    text = replace_once(text, '        connected_ = true;\n        if (on_connected_) {',
        '        if (on_connected_) {')
    text = replace_once(text, '    tcp_->OnDisconnected([this]() {',
        '    tcp_->OnDisconnected([this]() {\n        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);')
    text = replace_once(text, '    if (len > 65535) {', '    if (!tcp_ || !connected_ || (!data && len) || len > 65535) {')
    text = replace_once(text, '    for (int i = 0; i < 4; ++i) {\n        mask[i] = rand() & 0xFF;\n    }',
        '    if (WebsocketRandom(mask, sizeof(mask)) != 0) return false;', count=2)
    text = replace_once(text, '    if (len > 125) {', '    if (!tcp_ || !connected_ || (!data && len) || len > 125) {')
    text = replace_once(text, '    receive_buffer_.append(data);', r'''    // 16 KiB message limit plus one receive chunk; prevents unbounded allocation.
    if (data.size() > 17408 || receive_buffer_.size() > 17408 - data.size()) {
        connected_ = false;
        xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
        if (on_error_) on_error_(NetworkError::ProtocolError());
        tcp_->Disconnect();
        return;
    }
    receive_buffer_.append(data);''')
    text = replace_once(text, '    receive_buffer_.append(data);',
        '    if (xEventGroupGetBits(handshake_event_group_) & HANDSHAKE_FAILED_BIT) return;\n    receive_buffer_.append(data);')
    text = replace_once(text, '            if (handshake_response.find("HTTP/1.1 101") != std::string::npos) {',
        '            if (VerifyWebsocketUpgrade(handshake_response, headers_.at("Sec-WebSocket-Key"))) {\n                connected_ = true;')
    text = replace_once(text, '        uint8_t mask_key[4] = {0};', r'''        if (payload_length > 16384 || (buffer[buffer_offset] & 0x70) || mask ||
            (opcode >= 8 && (!fin || payload_length > 125)) ||
            (opcode == 0 && !is_fragmented_) ||
            (opcode != 0 && opcode < 8 && is_fragmented_) ||
            (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) ||
            (opcode == 8 && payload_length == 1) ||
            (opcode < 8 && current_message_.size() > 16384 - payload_length)) {
            connected_ = false;
            receive_buffer_.clear();
            xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);
            if (on_error_) on_error_(NetworkError::ProtocolError());
            tcp_->Disconnect();
            return;
        }
        uint8_t mask_key[4] = {0};''')
    text = replace_once(text, '                connected_ = false;\n                if (on_disconnected_) {',
        '                connected_ = false;\n                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);\n                if (on_disconnected_) {')
    text = replace_once(text, '                break;\n            case 0x9: // Ping',
        '                receive_buffer_.clear();\n                tcp_->Disconnect();\n                return;\n            case 0x9: // Ping')
    text = replace_once(text, '                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);\n                return;',
        '                xEventGroupSetBits(handshake_event_group_, HANDSHAKE_FAILED_BIT);\n                tcp_->Disconnect();\n                return;')
    Path(destination).write_text(text)

if __name__ == '__main__':
    prepare(sys.argv[1], sys.argv[2])
