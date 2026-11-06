// Online lobbies: the window the game's Internet Play entry opens (game hook in templates/game/src/game/online.cpp).
// It replaces the Xbox Live sign-in, which needs Microsoft's retired servers: pick the Main Lobby (everyone online),
// a room someone hosts, your own room, or a private room shared by name. The game then continues into its System
// Link Host/Join screens, where everyone in the chosen room shows up in Join Game.
// Drawn by the renderer inside its ImGui frame under UiLock (settings_menu.cpp), like the settings menu.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/app.h"
#include "core/relay_link.h"
#include "imgui.h"

namespace app {

namespace {
char g_name[33], g_room[51];
bool g_fields_loaded = false;

void LoadFields() {
    const Settings& s = Get();
    std::snprintf(g_name, sizeof g_name, "%s", s.online.name.c_str());
    std::snprintf(g_room, sizeof g_room, "%s", s.online.room.c_str());
    g_fields_loaded = true;
}

void SaveName() {
    Settings& s = Get();
    if (s.online.name != g_name) {
        s.online.name = g_name;
        Save();
        relay::Reconnect();  // the server knows players by the name they signed in with
    }
}
}  // namespace

// returns false when the player closes the window
bool DrawLobby() {
    if (!g_fields_loaded) LoadFields();
    ImGuiIO& io = ImGui::GetIO();
    float ui = std::clamp(io.DisplaySize.y / 900.0f, 1.0f, 2.0f);
    io.FontGlobalScale = ui;
    float width = std::min(std::max(620.0f * ui, io.DisplaySize.x * 0.55f), io.DisplaySize.x - 24.0f);
    float height = std::min(560.0f * ui, io.DisplaySize.y - 32.0f);
    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.94f);
    bool open = true;
    ImGui::Begin("Online lobbies", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

    relay::Status st = relay::Current();
    ImVec4 colour = st.state == relay::Status::Online ? ImVec4(0.3f, 1, 0.5f, 1)
                    : st.state == relay::Status::Failed ? ImVec4(1, 0.4f, 0.3f, 1)
                                                        : ImVec4(1, 0.85f, 0.3f, 1);
    ImGui::TextColored(colour, "%s", st.message.empty() ? "Starting" : st.message.c_str());
    if (!st.name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  as %s", st.name.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Your name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", g_name, sizeof g_name, ImGuiInputTextFlags_EnterReturnsTrue)) SaveName();
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveName();
    ImGui::TextDisabled("Press Enter to use it: you reconnect under the new name.");

    ImGui::Separator();
    ImGui::TextUnformatted("Lobbies");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f * ui);
    if (ImGui::Button("Refresh")) relay::Refresh();
    ImGui::BeginChild("rooms", {0, -ImGui::GetFrameHeightWithSpacing() * 4.2f}, true);
    std::vector<relay::Room> rooms = relay::Rooms();
    if (rooms.empty()) ImGui::TextDisabled(st.state == relay::Status::Online ? "No lobbies listed." : "Waiting for the server...");
    for (const relay::Room& r : rooms) {
        bool here = r.id == st.room_id;
        std::string label = r.main ? r.name + " (open to everyone)" : r.name;
        if (here) label += "   < you are here";
        char info[96];
        std::snprintf(info, sizeof info, "%u online%s%s", r.players, r.racing ? " - racing" : "",
                      !r.main && !r.host.empty() ? (" - host " + r.host).substr(0, 60).c_str() : "");
        ImGui::PushID(r.id.c_str());
        if (ImGui::Selectable(label.c_str(), here, 0, {0, 0})) {
            if (r.main) relay::EnterMain();
            else relay::JoinRoom(r.id);
            Get().online.room.clear();
            Save();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.62f + ImGui::GetCursorPosX() * 0);
        ImGui::TextDisabled("%s", info);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::TextUnformatted("Room name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f * ui);
    ImGui::InputText("##room", g_room, sizeof g_room);
    ImGui::SameLine();
    bool named = g_room[0] != 0;
    if (!named) ImGui::BeginDisabled();
    if (ImGui::Button("Host public room")) relay::CreateRoom(g_room);
    ImGui::SameLine();
    if (ImGui::Button("Private room")) {
        relay::EnterPrivate(g_room);
        Get().online.room = g_room;
        Save();
    }
    if (!named) ImGui::EndDisabled();
    ImGui::TextDisabled("Private rooms are not listed: friends who type the same name join you.");

    ImGui::Separator();
    if (ImGui::Button("Race", {160.0f * ui, 0})) open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("Then Host Game or Join Game: everyone in your lobby is listed.");
    ImGui::End();
    return open;
}

}  // namespace app
