// The built-in link to the online lobbies (core/relay_link.h). The in-game form of tools/multiplayer/proxy/proxy.py,
// speaking the same protocol to the same server, so players need nothing but the game.
//
// Local hop (xnet.cpp), both directions, little endian: "MC3R" | src port u16 | dst port u16 | peer u32 | payload.
// Peer 0 = everyone in the room. The older 8-byte "MC3P" header (magic | src | dst) from the game means peer 0.
// Server hop: JSON over one WebSocket. {"type":"join","clientType":"game","name":...} then enter_room / join_game /
// create_game; each datagram is {"type":"input","data":"{\"payload\":<base64>,\"src_port\":..,\"dst_port\":..}",
// "to":<client id>?}; arriving datagrams are "input" messages naming the sender's client id (fromId).
// Rooms on objects of their own (server room.js): game_joined names roomWs, a one-use address for the room's socket;
// datagrams then go there (in parallel with other rooms) and arrive on both sockets. Without it, the main socket.
#include "core/relay_link.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
// the room's own socket (roomWs), a thread of its own
std::mutex g_room_lock;
HINTERNET g_room_ws = nullptr;
std::thread g_room_thread;
std::atomic<bool> g_room_ready{false};
void StartRoom(const std::string& url);
std::string Origin(const std::string& url);
void StopRoom();
std::atomic<bool> g_reconnect{false};  // Reconnect(): sign in again at once (a new name), no retry delay
SOCKET g_udp = INVALID_SOCKET;

// ---------- accounts (proxy/account.py's protocol and store) ----------
std::string AccountBase() {  // wss://host/ws -> https://host
    std::string r = app::Get().online.relay;
    std::string scheme = r.rfind("wss://", 0) == 0 ? "https://" : "http://";
    size_t a = r.find("://");
    std::string rest = a == std::string::npos ? r : r.substr(a + 3);
    return scheme + rest.substr(0, rest.find('/'));
}

std::filesystem::path AccountFile() {
    const char* local = std::getenv("LOCALAPPDATA");
    std::filesystem::path root = local && *local ? std::filesystem::path(local) : std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".");
    return root / "MidnightClub3-Port" / "multiplayer" / "accounts.json";
}

json::Value LoadAccounts() {
    std::ifstream f(AccountFile(), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), {});
    json::Value v = json::Parse(text);
    return v.kind == json::Value::Object ? v : json::Value{json::Value::Object};
}

void SaveAccounts(const json::Value& all) {
    std::string out = "{";
    bool first = true;
    for (const auto& [base, e] : all.o) {
        if (e.kind != json::Value::Object) continue;
        out += (first ? "\n " : ",\n ") + json::Quote(base) + ": {\"username\": " + json::Quote(e["username"].str()) +
               ", \"device\": " + json::Quote(e["device"].str()) + "}";
        first = false;
    }
    out += "\n}\n";
    std::error_code ec;
    std::filesystem::create_directories(AccountFile().parent_path(), ec);
    std::filesystem::path tmp = AccountFile();
    tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << out; }
    std::filesystem::rename(tmp, AccountFile(), ec);
}

bool ValidDevice(const std::string& d) {
    if (d.size() != 43) return false;
    for (char c : d)
        if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '_')) return false;
    return true;
}

struct HttpResult {
    int status = 0;
    std::string body;
    std::vector<std::string> cookies;
};

// one HTTPS request to the account API, with the headers proxy/account.py sends
HttpResult Http(const std::string& method, const std::string& url, const std::string& body, const std::string& device) {
    HttpResult r;
    std::wstring w(url.begin(), url.end());
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = {}, path[1024] = {};
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &u)) return r;
    HINTERNET s = WinHttpOpen(L"mc3-game/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return r;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    std::wstring wmethod(method.begin(), method.end());
    HINTERNET q = c ? WinHttpOpenRequest(c, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (q) {
        std::string origin = AccountBase();
        std::string headers = "Content-Type: application/json\r\nOrigin: " + origin + "\r\n";
        if (!device.empty()) headers += "Cookie: mc3_device=" + device + "\r\n";
        std::wstring wh(headers.begin(), headers.end());
        WinHttpAddRequestHeaders(q, wh.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
        DWORD flags = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(q, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof flags);  // the device cookie is read by hand
        if (WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data(), DWORD(body.size()), DWORD(body.size()), 0) &&
            WinHttpReceiveResponse(q, nullptr)) {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
            r.status = int(code);
            for (DWORD index = 0;;) {
                wchar_t cookie[2048];
                DWORD len = sizeof cookie;
                if (!WinHttpQueryHeaders(q, WINHTTP_QUERY_SET_COOKIE, nullptr, cookie, &len, &index)) break;
                std::wstring wc(cookie, len / sizeof(wchar_t));
                r.cookies.emplace_back(wc.begin(), wc.end());
            }
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(q, &avail) || !avail || r.body.size() > 65536) break;
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(q, chunk.data(), avail, &got) || !got) break;
                r.body.append(chunk.data(), got);
            }
        }
        WinHttpCloseHandle(q);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

// the saved account for this server: {username, device}, or empty
std::pair<std::string, std::string> SavedAccount() {
    json::Value e = LoadAccounts()[AccountBase()];
    std::string device = e["device"].str();
    if (!ValidDevice(device)) return {};
    return {e["username"].str(), device};
}

// a fresh access token for the saved device sign-in, empty when not signed in (or it expired or was revoked)
std::string AccessToken() {
    auto [name, device] = SavedAccount();
    if (device.empty()) return {};
    HttpResult r = Http("POST", AccountBase() + "/api/session", "{}", device);
    if (r.status != 200) {
        LOGW("relay: account sign-in for %s not accepted (HTTP %d), playing as a guest", name.c_str(), r.status);
        return {};
    }
    return json::Parse(r.body)["token"].str();
}

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
        std::string room_ws = m["roomWs"].str();
        if (!room_ws.empty() && room_ws[0] == '/') StartRoom(Origin(app::Get().online.relay) + room_ws);
        else StopRoom();
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

// a WebSocket to url (ws:// or wss://), null on failure; session and connection are the caller's to close
HINTERNET Connect(const std::string& url, HINTERNET& session, HINTERNET& connection) {
    HINTERNET ws = nullptr;
    std::wstring w(url.begin(), url.end());
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = {}, path[1024] = {};
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 1024;
    std::wstring http = w;
    if (http.rfind(L"wss://", 0) == 0) http = L"https://" + http.substr(6);
    else if (http.rfind(L"ws://", 0) == 0) http = L"http://" + http.substr(5);
    u.dwExtraInfoLength = 1024;
    wchar_t extra[1024] = {};
    u.lpszExtraInfo = extra;
    if (!WinHttpCrackUrl(http.c_str(), 0, 0, &u)) return nullptr;
    bool secure = u.nScheme == INTERNET_SCHEME_HTTPS;
    session = WinHttpOpen(L"MC3-PC/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return nullptr;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 0);  // receive waits forever: the server pushes when it has news
    connection = WinHttpConnect(session, host, u.nPort, 0);
    if (!connection) return nullptr;
    std::wstring target = std::wstring(path[0] ? path : L"/ws") + extra;  // the room address carries ?t=ticket
    HINTERNET req = WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return nullptr;
    bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD code = 0, size = sizeof code;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
        if (code == 101) ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    }
    WinHttpCloseHandle(req);
    return ws;
}

// the scheme and host of the relay address: where roomWs paths go
std::string Origin(const std::string& url) {
    size_t a = url.find("://");
    size_t b = url.find('/', a == std::string::npos ? 0 : a + 3);
    return b == std::string::npos ? url : url.substr(0, b);
}

bool SendRoom(const std::string& text) {
    std::lock_guard lock(g_room_lock);
    if (!g_room_ws) return false;
    return WinHttpWebSocketSend(g_room_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
                                DWORD(text.size())) == NO_ERROR;
}

void RoomThread(std::string url) {
    HINTERNET session = nullptr, connection = nullptr;
    HINTERNET ws = Connect(url, session, connection);
    if (ws) {
        {
            std::lock_guard lock(g_room_lock);
            g_room_ws = ws;
        }
        g_room_ready = true;
        LOGR("relay: room socket open (%s)", url.substr(0, url.find('?')).c_str());
        std::string message;
        std::vector<char> buf(16 * 1024);
        while (g_run) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &got, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            message.append(buf.data(), got);
            if (message.size() > kMaxMessage) break;
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                continue;
            json::Value m = json::Parse(message);
            if (m["type"].str() == "input") Deliver(m);
            message.clear();
        }
        g_room_ready = false;
        std::lock_guard lock(g_room_lock);
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);
        g_room_ws = nullptr;
    } else {
        LOGW("relay: room socket refused, datagrams stay on the main socket");
    }
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

void StopRoom() {
    g_room_ready = false;
    {
        std::lock_guard lock(g_room_lock);
        if (g_room_ws) WinHttpWebSocketShutdown(g_room_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    if (g_room_thread.joinable()) g_room_thread.join();
}

void StartRoom(const std::string& url) {
    StopRoom();
    g_room_thread = std::thread(RoomThread, url);
}

void Session(const std::string& url) {
    HINTERNET session = nullptr, connection = nullptr;
    SetStatus(Status::Connecting, "Connecting to " + HostOf(url));
    HINTERNET ws = Connect(url, session, connection);
    {
        std::lock_guard lock(g_send_lock);
        g_ws = ws;
    }
    if (ws) {
        LOGR("relay: connected to %s", url.c_str());
        SetStatus(Status::Connecting, "Signing in to " + HostOf(url));
        std::string token = AccessToken();
        {
            std::lock_guard lock(g_lock);
            g_status.account = token.empty() ? std::string() : SavedAccount().first;
        }
        if (!token.empty()) Send(R"({"type":"auth","clientType":"game","token":)" + json::Quote(token) + "}");
        else Send(R"({"type":"join","clientType":"game","name":)" + json::Quote(PlayerName()) + "}");
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
    StopRoom();
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
        if (!(g_room_ready && SendRoom(env))) Send(env);
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
    StopRoom();
    if (g_ws_thread.joinable()) g_ws_thread.detach();  // a blocked receive ends with the shutdown above
    closesocket(g_udp);
    g_udp = INVALID_SOCKET;
}

void SignIn(const std::string& username, const std::string& password, bool create) {
    {
        std::lock_guard lock(g_lock);
        if (g_status.account_busy) return;
        g_status.account_busy = true;
        g_status.account_message = create ? "Creating the account..." : "Signing in...";
    }
    std::thread([username, password, create] {
        std::string body = R"({"username":)" + json::Quote(username) + R"(,"password":)" + json::Quote(password) + R"(,"remember":true})";
        HttpResult r = Http("POST", AccountBase() + (create ? "/api/register" : "/api/login"), body, {});
        std::string device;
        for (const std::string& c : r.cookies) {
            size_t at = c.find("mc3_device=");
            if (at == std::string::npos) continue;
            std::string v = c.substr(at + 11, 43);
            if (ValidDevice(v)) device = v;
        }
        json::Value answer = json::Parse(r.body);
        std::string message;
        if ((r.status == 200 || r.status == 201) && !device.empty()) {
            std::string name = answer["user"]["username"].str(username);
            json::Value all = LoadAccounts();
            json::Value e{json::Value::Object};
            json::Value n{json::Value::String}, d{json::Value::String};
            n.s = name, d.s = device;
            e.o["username"] = n, e.o["device"] = d;
            all.o[AccountBase()] = e;
            SaveAccounts(all);
            message = "Signed in as " + name;
            LOGR("relay: signed in as %s", name.c_str());
        } else {
            message = answer["error"].str(r.status ? "Sign-in failed (HTTP " + std::to_string(r.status) + ")" : "The account server is unreachable");
        }
        {
            std::lock_guard lock(g_lock);
            g_status.account_busy = false;
            g_status.account_message = message;
        }
        if (!device.empty()) Reconnect();  // sign in to the lobby server as the account
    }).detach();
}

void SignOut() {
    std::thread([] {
        auto [name, device] = SavedAccount();
        json::Value all = LoadAccounts();
        all.o.erase(AccountBase());
        SaveAccounts(all);
        if (!device.empty()) Http("POST", AccountBase() + "/api/logout", "{}", device);  // revoke this PC's device
        {
            std::lock_guard lock(g_lock);
            g_status.account.clear();
            g_status.account_message = name.empty() ? "" : "Signed out";
        }
        Reconnect();
    }).detach();
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
void SignIn(const std::string&, const std::string&, bool) {}
void SignOut() {}
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
