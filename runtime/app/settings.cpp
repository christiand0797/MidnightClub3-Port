// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "core/host_path.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

// [graphics]: every field by its key, so load and save stay one list
struct GraphicsField {
    const char* key;
    float* f;
    bool* b;
    uint32_t* u;
    float lo, hi;
};
std::vector<GraphicsField> TuningFields(Settings::Tuning& v) {
    return {
        {"garage_extended", nullptr, &v.garage_extended, nullptr, 0, 1},
        {"ride_height_front", &v.ride_front, nullptr, nullptr, -0.5f, 0.5f},
        {"ride_height_rear", &v.ride_rear, nullptr, nullptr, -0.5f, 0.5f},
        {"wheel_size_limit", &v.wheel_limit, nullptr, nullptr, 1, 3},
        {"camber_front", &v.camber_front, nullptr, nullptr, -30, 20},
        {"camber_rear", &v.camber_rear, nullptr, nullptr, -30, 20},
        {"offset_front", &v.offset_front, nullptr, nullptr, -0.3f, 0.3f},
        {"offset_rear", &v.offset_rear, nullptr, nullptr, -0.3f, 0.3f},
        {"spring_front", &v.spring_front, nullptr, nullptr, 0.25f, 3},
        {"spring_rear", &v.spring_rear, nullptr, nullptr, 0.25f, 3},
        {"damper_front", &v.damper_front, nullptr, nullptr, 0.25f, 3},
        {"damper_rear", &v.damper_rear, nullptr, nullptr, 0.25f, 3},
        {"brake_front", &v.brake_front, nullptr, nullptr, 0.25f, 3},
        {"brake_rear", &v.brake_rear, nullptr, nullptr, 0.25f, 3},
        {"grip_front", &v.grip_front, nullptr, nullptr, 0.5f, 2},
        {"grip_rear", &v.grip_rear, nullptr, nullptr, 0.5f, 2},
        {"engine_power", &v.engine_power, nullptr, nullptr, 0.5f, 3},
        {"airbag_match_ride", nullptr, &v.airbag_match_ride, nullptr, 0, 1},
        {"wheel_tuck", nullptr, &v.wheel_tuck, nullptr, 0, 1},
    };
}

std::vector<GraphicsField> GraphicsFields(Settings::Graphics& g) {
    return {
        {"scene_width", nullptr, nullptr, &g.scene_width, 0, resolution::kMaxDimension},
        {"scene_height", nullptr, nullptr, &g.scene_height, 0, resolution::kMaxDimension},
        {"post", nullptr, &g.post, nullptr, 0, 1},
        {"anti_aliasing", nullptr, nullptr, &g.aa, 0, 1},
        {"anti_aliasing_strength", &g.aa_strength, nullptr, nullptr, 0, 1},
        {"sharpen", &g.sharpen, nullptr, nullptr, 0, 1},
        {"brightness", &g.brightness, nullptr, nullptr, -0.5f, 0.5f},
        {"contrast", &g.contrast, nullptr, nullptr, 0.5f, 1.5f},
        {"saturation", &g.saturation, nullptr, nullptr, 0, 2},
        {"vibrance", &g.vibrance, nullptr, nullptr, -1, 1},
        {"gamma", &g.gamma, nullptr, nullptr, 0.5f, 2},
        {"sixty_fps", nullptr, &g.sixty, nullptr, 0, 1},
        {"motion_blur", &g.motion_blur, nullptr, nullptr, 0, 2},
        {"glow", &g.glow, nullptr, nullptr, 0, 2},
        {"headlights", &g.headlights, nullptr, nullptr, 0, 2},
        {"neon", &g.neon, nullptr, nullptr, 0, 4},
        {"show_fps", nullptr, &g.show_fps, nullptr, 0, 1},
        {"texture_pack", nullptr, &g.texture_pack, nullptr, 0, 1},
        {"texture_dump", nullptr, &g.texture_dump, nullptr, 0, 1},
        {"texture_detail", nullptr, nullptr, &g.texture_detail, 0, 2},
        {"hd_movies", nullptr, &g.hd_movies, nullptr, 0, 1},
        {"lod", &g.lod, nullptr, nullptr, 0.5f, 4},
        {"fov_scale", &g.fov_scale, nullptr, nullptr, 0.75f, 1.5f},
    };
}

// config/mc3_enhancements.toml beside the exe: the settings app.h lists under it. An earlier session's version of the
// file held many more keys that nothing read; those are ignored now and dropped when the file is next written.
std::filesystem::path EnhancementsFile() { return ExeDir() / "config" / "mc3_enhancements.toml"; }

void ClampDeadzones(Settings& s) {
    s.deadzone_inner = std::clamp(s.deadzone_inner, 0.0f, 0.9f);
    s.deadzone_outer = std::clamp(s.deadzone_outer, s.deadzone_inner + 0.05f, 1.0f);
}

// after the settings file: graphics_keys says which [graphics] keys that file set, so an older name here for one of
// them only applies when settings.toml does not have it
void LoadEnhancements(Settings& s, const toml::table* graphics_keys) {
    toml::table t;
    try {
        t = toml::parse_file(EnhancementsFile().wstring());
    } catch (...) {
        return;  // optional
    }
    if (const toml::table* r = t["render"].as_table()) {
        s.force_1080p = (*r)["force_1080p"].value_or(s.force_1080p);
        s.force_widescreen = (*r)["force_widescreen"].value_or(s.force_widescreen);
    }
    // [input]; the first version of the file had the deadzones under [gameplay]
    const toml::table* in = t["input"].as_table();
    if (!in) in = t["gameplay"].as_table();
    if (in) {
        s.deadzone_inner = (*in)["deadzone_inner"].value_or(s.deadzone_inner);
        s.deadzone_outer = (*in)["deadzone_outer"].value_or(s.deadzone_outer);
    }
    ClampDeadzones(s);
    // older names for [graphics] settings, read once: the next save keeps them in settings.toml only
    auto unset = [&](const char* key) { return !graphics_keys || !graphics_keys->contains(key); };
    if (unset("sixty_fps"))
        if (auto v = t["gameplay"]["force_60fps"].value<bool>()) s.graphics.sixty = *v;
    if (unset("show_fps"))
        if (auto v = t["debug"]["show_fps"].value<bool>()) s.graphics.show_fps = *v;
}

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().wstring());
    } catch (...) {
        // none yet (first run) or unreadable: the defaults, and the game's load hook and the enhancements file
        // still run
        t = toml::table{};
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    if (const toml::table* g = t["tuning"].as_table())
        for (const GraphicsField& f : TuningFields(s.tuning)) {
            if (f.f) {
                float v = (*g)[f.key].value_or(*f.f);
                if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
            }
            if (f.b) *f.b = (*g)[f.key].value_or(*f.b);
        }
    if (const toml::table* g = t["graphics"].as_table()) {
        for (const GraphicsField& f : GraphicsFields(s.graphics)) {
            if (f.f) {
                float v = (*g)[f.key].value_or(*f.f);
                if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
            }
            if (f.b) *f.b = (*g)[f.key].value_or(*f.b);
            if (f.u) *f.u = std::min<uint32_t>((*g)[f.key].value_or(*f.u), uint32_t(f.hi));
        }
    }
    LoadEnhancements(s, t["graphics"].as_table());
    s.render_percent = std::clamp(s.render_percent, 25u, 400u);
    s.width = std::min(s.width, resolution::kMaxDimension);
    s.height = std::min(s.height, resolution::kMaxDimension);
    if (!s.width || !s.height) s.width = s.height = 0;
    if (!resolution::ValidScene({s.graphics.scene_width, s.graphics.scene_height}))
        s.graphics.scene_width = s.graphics.scene_height = 0;
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    toml::table g;
    for (const GraphicsField& f : GraphicsFields(const_cast<Settings&>(s).graphics)) {
        if (f.f) g.insert(f.key, double(*f.f));
        if (f.b) g.insert(f.key, *f.b);
        if (f.u) g.insert(f.key, int64_t(*f.u));
    }
    t.insert("graphics", std::move(g));
    toml::table v;
    for (const GraphicsField& f : TuningFields(const_cast<Settings&>(s).tuning)) {
        if (f.f) v.insert(f.key, double(*f.f));
        if (f.b) v.insert(f.key, *f.b);
    }
    t.insert("tuning", std::move(v));
    return t;
}

toml::table EnhancementsTable() {
    const Settings& s = Get();
    toml::table t;
    t.insert("render", toml::table{{"force_1080p", s.force_1080p}, {"force_widescreen", s.force_widescreen}});
    t.insert("input", toml::table{{"deadzone_inner", double(s.deadzone_inner)}, {"deadzone_outer", double(s.deadzone_outer)}});
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
    std::error_code ec;
    std::filesystem::create_directories(EnhancementsFile().parent_path(), ec);
    std::ofstream ef(EnhancementsFile());
    ef << EnhancementsTable() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table() << "\n" << hostpath::Utf8(EnhancementsFile().filename()) << ":\n" << EnhancementsTable();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.force_1080p) return {1920, 1080};
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

Mode RenderResolution() {
    const Settings& s = Get();
    return resolution::Scene({s.graphics.scene_width, s.graphics.scene_height}, Output(), s.render_percent);
}

namespace { std::atomic<uint64_t> g_actual_scene{0}; }
Mode ActualRenderResolution() {
    uint64_t v = g_actual_scene.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
void ReportRenderResolution(Mode m) {
    g_actual_scene.store(uint64_t(m.width) << 32 | m.height, std::memory_order_relaxed);
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<uint64_t> scene_key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    // the two force switches are in the key too: they change from the settings menu while the game runs
    uint64_t k = uint64_t(s.width) << 34 | uint64_t(s.height) << 2 | uint64_t(s.force_widescreen) << 1 | uint64_t(s.force_1080p);
    uint64_t sk = uint64_t(s.graphics.scene_width) << 32 | s.graphics.scene_height;
    bool changed = key.exchange(k) != k;
    changed |= scene_key.exchange(sk) != sk;
    if (changed) {
        Mode explicit_scene{s.graphics.scene_width, s.graphics.scene_height};
        Mode m = resolution::ValidScene(explicit_scene) ? explicit_scene : Output();
        wide = s.force_widescreen || m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    float h = float(RenderResolution().height);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

// the renderer (gpu/d3d12.cpp) declares and calls this name; force_1080p is already in Output(), so it is RenderScale
float EnhancedRenderScale(uint32_t guest_height) { return RenderScale(guest_height); }

}  // namespace app
