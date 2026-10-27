#include "http_bouffalo.h"
#include "tcp_bouffalo.h"
#include <new>
std::unique_ptr<Tcp> BouffaloNetwork::CreateSsl(int) {
    return std::unique_ptr<Tcp>(new (std::nothrow) BouffaloSsl);
}
std::unique_ptr<WebSocket> BouffaloNetwork::CreateWebSocket(int connect_id) {
    return std::unique_ptr<WebSocket>(new (std::nothrow) WebSocket(this, connect_id));
}
