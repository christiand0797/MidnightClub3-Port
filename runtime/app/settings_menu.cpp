// In-game native settings, opened with F1 or Back + Y. Picture/quality changes are live; display changes are staged
// and require confirmation before being saved, with an automatic timeout and resource-allocation rollback.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_vehicle_requested{false};
std::atomic<uint64_t> g_garage_hint_until{0};
bool g_dirty = false;

const char* kModeNames[] = {"Fullscreen", "Borderless window", "Window"};

struct DisplaySelection {
    DisplayMode mode;
    uint32_t width, height, percent, scene_width, scene_height;
    bool force_1080p, force_widescreen;
    bool operator==(const DisplaySelection&) const = default;
};
DisplaySelection Selection(const Settings& s) {
    return {s.display_mode, s.width, s.height, s.render_percent, s.graphics.scene_width, s.graphics.scene_height,
            s.force_1080p, s.force_widescreen};
}
void Select(Settings& s, const DisplaySelection& v) {
    s.display_mode = v.mode;
    s.width = v.width; s.height = v.height; s.render_percent = v.percent;
    s.graphics.scene_width = v.scene_width; s.graphics.scene_height = v.scene_height;
    s.force_1080p = v.force_1080p; s.force_widescreen = v.force_widescreen;
}
DisplaySelection g_draft{}, g_previous{};
bool g_draft_valid = false, g_confirm_display = false, g_display_requested = false;
using DisplayClock = std::chrono::steady_clock;
DisplayClock::time_point g_display_deadline;
std::string g_display_error;

void RevertDisplay() {
    if (!g_confirm_display) return;
    Select(Get(), g_previous);
    g_draft = g_previous;
    g_confirm_display = false;
    ApplyDisplay();
    LOGR("settings: display selection reverted");
}

void DisplayConfirmation() {
    if (g_confirm_display && DisplayClock::now() >= g_display_deadline) RevertDisplay();
    if (g_confirm_display) ImGui::OpenPopup("Keep display settings?");
    ImGui::SetNextWindowSize({std::min(560.0f * ImGui::GetIO().FontGlobalScale,
                                     std::max(240.0f, ImGui::GetIO().DisplaySize.x - 32)), 0});
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x * .5f, ImGui::GetIO().DisplaySize.y * .5f},
                           ImGuiCond_Appearing, {.5f, .5f});
    if (ImGui::BeginPopupModal("Keep display settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!g_confirm_display) {
            ImGui::CloseCurrentPopup();
        } else {
            int left = std::max(0, int(std::chrono::duration<double>(g_display_deadline - DisplayClock::now()).count()));
            ImGui::TextWrapped("Keep this display and scene resolution? Reverting in %d seconds.", left);
            bool ready = ActualRenderResolution() == RenderResolution();
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button("Keep settings")) {
                g_confirm_display = false;
                g_dirty = true;
                Save();
                g_dirty = false;
                LOGR("settings: display selection confirmed and saved");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
            if (!ready) ImGui::TextDisabled("Waiting for the new scene resolution...");
            ImGui::SameLine();
            if (ImGui::Button("Revert") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                RevertDisplay();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// a slider whose change marks the settings for saving
bool Slider(const char* label, float* v, float lo, float hi, const char* fmt, const char* help) {
    bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    g_dirty |= changed;
    return changed;
}

bool Check(const char* label, bool* v, const char* help) {
    bool changed = ImGui::Checkbox(label, v);
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    g_dirty |= changed;
    return changed;
}

void Display(Settings& s) {
    if (!g_draft_valid) { g_draft = Selection(s); g_draft_valid = true; }
    auto& d = g_draft;
    ImGui::TextDisabled("Display changes apply together after choosing Apply.");
    ImGui::BeginDisabled(g_confirm_display);
    int mode = int(d.mode);
    if (ImGui::Combo("Display mode", &mode, kModeNames, 3)) d.mode = DisplayMode(mode);
    // Monitor modes control physical output only. Large scene resolutions remain available on every monitor.
    static std::vector<Mode> modes = [] {
        std::vector<Mode> out;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
            if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
            Mode m{dm.dmPelsWidth, dm.dmPelsHeight};
            if (std::none_of(out.begin(), out.end(), [&](const Mode& o) { return o.width == m.width && o.height == m.height; }))
                out.push_back(m);
        }
        std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) {
            return a.width != b.width ? a.width > b.width : a.height > b.height;
        });
        return out;
    }();
    char current[48];
    if (d.width && d.height) std::snprintf(current, sizeof(current), "%u x %u", d.width, d.height);
    else std::snprintf(current, sizeof(current), "Desktop (%u x %u)", Desktop().width, Desktop().height);
    if (ImGui::BeginCombo("Monitor / window size", current)) {
        if (ImGui::Selectable("Desktop", !d.width)) d.width = d.height = 0;
        for (const Mode& m : modes) {
            char label[32];
            std::snprintf(label, sizeof(label), "%u x %u", m.width, m.height);
            if (ImGui::Selectable(label, d.width == m.width && d.height == m.height)) d.width = m.width, d.height = m.height;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fullscreen requests a supported monitor mode. Borderless covers the desktop.\n"
                          "A window always fits the available desktop. Scene resolution is independent below.");
    ImGui::Separator();
    struct Preset { const char* name; uint32_t w, h; };
    static constexpr Preset presets[] = {
        {"Automatic (follow output)", 0, 0}, {"480p 4:3 (640 x 480)", 640, 480},
        {"480p widescreen (854 x 480)", 854, 480}, {"720p (1280 x 720)", 1280, 720},
        {"1080p (1920 x 1080)", 1920, 1080}, {"1440p (2560 x 1440)", 2560, 1440},
        {"4K (3840 x 2160)", 3840, 2160}, {"5K (5120 x 2880)", 5120, 2880},
        {"8K (7680 x 4320)", 7680, 4320}, {"16K (15360 x 8640)", 15360, 8640},
    };
    const char* selected = "Custom";
    for (const auto& p : presets) if (d.scene_width == p.w && d.scene_height == p.h) selected = p.name;
    if (ImGui::BeginCombo("Native scene resolution", selected)) {
        for (const auto& p : presets)
            if (ImGui::Selectable(p.name, d.scene_width == p.w && d.scene_height == p.h)) {
                d.scene_width = p.w; d.scene_height = p.h;
                // A deliberately selected scene resolution replaces the legacy forced-output override.
                d.force_1080p = false;
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Actual 3D scene dimensions, including widescreen pixels. The settings UI stays at the\n"
                          "display size. 8K / 16K need substantial GPU memory; failures restore your previous settings.");
    int width = int(d.scene_width), height = int(d.scene_height);
    if (ImGui::InputInt("Custom scene width", &width, 16, 128)) d.scene_width = uint32_t(std::clamp(width, 0, 16384));
    if (ImGui::InputInt("Custom scene height", &height, 16, 128)) d.scene_height = uint32_t(std::clamp(height, 0, 16384));
    bool valid = (!d.scene_width && !d.scene_height) || resolution::ValidScene({d.scene_width, d.scene_height});
    if (!valid) ImGui::TextWrapped("Enter both dimensions (320-16384 wide, 240-16384 high), or set both to 0 for Automatic.");
    int scale = int(d.percent);
    if (ImGui::SliderInt("Scene render scale", &scale, 25, 400, "%d%%", ImGuiSliderFlags_AlwaysClamp)) d.percent = uint32_t(scale);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Percent of the output resolution the game renders at.\n"
                          "Above 100%% is supersampling: 200%% renders 4x the pixels and downscales,\n"
                          "the cleanest anti-aliasing there is, at a large GPU cost.");
    ImGui::SameLine();
    if (ImGui::SmallButton("100")) d.percent = 100;
    ImGui::SameLine();
    if (ImGui::SmallButton("150")) d.percent = 150;
    ImGui::SameLine();
    if (ImGui::SmallButton("200")) d.percent = 200;
    ImGui::Checkbox("Force widescreen", &d.force_widescreen);
    Mode output = d.force_1080p ? Mode{1920, 1080} : d.width && d.height ? Mode{d.width, d.height} : Desktop();
    Mode planned = resolution::Scene({d.scene_width, d.scene_height}, output, d.percent);
    Mode actual = ActualRenderResolution();
    ImGui::Text("Requested scene: %u x %u", planned.width, planned.height);
    if (actual.width) ImGui::Text("Current scene: %u x %u", actual.width, actual.height);
    ImGui::TextDisabled("One colour + depth pair: %.0f MiB; effects and textures need more.",
                        double(planned.width) * planned.height * 8 / 1048576.0);
    ImGui::BeginDisabled(!valid || d == Selection(s));
    if (ImGui::Button("Apply display changes")) {
        g_previous = Selection(s);
        Select(s, d);
        g_confirm_display = true;
        g_display_deadline = DisplayClock::now() + std::chrono::seconds(20);
        g_display_error.clear();
        ApplyDisplay();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard pending changes")) d = Selection(s);
    ImGui::EndDisabled();
    if (!g_display_error.empty()) ImGui::TextWrapped("%s", g_display_error.c_str());
    ImGui::Separator();
    Check("V-sync", &s.vsync, "Waits for the display's refresh: no tearing.");
    const char* rates[] = {"60 fps (smooth, correct game speed)", "30 fps (as the original Xbox)"};
    int rate = s.graphics.sixty ? 0 : 1;
    if (ImGui::Combo("Frame rate", &rate, rates, 2)) {
        s.graphics.sixty = rate == 0;
        s.fps_cap = s.graphics.sixty ? 60 : 30;
        g_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The console locked races to 30 fps. At 60 the game's clock is set to 1/60 s steps,\n"
                          "so cars, animation and race times keep their real speed.");
}

void Quality(Settings& s) {
    ImGui::TextDisabled("Apply quality preset");
    if (ImGui::Button("Performance")) {
        s.aniso = 4;
        s.graphics.aa = 1; s.graphics.aa_strength = 0.75f; s.graphics.sharpen = 0.2f;
        s.graphics.texture_detail = 1; s.graphics.lod = 0.85f;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Balanced")) {
        s.aniso = 16;
        s.graphics.aa = 1; s.graphics.aa_strength = 1; s.graphics.sharpen = 0.35f;
        s.graphics.texture_detail = 1; s.graphics.lod = 1;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("High detail")) {
        s.aniso = 16;
        s.graphics.aa = 1; s.graphics.aa_strength = 1; s.graphics.sharpen = 0.25f;
        s.graphics.texture_detail = 1; s.graphics.lod = 1.5f;
        g_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("x1.5 draw detail, 16x anisotropic filtering and full texture detail.\n"
                          "Presets keep your scene resolution, picture, effects, HD pack and camera settings.");
    ImGui::Separator();
    const char* aniso[] = {"As the game asks", "2x", "4x", "8x", "16x"};
    int a = s.aniso >= 16 ? 4 : s.aniso >= 8 ? 3 : s.aniso >= 4 ? 2 : s.aniso >= 2 ? 1 : 0;
    if (ImGui::Combo("Anisotropic filtering", &a, aniso, 5)) {
        s.aniso = a ? 1u << a : 0;
        g_dirty = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sharper road and wall textures at a distance and at angles.");
    const char* detail[] = {"As the console", "Full", "Max (sharpest mip only)"};
    int dl = int(std::min<uint32_t>(s.graphics.texture_detail, 2));
    if (ImGui::Combo("Texture detail", &dl, detail, 3)) {
        s.graphics.texture_detail = uint32_t(dl);
        g_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Full: textures always reach their sharpest level, with 16x anisotropic filtering.\n"
                          "Max: only the sharpest level at any distance; distant textures may shimmer.");
    const char* aa[] = {"Off", "FXAA"};
    int m = int(std::min<uint32_t>(s.graphics.aa, 1));
    if (ImGui::Combo("Anti-aliasing", &m, aa, 2)) {
        s.graphics.aa = uint32_t(m);
        g_dirty = true;
    }
    if (s.graphics.aa) Slider("Anti-aliasing strength", &s.graphics.aa_strength, 0, 1, "%.2f", nullptr);
    Slider("Sharpening (CAS)", &s.graphics.sharpen, 0, 1, "%.2f",
           "AMD FidelityFX contrast adaptive sharpening: restores texture detail, strongest on soft areas.");
    ImGui::Separator();
    Check("HD texture pack", &s.graphics.texture_pack,
          "Replacement textures from textures\\replace\\<hash>.png (AI upscales, remakes), at any resolution.");
    Check("HD movies", &s.graphics.hd_movies,
          "Show matching video_hd MP4 files while keeping the original movie audio and timing.");
    Check("Dump textures", &s.graphics.texture_dump,
          "Saves every texture the game shows to textures\\dump\\<hash>.png: upscale them, put them in replace.");
    ImGui::Separator();
    ImGui::TextDisabled("Game effects");
    Slider("Motion blur", &s.graphics.motion_blur, 0, 2, "%.2f",
           "The game's speed blur: 0 off, 1 as the console, 2 doubled.");
    Slider("Light glow", &s.graphics.glow, 0, 2, "%.2f", "The glow around street lights and neon: 0 off, 1 as the console.");
    Slider("Headlight pools", &s.graphics.headlights, 0, 2, "%.2f",
           "The light the headlights throw on the road: 0 off, 1 as the console.");
    Slider("Neon brightness", &s.graphics.neon, 0, 4, "%.2f",
           "Car neon underglow: 0 off, 1 as the console, 2 (default) twice as bright.");
    Slider("Level of detail", &s.graphics.lod, 0.5f, 4, "x%.2f",
           "How far cars, people and props keep their detail and stay drawn. Above 1 also pushes the far clip out "
           "(up to x2) from the next race or free roam.");
    Slider("Field of view", &s.graphics.fov_scale, 0.75f, 1.5f, "x%.2f",
           "The driving camera's field of view; the mirror and menus keep theirs.");
}

void Picture(Settings& s) {
    Settings::Graphics& g = s.graphics;
    Check("Post-processing", &g.post, "Off shows the game's picture exactly as the console made it.");
    ImGui::BeginDisabled(!g.post);
    Slider("Brightness", &g.brightness, -0.5f, 0.5f, "%+.2f", nullptr);
    Slider("Contrast", &g.contrast, 0.5f, 1.5f, "%.2f", nullptr);
    Slider("Saturation", &g.saturation, 0, 2, "%.2f", nullptr);
    Slider("Vibrance", &g.vibrance, -1, 1, "%+.2f", "Lifts dull colours more than vivid ones.");
    Slider("Gamma", &g.gamma, 0.5f, 2, "%.2f", "Above 1 darkens the mid tones, below 1 lightens them.");
    if (ImGui::Button("Reset picture")) {
        g.brightness = 0, g.contrast = 1, g.saturation = 1, g.vibrance = 0, g.gamma = 1;
        g_dirty = true;
    }
    ImGui::EndDisabled();
}

void Input(Settings& s) {
    if (Slider("Inner stick deadzone", &s.deadzone_inner, 0, 0.9f, "%.2f",
               "Ignores small stick movements near the centre, including controller drift."))
        s.deadzone_outer = std::max(s.deadzone_outer, s.deadzone_inner + 0.05f);
    Slider("Full stick threshold", &s.deadzone_outer, s.deadzone_inner + 0.05f, 1, "%.2f",
           "Stick travel that gives full steering or camera input. The range in between is rescaled smoothly.");
    if (ImGui::Button("Reset controller response")) {
        const Settings defaults;
        s.deadzone_inner = defaults.deadzone_inner; s.deadzone_outer = defaults.deadzone_outer;
        g_dirty = true;
    }
    ImGui::Separator();
    ImGui::TextWrapped("Keyboard: Enter Start, Space A, Escape B, Backspace Back. Arrows navigate; "
                       "W/S accelerate/brake, A/D steer. Q/E are black/white; X/Y are the face buttons.");
}

// the controller drives ImGui's navigation while the menu is open
void FeedController() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    input::Pad p = input::RawPad(0);
    auto key = [&](ImGuiKey k, bool down) { io.AddKeyEvent(k, down); };
    key(ImGuiKey_GamepadDpadUp, (p.buttons & 1) || p.ly > 16000);
    key(ImGuiKey_GamepadDpadDown, (p.buttons & 2) || p.ly < -16000);
    key(ImGuiKey_GamepadDpadLeft, (p.buttons & 4) || p.lx < -16000);
    key(ImGuiKey_GamepadDpadRight, (p.buttons & 8) || p.lx > 16000);
    key(ImGuiKey_GamepadFaceDown, p.analog[0] > 128);   // A: activate
    key(ImGuiKey_GamepadFaceRight, p.analog[1] > 128);  // B: back
    key(ImGuiKey_GamepadL1, p.analog[5] > 128);         // white / LB: previous tab
    key(ImGuiKey_GamepadR1, p.analog[4] > 128);         // black / RB: next tab
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleMenu() {
    std::lock_guard lock(UiLock());
    bool was = g_open.exchange(!g_open.load());
    if (was) RevertDisplay();
    else g_draft_valid = false;
    if (was && g_dirty) {
        Save();
        g_dirty = false;
        LOGR("settings: saved from the menu");
    }
}

void OpenVehicleMenu() {
    std::lock_guard lock(UiLock());
    g_vehicle_requested = true;
    g_open = true;
}

void GarageTuningActive() { g_garage_hint_until = GetTickCount64() + 750; }

void ReportGraphicsFailure(const char* reason) {
    std::lock_guard lock(UiLock());
    g_display_error = reason;
    if (g_confirm_display) {
        RevertDisplay();
    } else {
        // A settings file copied from another PC can request a scene that does not fit this GPU.
        auto& s = Get();
        Mode old = RenderResolution();
        s.graphics.scene_width = old.width > 1280 || old.height > 720 ? 1280 : 640;
        s.graphics.scene_height = s.graphics.scene_width == 1280 ? 720 : 480;
        s.render_percent = 100;
        s.force_1080p = false;
        g_dirty = true;
    }
    g_draft = Selection(Get());
    g_draft_valid = true;
    g_display_requested = true;
    g_open = true;
    LOGR("settings: graphics recovery: %s", reason);
}

void DrawSettingsMenu(float fps) {
    Settings& s = Get();
    ImGuiIO& io = ImGui::GetIO();
    if (g_confirm_display && DisplayClock::now() >= g_display_deadline) RevertDisplay();
    if (s.graphics.show_fps) {
        ImGui::SetNextWindowPos({12, 12});
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("%.0f fps", fps);
        ImGui::End();
    }
    io.MouseDrawCursor = MenuOpen();
    if (!MenuOpen()) {
        if (GetTickCount64() < g_garage_hint_until.load()) {
            ImGui::SetNextWindowPos({io.DisplaySize.x - 16, 16}, ImGuiCond_Always, {1, 0});
            ImGui::SetNextWindowBgAlpha(0.8f);
            ImGui::Begin("##garage_pc_tuning", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav);
            if (ImGui::Button("Vehicle tuning (F2)")) OpenVehicleMenu();
            ImGui::TextDisabled("Camber, offset, suspension, power");
            ImGui::TextDisabled("Controller: Back + Y, Vehicle tab");
            ImGui::End();
        }
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        return;
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    FeedController();

    // sized for the screen: the console's UI is drawn for 480 lines, this follows the output height
    float ui = std::clamp(io.DisplaySize.y / 900.0f, 1.0f, 2.0f);
    io.FontGlobalScale = ui;
    float width = std::min(std::max(640.0f * ui, io.DisplaySize.x * 0.65f), std::max(240.0f, io.DisplaySize.x - 24.0f));
    float height = std::max(160.0f, io.DisplaySize.y - 32.0f);
    ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    ImGui::Begin("Game settings", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::BeginChild("settings contents", {0, -ImGui::GetFrameHeightWithSpacing() - 8.0f}, false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PushItemWidth(-std::min(230.0f * ui, width * 0.38f));
    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Display", nullptr, g_display_requested ? ImGuiTabItemFlags_SetSelected : 0)) {
            g_display_requested = false;
            Display(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vehicle", nullptr,
                                g_vehicle_requested.exchange(false) ? ImGuiTabItemFlags_SetSelected : 0)) {
            ImGui::TextWrapped("Live player-car tuning. Available in the garage with F2; applies to both garage "
                               "previews and driving. These additional settings are saved with the PC settings.");
            Check("Extended garage choices", &s.tuning.garage_extended,
                  "More wheel, tire and ride-height steps in the game's garage menus. Reopen the garage menu\n"
                  "after changing this. Previously saved extended values remain readable when disabled.");
            ImGui::Separator();
            ImGui::TextDisabled("Stance / front and rear");
            Slider("Ride height front", &s.tuning.ride_front, -0.5f, 0.5f, "%+.3f m",
                   "Added to your car's front suspension, on top of its garage setting: - lowers, + raises.");
            Slider("Ride height rear", &s.tuning.ride_rear, -0.5f, 0.5f, "%+.3f m", nullptr);
            Check("Allow wheels to tuck into the body", &s.tuning.wheel_tuck,
                  "Keeps grounded tires at their contact position when lowered past the original range. "
                  "The fenders may intersect the tires.");
            Slider("Camber front", &s.tuning.camber_front, -30, 20, "%+.1f deg",
                   "Additional visual camber relative to the car's saved setup. Negative angles tuck the top in.");
            Slider("Camber rear", &s.tuning.camber_rear, -30, 20, "%+.1f deg", nullptr);
            Slider("Wheel offset front", &s.tuning.offset_front, -0.3f, 0.3f, "%+.3f m",
                   "Additional offset on each side. Positive moves the wheel outward; negative tucks it inward.");
            Slider("Wheel offset rear", &s.tuning.offset_rear, -0.3f, 0.3f, "%+.3f m", nullptr);
            Check("Airbags follow selected ride height", &s.tuning.airbag_match_ride,
                  "For a lowered axle the fully deflated position equals its chosen ride height. "
                  "Fully inflated mirrors that amount above stock. Uses the installed airbag controls.");
            ImGui::Separator();
            ImGui::TextDisabled("Suspension");
            Slider("Front spring stiffness", &s.tuning.spring_front, 0.25f, 3, "x%.2f", nullptr);
            Slider("Rear spring stiffness", &s.tuning.spring_rear, 0.25f, 3, "x%.2f", nullptr);
            Slider("Front damping", &s.tuning.damper_front, 0.25f, 3, "x%.2f", nullptr);
            Slider("Rear damping", &s.tuning.damper_rear, 0.25f, 3, "x%.2f", nullptr);
            ImGui::Separator();
            ImGui::TextDisabled("Brakes, tires and engine");
            Slider("Front brake force", &s.tuning.brake_front, 0.25f, 3, "x%.2f", nullptr);
            Slider("Rear brake force", &s.tuning.brake_rear, 0.25f, 3, "x%.2f", nullptr);
            Slider("Front tire grip", &s.tuning.grip_front, 0.5f, 2, "x%.2f", "Multiplies static and sliding tire friction.");
            Slider("Rear tire grip", &s.tuning.grip_rear, 0.5f, 2, "x%.2f", nullptr);
            Slider("Engine horsepower", &s.tuning.engine_power, 0.5f, 3, "x%.2f",
                   "Multiplies the engine power after the car's purchased upgrades.");
            Slider("Wheel and tire size limit", &s.tuning.wheel_limit, 1, 3, "x%.2f",
                   "How large a wheel and tire each car takes in the garage, times its stock limit. Applies when a "
                   "car loads.");
            if (ImGui::Button("Reset extra vehicle tuning")) {
                const Settings defaults;
                const bool extended = s.tuning.garage_extended;
                s.tuning = defaults.tuning;
                s.tuning.garage_extended = extended;
                g_dirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resets additional PC tuning; keeps purchased garage parts and extended choices.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
            Input(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Quality")) {
            Quality(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Picture")) {
            Picture(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            Check("Show frame rate", &s.graphics.show_fps, nullptr);
            ImGui::Text("%.1f fps", fps);
            ImGui::TextWrapped("F1 or Back + Y opens and closes this menu. Mouse, Tab / Shift+Tab, arrow keys and "
                              "controller D-pad navigate it. Enter / A activates. Display changes require Keep settings; "
                              "other changes apply live and save on close.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::Spacing();
    if (ImGui::Button("Close")) open = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(g_confirm_display);
    if (ImGui::Button("Save now")) { Save(); g_dirty = false; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("F1 / B closes");
    ImGui::End();
    DisplayConfirmation();
    io.FontGlobalScale = 1.0f;
    if (!open || (!g_confirm_display && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) && !ImGui::IsAnyItemActive())) ToggleMenu();
}

}  // namespace app
