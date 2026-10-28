#include <algorithm>
#include <cstdint>

#include <imgui.h>

#include "app/app.h"
#include "../game/economy.h"
#include "../game/world_tuning.h"

namespace mc3 {
namespace {
bool Population() {
    auto& options = app::Get().world;
    const auto state = world::GetSnapshot();
    ImGui::TextWrapped("Free-roam traffic and pedestrians. 1x keeps the game's selected density; 0x requests "
                       "none. The original spawn pools, authored limits and scripted events still apply, so a "
                       "4x target does not guarantee four times as many visible actors.");
    bool changed = ImGui::SliderFloat("Traffic density", &options.traffic_multiplier, 0,
                                      world::kMaxMultiplier, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Pedestrian density", &options.pedestrian_multiplier, 0,
                                  world::kMaxMultiplier, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
    auto preset = [&](const char* label, float traffic, float pedestrians) {
        if (ImGui::Button(label)) {
            options.traffic_multiplier = traffic;
            options.pedestrian_multiplier = pedestrians;
            changed = true;
        }
    };
    preset("Empty streets", 0, 0);
    ImGui::SameLine();
    preset("Original density", 1, 1);
    ImGui::SameLine();
    preset("Busy streets", 2, 2);
    if (state.free_roam_player) {
        ImGui::Text("Active targets: traffic x%.2f, pedestrians x%.2f",
                    state.traffic_multiplier, state.pedestrian_multiplier);
        ImGui::Text("Traffic roads tracked: %u", state.traffic_roads_ready);
        ImGui::TextUnformatted(state.pedestrians_ready ? "Pedestrian manager ready" : "Waiting for pedestrian manager");
    } else {
        ImGui::TextWrapped("Targets apply when a player car is active in free roam. Races, missions and the "
                           "garage keep their original population rules.");
    }
    return changed;
}

void Money() {
    const auto snapshot = economy::ReadSnapshot();
    static int draft = 0;
    static bool initialized = false;
    static uint64_t draft_career = 0;
    if (!snapshot.ready) initialized = false;
    if (snapshot.ready && (!initialized || draft_career != snapshot.career_token)) {
        draft = int(std::min(snapshot.money, economy::kMaxMoney));
        draft_career = snapshot.career_token;
        initialized = true;
    }
    ImGui::TextWrapped("Edit the loaded career's balance. Changes apply once, then purchases and race rewards "
                       "continue normally. Use the game's normal save flow to keep the change.");
    if (snapshot.ready) ImGui::Text("Current balance: $%u", snapshot.money);
    ImGui::TextWrapped("%s", economy::StatusText(snapshot.status));
    const bool can_apply = snapshot.ready && !snapshot.pending;
    ImGui::BeginDisabled(!can_apply);
    ImGui::InputInt("New balance", &draft, 1000, 10000);
    const bool valid = draft >= 0 && uint32_t(draft) <= economy::kMaxMoney;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Apply balance") && can_apply && valid)
        economy::SetMoney(uint32_t(draft), snapshot.career_token);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Use current balance")) draft = int(std::min(snapshot.money, economy::kMaxMoney));
    auto add = [&](const char* label, uint32_t amount) {
        if (ImGui::Button(label) && can_apply)
            economy::SetMoney(uint32_t(std::min(uint64_t(economy::kMaxMoney), uint64_t(snapshot.money) + amount)),
                              snapshot.career_token);
    };
    add("Add $10,000", 10000);
    ImGui::SameLine();
    add("Add $100,000", 100000);
    ImGui::SameLine();
    add("Add $1,000,000", 1000000);
    ImGui::EndDisabled();
    if (!valid) ImGui::TextWrapped("Enter a balance between $0 and $999,999,999.");
}
}  // namespace

bool DrawGameplaySettingsTabs() {
    bool changed = false;
    if (ImGui::BeginTabItem("World")) {
        changed = Population();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Career")) {
        Money();
        ImGui::EndTabItem();
    }
    return changed;  // Money is career data, never a persisted PC setting or startup override.
}
}  // namespace mc3
