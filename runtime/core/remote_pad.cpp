// Loopback UDP controller reports (see remote_pad.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/remote_pad.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/log.h"

namespace input {

namespace {

std::mutex g_remote_lock;
Pad g_remote[4];
uint64_t g_remote_ms[4] = {};
std::once_flag g_remote_once;
// MC3_REMOTE_PRIMARY=1 (a copy run by tools/multiplayer/playhost for one browser player, with no controller of its
// own): the stream proxy hands browsers ports 1-3, so they shift down one and the browser player is player 1
int g_remote_shift = 0;

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Listen(SOCKET s) {
    uint8_t buf[64];
    for (;;) {
        sockaddr_in from{};
        int fl = sizeof(from);
        int n = ::recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;
            LOGW("input: remote pad listener stopped (%d)", WSAGetLastError());
            return;
        }
        int port;
        Pad pad;
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || !ParseRemotePad(buf, n, port, pad)) continue;
        std::lock_guard lock(g_remote_lock);
        if (!g_remote[port].connected && pad.connected) LOGI("input: remote controller on port %d", port + 1);
        g_remote[port] = pad;
        g_remote_ms[port] = NowMs();
    }
}

}  // namespace

void StartRemotePads() {
    std::call_once(g_remote_once, [] {
        const char* primary = std::getenv("MC3_REMOTE_PRIMARY");
        g_remote_shift = primary && primary[0] == '1' ? 1 : 0;
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        uint16_t port = uint16_t(kRemotePadPort + app::Instance());
        at.sin_port = htons(port);
        if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
            LOGW("input: remote controllers unavailable, 127.0.0.1:%u busy (%d)", port, WSAGetLastError());
            if (s != INVALID_SOCKET) ::closesocket(s);
            return;
        }
        LOGI("input: remote controllers on 127.0.0.1:%u%s", port, g_remote_shift ? ", browser is player 1" : "");
        std::thread(Listen, s).detach();
    });
}

Pad RemotePad(int port) {
    port += g_remote_shift;
    if (port < 0 || port > 3) return {};
    std::lock_guard lock(g_remote_lock);
    if (!g_remote[port].connected || NowMs() - g_remote_ms[port] > kRemotePadStaleMs) return {};
    return g_remote[port];
}

}  // namespace input
