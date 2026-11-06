// The built-in link to the online lobbies (core/relay_link.h). The in-game form of tools/multiplayer/proxy/proxy.py,
// speaking the same protocol to the same server, so players need nothing but the game.
//
// Local hop (xnet.cpp), both directions, little endian: "MC3R" | src port u16 | dst port u16 | peer u32 | payload.
// Peer 0 = everyone in the room. The older 8-byte "MC3P" header (magic | src | dst) from the game means peer 0.
// Server hop: JSON over one WebSocket. {"type":"join","clientType":"game","name":...} then enter_room / join_game /
// create_game; each datagram is {"type":"input","data":"{\"payload\":<base64>,\"src_port\":..,\"dst_port\":..}",
// "to":<client id>?}; arriving datagrams are "input" messages naming the sender's client id (fromId).
#include "core/relay_link.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/json_lite.h"
#include "core/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#endif

namespace relay {

#ifdef _WIN32
namespace {

constexpr uint16_t kProxyPort = 30741;  // xnet.cpp: instance N's proxy listens on kProxyPort + N
constexpr uint32_t kMaxPeer = 254;
constexpr size_t kMaxMessage = 64 * 1024;

std::mutex g_lock;  // status, rooms, wanted room, peers
Status g_status;
std::vector<Room> g_rooms;
enum class Want { Main, Private, Create, Join } g_want = Want::Main;
std::string g_want_arg;
std::map<std::string, uint32_t> g_peer_of;  // relay client id -> peer number
std::map<uint32_t, std::string> g_id_of;
sockaddr_in g_game{};                       // where the game's tunnelled datagrams come from
uint16_t g_game_port = 0;

std::atomic<bool> g_run{false};
std::thread g_ws_thread, g_udp_thread;
std::mutex g_send_lock;  // one WinHttpWebSocketSend at a time; receive runs alongside on the WebSocket thread
HINTERNET g_ws = nullptr;
std::atomic<bool> g_ready{false};  // joined a room: datagrams flow
std::atomic<bool> g_reconnect{false};  // Reconnect(): sign in again at once (a new name), no retry delay
SOCKET g_udp = INVALID_SOCKET;

void SetStatus(Status::State st, const std::string& msg) {
    std::lock_guard lock(g_lock);
    g_status.state = st;
    g_status.message = msg;
}

std::string HostOf(const std::string& url) {
    size_t a = url.find("://");
    a = a == std::string::npos ? 0 : a + 3;
    size_t b = url.find_first_of(":/", a);
    return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// ---------- base64 ----------
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string B64(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < n ? kB64[v >> 6 & 63] : '=';
        out += i + 2 < n ? kB64[v & 63] : '=';
    }
    return out;
}
bool Unb64(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const char* f = std::strchr(kB64, c);
        if (!f || !c) return false;
        v = v << 6 | uint32_t(f - kB64);
        if ((bits += 6) >= 8) {
            bits -= 8;
            out.push_back(uint8_t(v >> bits));
        }
    }
    return true;
}

// ---------- WebSocket ----------
bool Send(const std::string& text) {
    std::lock_guard lock(g_send_lock);
    if (!g_ws) return false;
    return WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
                                DWORD(text.size())) == NO_ERROR;
}

std::string PlayerName() {
    std::string n = app::Get().online.name;
    if (n.empty()) {
        char user[64] = {};
        DWORD len = sizeof user;
        n = GetUserNameA(user, &len) && user[0] ? user : "Player";
    }
    n.erase(std::remove_if(n.begin(), n.end(), [](char c) { return uint8_t(c) < 0x20; }), n.end());
    return n.substr(0, 32);
}

void SendRoomRequest() {
    Want want;
    std::string arg;
    {
        std::lock_guard lock(g_lock);
        want = g_want;
        arg = g_want_arg;
    }
    g_ready = false;
    switch (want) {
        case Want::Main: Send(R"({"type":"enter_room","name":""})"); break;
        case Want::Private: Send(R"({"type":"enter_room","name":)" + json::Quote(arg) + "}"); break;
        case Want::Create: Send(R"({"type":"create_game","name":)" + json::Quote(arg) + "}"); break;
        case Want::Join: Send(R"({"type":"join_game","gameId":)" + json::Quote(arg) + "}"); break;
    }
}

void ForgetPeers() {
    std::lock_guard lock(g_lock);
    g_peer_of.clear();
    g_id_of.clear();
    g_status.peers = 0;
}

uint32_t PeerFor(const std::string& id) {  // caller holds g_lock
    auto it = g_peer_of.find(id);
    if (it != g_peer_of.end()) return it->second;
    for (uint32_t p = 1; p <= kMaxPeer; p++)
        if (!g_id_of.count(p)) {
            g_peer_of[id] = p;
            g_id_of[p] = id;
            g_status.peers = uint32_t(g_peer_of.size());
            LOGI("relay: player %s is peer %u (127.0.1.%u)", id.c_str(), p, p);
            return p;
        }
    return 0;
}

void Deliver(const json::Value& msg) {
    json::Value inner = json::Parse(msg["data"].str());
    std::vector<uint8_t> payload;
    if (!Unb64(inner["payload"].str(), payload) || payload.empty() || payload.size() > 4096) return;
    double sp = inner["src_port"].num(), dp = inner["dst_port"].num();
    if (sp < 0 || sp > 0xFFFF || dp < 0 || dp > 0xFFFF) return;
    std::string from = msg["fromId"].str(msg["from"].str("?"));
    uint32_t peer;
    sockaddr_in to;
    {
        std::lock_guard lock(g_lock);
        peer = PeerFor(from);
        to = g_game;
        uint16_t port = uint16_t(dp) ? uint16_t(dp) : g_game_port;
        if (!peer || !port || !to.sin_family) return;
        to.sin_port = htons(port);
    }
    std::vector<uint8_t> pkt(12 + payload.size());
    std::memcpy(pkt.data(), "MC3R", 4);
    uint16_t s16 = uint16_t(sp), d16 = ntohs(to.sin_port);
    std::memcpy(&pkt[4], &s16, 2);
    std::memcpy(&pkt[6], &d16, 2);
    std::memcpy(&pkt[8], &peer, 4);
    std::memcpy(&pkt[12], payload.data(), payload.size());
    sendto(g_udp, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0, reinterpret_cast<const sockaddr*>(&to),
           sizeof to);
}

void OnMessage(const std::string& text) {
    json::Value m = json::Parse(text);
    std::string type = m["type"].str();
    if (type == "input") return Deliver(m);
    if (type == "auth_ok") {
        {
            std::lock_guard lock(g_lock);
            g_status.name = m["user"]["username"].str(PlayerName());
        }
        LOGR("relay: online as %s", m["user"]["username"].str().c_str());
        SendRoomRequest();
    } else if (type == "game_joined" || type == "game_created") {
        const json::Value& g = m["game"];
        ForgetPeers();
        {
            std::lock_guard lock(g_lock);
            g_status.state = Status::Online;
            g_status.room_id = g["id"].str();
            g_status.room_name = g["name"].str();
            g_status.in_main = g["main"].boolean();
            g_status.room_players = uint32_t(g["playerCount"].num());
            g_status.message = g_status.in_main ? "In the Main Lobby" : "In room " + g_status.room_name;
        }
        g_ready = true;
        LOGR("relay: %s (%u here)", g["main"].boolean() ? "in the Main Lobby" : ("in room " + g["name"].str()).c_str(),
             uint32_t(g["playerCount"].num()));
        Send(R"({"type":"list"})");
    } else if (type == "gameList") {
        std::vector<Room> rooms;
        for (const json::Value& g : m["games"].a) {
            Room r;
            r.id = g["id"].str();
            r.name = g["name"].str();
            r.host = g["host_username"].str();
            r.players = uint32_t(g["player_count"].num());
            r.max_players = uint32_t(g["max_players"].num());
            r.racing = g["status"].str() == "playing";
            r.main = g["main"].boolean() || r.id == "main";  // Main Lobby instances: Main Lobby, Main Lobby 2, ...
            if (!r.id.empty()) rooms.push_back(std::move(r));
        }
        std::stable_sort(rooms.begin(), rooms.end(), [](const Room& a, const Room& b) { return a.main > b.main; });
        std::lock_guard lock(g_lock);
        g_rooms = std::move(rooms);
        for (const Room& r : g_rooms)
            if (r.id == g_status.room_id) g_status.room_players = r.players;
    } else if (type == "queue") {
        // the server is full: the sign-in waits here until a place frees (auth_ok follows)
        std::lock_guard lock(g_lock);
        g_status.state = Status::Connecting;
        g_status.message = "Server full: you are " + std::to_string(int(m["pos"].num())) + " of " +
                           std::to_string(int(m["of"].num())) + " in line";
    } else if (type == "player_left") {
        std::lock_guard lock(g_lock);
        auto it = g_peer_of.find(m["id"].str());
        if (it != g_peer_of.end()) {
            g_id_of.erase(it->second);
            g_peer_of.erase(it);
            g_status.peers = uint32_t(g_peer_of.size());
        }
    } else if (type == "error") {
        std::string e = m["error"].str("error");
        LOGW("relay: server says %s", e.c_str());
        if (e.rfind("Already in", 0) == 0) {  // picking the room we are in: nothing changed
            g_ready = true;
            return;
        }
        std::lock_guard lock(g_lock);
        g_status.message = e;
    }
}

bool Connect(const std::string& url, HINTERNET& session, HINTERNET& connection) {
    std::wstring w(url.begin(), url.end());
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = {}, path[1024] = {};
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 1024;
    std::wstring http = w;
    if (http.rfind(L"wss://", 0) == 0) http = L"https://" + http.substr(6);
    else if (http.rfind(L"ws://", 0) == 0) http = L"http://" + http.substr(5);
    if (!WinHttpCrackUrl(http.c_str(), 0, 0, &u)) return false;
    bool secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    session = WinHttpOpen(L"MC3-PC/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 0);  // receive waits forever: the server pushes when it has news
    connection = WinHttpConnect(session, host, u.nPort, 0);
    if (!connection) return false;
    HINTERNET req = WinHttpOpenRequest(connection, L"GET", path[0] ? path : L"/ws", nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
        ok = code == 101;
        if (ok) {
            std::lock_guard lock(g_send_lock);
            g_ws = WinHttpWebSocketCompleteUpgrade(req, 0);
            ok = g_ws != nullptr;
        }
    }
    WinHttpCloseHandle(req);
    return ok;
}

void Session(const std::string& url) {
    HINTERNET session = nullptr, connection = nullptr;
    SetStatus(Status::Connecting, "Connecting to " + HostOf(url));
    bool connected = Connect(url, session, connection);
    if (connected) {
        LOGR("relay: connected to %s", url.c_str());
        SetStatus(Status::Connecting, "Signing in to " + HostOf(url));
        Send(R"({"type":"join","clientType":"game","name":)" + json::Quote(PlayerName()) + "}");
        std::string message;
        std::vector<char> buf(16 * 1024);
        while (g_run) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (WinHttpWebSocketReceive(g_ws, buf.data(), DWORD(buf.size()), &got, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            message.append(buf.data(), got);
            if (message.size() > kMaxMessage) break;
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                continue;
            OnMessage(message);
            message.clear();
        }
    } else {
        LOGW("relay: cannot reach %s (%lu)", url.c_str(), GetLastError());
    }
    g_ready = false;
    {
        std::lock_guard lock(g_send_lock);
        if (g_ws) {
            WinHttpWebSocketClose(g_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(g_ws);
            g_ws = nullptr;
        }
    }
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    ForgetPeers();
    std::lock_guard lock(g_lock);
    g_status.room_id.clear();
    g_status.room_name.clear();
    g_rooms.clear();
}

void WsThread() {
    int delay = 1;
    while (g_run) {
        Session(app::Get().online.relay);
        if (!g_run) break;
        if (g_reconnect.exchange(false)) {
            delay = 1;
            continue;
        }
        SetStatus(Status::Failed, "Server unreachable, retrying in " + std::to_string(delay) + " s");
        for (int i = 0; i < delay * 10 && g_run; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        delay = std::min(delay * 2, 30);
    }
    SetStatus(Status::Off, "Offline");
}

// ---------- game -> server ----------
void UdpThread() {
    std::vector<uint8_t> buf(65536);
    while (g_run) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(g_udp, &r);
        timeval tv{0, 250000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in from{};
        int fl = sizeof from;
        int n = recvfrom(g_udp, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        uint16_t src = 0, dst = 0;
        uint32_t peer = 0;
        size_t head;
        if (n >= 12 && !std::memcmp(buf.data(), "MC3R", 4)) {
            std::memcpy(&src, &buf[4], 2), std::memcpy(&dst, &buf[6], 2), std::memcpy(&peer, &buf[8], 4);
            head = 12;
        } else if (n >= 8 && !std::memcmp(buf.data(), "MC3P", 4)) {
            std::memcpy(&src, &buf[4], 2), std::memcpy(&dst, &buf[6], 2);
            head = 8;
        } else continue;  // not the game's tunnel
        std::string to;
        {
            std::lock_guard lock(g_lock);
            g_game = from;
            g_game_port = src;
            if (peer) {
                auto it = g_id_of.find(peer);
                if (it == g_id_of.end()) continue;  // that player left
                to = it->second;
            }
        }
        if (!g_ready || size_t(n) == head) continue;
        std::string inner = R"({"payload":")" + B64(buf.data() + head, size_t(n) - head) + R"(","src_port":)" +
                            std::to_string(src) + R"(,"dst_port":)" + std::to_string(dst) + "}";
        std::string env = R"({"type":"input","data":)" + json::Quote(inner) + (to.empty() ? "" : R"(,"to":)" + json::Quote(to)) + "}";
        Send(env);
    }
}

void Want_(Want w, const std::string& arg) {
    {
        std::lock_guard lock(g_lock);
        g_want = w;
        g_want_arg = arg;
    }
    SendRoomRequest();  // sent now when connected; otherwise on the next sign in
}

}  // namespace

void Start() {
    if (g_run.exchange(true)) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    at.sin_port = htons(uint16_t(kProxyPort + app::Instance()));
    if (g_udp == INVALID_SOCKET || bind(g_udp, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0) {
        // another tunnel (proxy.py) already serves this instance: leave it to that one
        LOGW("relay: 127.0.0.1:%u busy, the game uses the tunnel already there", ntohs(at.sin_port));
        if (g_udp != INVALID_SOCKET) closesocket(g_udp);
        g_udp = INVALID_SOCKET;
        SetStatus(Status::Failed, "Another online tunnel is already running for this game");
        g_run = false;
        return;
    }
    {
        std::lock_guard lock(g_lock);
        const std::string& room = app::Get().online.room;
        g_want = room.empty() ? Want::Main : Want::Private;
        g_want_arg = room;
    }
    LOGR("relay: built-in online link on 127.0.0.1:%u -> %s", ntohs(at.sin_port), app::Get().online.relay.c_str());
    g_ws_thread = std::thread(WsThread);
    g_udp_thread = std::thread(UdpThread);
}

void Stop() {
    if (!g_run.exchange(false)) return;
    {
        std::lock_guard lock(g_send_lock);
        if (g_ws) WinHttpWebSocketShutdown(g_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    if (g_udp_thread.joinable()) g_udp_thread.join();
    if (g_ws_thread.joinable()) g_ws_thread.detach();  // a blocked receive ends with the shutdown above
    closesocket(g_udp);
    g_udp = INVALID_SOCKET;
}

void Reconnect() {
    if (!g_run) return;
    g_reconnect = true;
    std::lock_guard lock(g_send_lock);
    if (g_ws) WinHttpWebSocketShutdown(g_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}

bool Running() { return g_run; }
Status Current() {
    std::lock_guard lock(g_lock);
    return g_status;
}
std::vector<Room> Rooms() {
    std::lock_guard lock(g_lock);
    return g_rooms;
}
void Refresh() {
    Send(R"({"type":"list"})");
}
void EnterMain() { Want_(Want::Main, {}); }
void EnterPrivate(const std::string& name) { Want_(Want::Private, name); }
void CreateRoom(const std::string& name) { Want_(Want::Create, name); }
void JoinRoom(const std::string& id) { Want_(Want::Join, id); }

#else  // the browser build links players through the page instead (docs/notes/browser_port_plan.md)

void Start() {}
void Stop() {}
void Reconnect() {}
bool Running() { return false; }
Status Current() { return {}; }
std::vector<Room> Rooms() { return {}; }
void Refresh() {}
void EnterMain() {}
void EnterPrivate(const std::string&) {}
void CreateRoom(const std::string&) {}
void JoinRoom(const std::string&) {}

#endif

}  // namespace relay
