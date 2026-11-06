#pragma once
// The built-in link to the online lobbies (the lobby server at Settings::online.relay, wss://mc3.top/ws by default).
// It is the in-game form of tools/multiplayer/proxy/proxy.py: it listens where xnet.cpp sends its relay datagrams
// (127.0.0.1, kProxyPort + instance), carries them over the server's WebSocket to the other players in the same room,
// and hands their datagrams back to the game, each remote player at the virtual address 127.0.1.<peer>.
// Nothing to install: the game's Internet Play entry opens the lobby list (app/lobby_menu.cpp) and starts it.
#include <cstdint>
#include <string>
#include <vector>

namespace relay {

struct Room {
    std::string id, name, host;
    uint32_t players = 0, max_players = 0;  // 0 max: no cap
    bool main = false, racing = false;
};

struct Status {
    enum State { Off, Connecting, Online, Failed } state = Off;
    std::string message;    // what the player sees: "Connecting to mc3.top", "Server unreachable: ..."
    std::string name;       // the name the server gave us
    std::string room_id, room_name;
    bool in_main = false;
    uint32_t room_players = 0;
    uint32_t peers = 0;     // players we have exchanged game traffic with
    std::string account;          // the mc3.top account signed in on this PC (empty: playing as a guest)
    std::string account_message;  // the last sign-in result: an error, or "Signed in"
    bool account_busy = false;    // a sign-in / sign-out is in progress
};

void Start();  // connects (and keeps reconnecting) with the saved settings; harmless when already running
void Stop();
void Reconnect();  // sign in again now, e.g. with a new name; keeps the wanted room
bool Running();
Status Current();
std::vector<Room> Rooms();  // listed rooms, the Main Lobby first; refreshed by Refresh() and on room changes
void Refresh();
void EnterMain();
void EnterPrivate(const std::string& name);  // unlisted room shared by name (friends)
void CreateRoom(const std::string& name);    // listed room hosted by this player
void JoinRoom(const std::string& id);

// mc3.top accounts (the same store as tools/multiplayer/proxy/account.py: %LOCALAPPDATA%\MidnightClub3-Port\
// multiplayer\accounts.json, one entry per server). The password goes to the server once and is never kept: what is
// kept is the 30-day device credential, traded for a 30-minute access token each time the game connects. Signed in,
// the game joins as the account (badge, race points, rankings) instead of a guest.
void SignIn(const std::string& username, const std::string& password, bool create);  // runs in the background
void SignOut();

}  // namespace relay
