#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include "app/app.h"
#include "../templates/game/src/game/economy.h"
#include "../templates/game/src/game/world_tuning.h"

namespace fixture {
mc3::economy::Snapshot displayed;
uint64_t live_career = 1;
std::vector<std::pair<uint32_t, uint64_t>> attempts, accepted;
unsigned money_reads = 0, world_reads = 0;
}
namespace app {
Settings& Get() { static Settings settings; return settings; }
}
namespace mc3::economy {
Snapshot ReadSnapshot() { ++fixture::money_reads; return fixture::displayed; }
bool SetMoney(uint32_t target, uint64_t expected_career_token) {
    fixture::attempts.emplace_back(target, expected_career_token);
    if (!fixture::displayed.ready || fixture::displayed.pending ||
        target > kMaxMoney || expected_career_token != fixture::live_career)
        return false;
    fixture::accepted.emplace_back(target, expected_career_token);
    fixture::displayed.pending = true;
    return true;
}
const char* StatusText(Status) { return "CPU fixture career status"; }
}
namespace mc3::world {
Snapshot GetSnapshot() {
    ++fixture::world_reads;
    return {true, true, 12, app::Get().world.traffic_multiplier, app::Get().world.pedestrian_multiplier};
}
}

// Exercise the actual title controls. No native window/device/game or save file is created.
#include "../templates/game/src/common/gameplay_menu.cpp"

namespace {
enum class Page { Money, Population, Tabs };
bool Frame(Page page, const char* select_tab = nullptr) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {640, 480};
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({8, 8}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({624, 464}, ImGuiCond_Always);
    ImGui::Begin("Gameplay CPU menu", nullptr, ImGuiWindowFlags_NoSavedSettings);
    bool dirty = false;
    if (page == Page::Money) mc3::Money();
    if (page == Page::Population) dirty = mc3::Population();
    if (page == Page::Tabs && ImGui::BeginTabBar("gameplay tabs")) {
        if (select_tab) {
            auto* bar = GImGui->CurrentTabBar;
            for (auto& tab : bar->Tabs)
                if (std::strcmp(ImGui::TabBarGetTabName(bar, &tab), select_tab) == 0)
                    bar->NextSelectedTabId = tab.ID;
        }
        dirty = mc3::DrawGameplaySettingsTabs();
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::Render();
    assert(ImGui::GetDrawData() && ImGui::GetDrawData()->DisplaySize.y == 480);
    return dirty;
}
void Activate(const char* label) {
    auto* window = ImGui::FindWindowByName("Gameplay CPU menu");
    assert(window);
    ImGui::ActivateItemByID(window->GetID(label));
}
void Money(uint32_t amount, uint64_t career) {
    fixture::displayed = {};
    fixture::displayed.ready = true;
    fixture::displayed.money = amount;
    fixture::displayed.status = mc3::economy::Status::Ready;
    fixture::displayed.career_token = career;
    fixture::live_career = career;
}
void Click(Page page, const char* label) {
    Activate(label);
    Frame(page);
}
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Money(30000, 1);

    // Visiting both real F1 tabs at 480p queues no money request and changes no preferences.
    for (int i = 0; i < 3; ++i) assert(!Frame(Page::Tabs));
    const auto world_reads = fixture::world_reads;
    for (int i = 0; i < 3; ++i) assert(!Frame(Page::Tabs, "Career"));
    assert(fixture::world_reads >= world_reads && fixture::money_reads > 0);
    assert(fixture::attempts.empty());
    assert(app::Get().world.traffic_multiplier == 1 && app::Get().world.pedestrian_multiplier == 1);

    Frame(Page::Money);
    Frame(Page::Money);
    Click(Page::Money, "Apply balance");
    assert((fixture::accepted == std::vector<std::pair<uint32_t, uint64_t>>{{30000, 1}}));
    for (int i = 0; i < 4; ++i) Frame(Page::Money);
    assert(fixture::accepted.size() == 1);  // No repeat or permanent balance override.

    // Ordinary balance updates do not erase an in-progress draft. The explicit refresh does.
    Money(40000, 1);
    Frame(Page::Money);
    Click(Page::Money, "Use current balance");
    Click(Page::Money, "Apply balance");
    assert(fixture::accepted.back() == std::pair(uint32_t(40000), uint64_t(1)));

    // A ready-to-ready career switch, even if the UI never saw the unloaded interval, resets the draft.
    Money(11111, 10); Frame(Page::Money);
    Money(88888, 11); Frame(Page::Money);
    Click(Page::Money, "Apply balance");
    assert(fixture::accepted.back() == std::pair(uint32_t(88888), uint64_t(11)));

    // The displayed token reaches the backend unchanged; a later actual career cannot receive this request.
    Money(12345, 20); Frame(Page::Money);
    fixture::live_career = 21;
    const auto accepted = fixture::accepted.size();
    Click(Page::Money, "Apply balance");
    assert(fixture::attempts.back() == std::pair(uint32_t(12345), uint64_t(20)));
    assert(fixture::accepted.size() == accepted);

    // Disabled contexts cannot be activated through controller/keyboard navigation either.
    Money(55555, 30); fixture::displayed.ready = false; Frame(Page::Money);
    const auto attempts = fixture::attempts.size();
    Click(Page::Money, "Apply balance"); Click(Page::Money, "Add $10,000");
    assert(fixture::attempts.size() == attempts);
    Money(55555, 30); fixture::displayed.pending = true; Frame(Page::Money);
    Click(Page::Money, "Apply balance"); Click(Page::Money, "Add $100,000");
    assert(fixture::attempts.size() == attempts);

    Money(mc3::economy::kMaxMoney - 5000, 40); Frame(Page::Money);
    Click(Page::Money, "Add $10,000");
    assert(fixture::accepted.back() == std::pair(mc3::economy::kMaxMoney, uint64_t(40)));

    // Population presets set supported preferences and return one dirty event, without touching cash.
    const auto money_attempts = fixture::attempts.size();
    Frame(Page::Population); Frame(Page::Population);
    Activate("Busy streets"); assert(Frame(Page::Population));
    assert(app::Get().world.traffic_multiplier == 2 && app::Get().world.pedestrian_multiplier == 2);
    assert(!Frame(Page::Population));
    Activate("Empty streets"); assert(Frame(Page::Population));
    assert(app::Get().world.traffic_multiplier == 0 && app::Get().world.pedestrian_multiplier == 0);
    Activate("Original density"); assert(Frame(Page::Population));
    assert(app::Get().world.traffic_multiplier == 1 && app::Get().world.pedestrian_multiplier == 1);
    assert(fixture::attempts.size() == money_attempts);
    ImGui::DestroyContext();
    std::cout << "gameplay_menu: 480p tabs, explicit one-shot actions, career tokens, disabled controls and population dirty events passed\n";
}
