// The in-game graphics settings menu: an ImGui window over the game, opened with F1 or Back + Y. Every change applies
// at once (the renderer reads Settings each present) and the file is written when the menu closes.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
bool g_dirty = false;

const char* kModeNames[] = {"Fullscreen", "Borderless window", "Window"};

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
    int mode = int(s.display_mode);
    if (ImGui::Combo("Display mode", &mode, kModeNames, 3)) {
        s.display_mode = DisplayMode(mode);
        g_dirty = true;
        ApplyDisplay();
    }
    // output resolution: the monitor's own modes, or the desktop's
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
    if (s.width && s.height) std::snprintf(current, sizeof(current), "%u x %u", s.width, s.height);
    else std::snprintf(current, sizeof(current), "Desktop (%u x %u)", Desktop().width, Desktop().height);
    if (ImGui::BeginCombo("Resolution", current)) {
        if (ImGui::Selectable("Desktop", !s.width)) {
            s.width = s.height = 0;
            g_dirty = true;
            ApplyDisplay();
        }
        for (const Mode& m : modes) {
            char label[32];
            std::snprintf(label, sizeof(label), "%u x %u", m.width, m.height);
            if (ImGui::Selectable(label, s.width == m.width && s.height == m.height)) {
                s.width = m.width;
                s.height = m.height;
                g_dirty = true;
                ApplyDisplay();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The output resolution. In borderless mode the window always covers the desktop and the\n"
                          "game renders at this size scaled to it; fullscreen switches the monitor's mode.");
    int scale = int(s.render_percent);
    if (ImGui::SliderInt("Render scale", &scale, 50, 400, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        s.render_percent = uint32_t(scale);
        g_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Percent of the output resolution the game renders at.\n"
                          "Above 100%% is supersampling: 200%% renders 4x the pixels and downscales,\n"
                          "the cleanest anti-aliasing there is, at a large GPU cost.");
    ImGui::SameLine();
    if (ImGui::SmallButton("100")) s.render_percent = 100, g_dirty = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("150")) s.render_percent = 150, g_dirty = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("200")) s.render_percent = 200, g_dirty = true;
    Check("V-sync", &s.vsync, "Waits for the display's refresh: no tearing.");
    Check("Force widescreen", &s.force_widescreen, "Report a 16:9 display to the game whatever the output shape.");
    Check("Force 1080p output", &s.force_1080p, "Use 1920x1080 as the output resolution, whatever the window size.");
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
        s.render_percent = 100; s.aniso = 4;
        s.graphics.aa = 1; s.graphics.aa_strength = 0.75f; s.graphics.sharpen = 0.2f;
        s.graphics.texture_detail = 1; s.graphics.lod = 0.85f;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Balanced")) {
        s.render_percent = 100; s.aniso = 16;
        s.graphics.aa = 1; s.graphics.aa_strength = 1; s.graphics.sharpen = 0.35f;
        s.graphics.texture_detail = 1; s.graphics.lod = 1;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("High detail")) {
        s.render_percent = 150; s.aniso = 16;
        s.graphics.aa = 1; s.graphics.aa_strength = 1; s.graphics.sharpen = 0.25f;
        s.graphics.texture_detail = 1; s.graphics.lod = 1.5f;
        g_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("150%% render scale and x1.5 draw detail. Costs more GPU time and memory.\n"
                          "Presets keep your picture, effects, HD pack and camera settings.");
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
    bool was = g_open.exchange(!g_open.load());
    if (was && g_dirty) {
        Save();
        g_dirty = false;
        LOGR("settings: saved from the menu");
    }
}

void DrawSettingsMenu(float fps) {
    Settings& s = Get();
    ImGuiIO& io = ImGui::GetIO();
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
        io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        return;
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    FeedController();

    // sized for the screen: the console's UI is drawn for 480 lines, this follows the output height
    float ui = std::max(1.0f, io.DisplaySize.y / 720.0f);
    io.FontGlobalScale = ui;
    ImGui::SetNextWindowSize({io.DisplaySize.x * 0.42f, io.DisplaySize.y * 0.62f}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    ImGui::Begin("Game settings", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Display")) {
            Display(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vehicle")) {
            Check("Extended garage choices", &s.tuning.garage_extended,
                  "More wheel, tire and ride-height steps in the game's garage menus. Reopen the garage menu\n"
                  "after changing this. Previously saved extended values remain readable when disabled.");
            Slider("Ride height front", &s.tuning.ride_front, -0.5f, 0.5f, "%+.2f m",
                   "Added to your car's front suspension, on top of its garage setting: - lowers, + raises.");
            Slider("Ride height rear", &s.tuning.ride_rear, -0.5f, 0.5f, "%+.2f m", nullptr);
            Slider("Wheel and tire size limit", &s.tuning.wheel_limit, 1, 3, "x%.2f",
                   "How large a wheel and tire each car takes in the garage, times its stock limit. Applies when a "
                   "car loads.");
            if (ImGui::Button("Reset extra vehicle tuning")) {
                const Settings defaults;
                s.tuning.ride_front = s.tuning.ride_rear = 0;
                s.tuning.wheel_limit = defaults.tuning.wheel_limit;
                g_dirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Resets the extra F1 offsets and fit limit; keeps your purchased garage parts.");
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
            ImGui::TextWrapped("F1 or Back + Y opens and closes this menu. Changes apply at once and are saved "
                           "to settings.toml when the menu closes.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    if (ImGui::Button("Close")) open = false;
    ImGui::SameLine();
    ImGui::TextDisabled("B closes");
    ImGui::End();
    io.FontGlobalScale = 1.0f;
    if (!open || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive()) ToggleMenu();
}

}  // namespace app
