// Browser port: the host pieces the WebAssembly build supplies in place of Windows-only runtime parts
// (docs/notes/browser_port_plan.md).
//
// - input: player 1's pad comes from the page (keyboard/gamepad) through web_set_pad, exported to JavaScript, with
//   radial stick deadzones, garage direction-pulse navigation, Back+Y menu toggle, and ArrowDriving support.
// - settings / vehicle tuning / world / career money: exported to the page's F1 / F2 overlay and persisted to OPFS.
// - HD movies: the Windows build swaps Bink movies for HD files through Media Foundation; the browser plays the
//   original Bink video (Open returns false).
// - pthread_setname_np: thread names are a debugging aid; Emscripten's pthreads have none.
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
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

#include "app/app.h"
#include "app/tuning_profiles.h"
#include "app/resolution.h"
#include "core/input.h"
#include "core/input_mapping.h"
#include "core/json_lite.h"
#include "core/log.h"
#include "gpu/video_hd.h"
#include "../templates/game/src/game/economy.h"
#include "../templates/game/src/game/world_tuning.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace app {
void SetDesktopForWeb(uint32_t w, uint32_t h);
}

namespace {
std::mutex g_pad_lock;
input::Pad g_pad;  // port 0, as the page last reported it
input::Pad g_raw;  // port 0 after deadzone, before menu blanking
uint16_t g_kb_arrows = 0;  // kUp=1, kDown=2, kLeft=4, kRight=8 held on browser keyboard
bool g_menu_combo = false;
input::GarageNavigation g_garage_navigation;

std::atomic<bool> g_menu_open{false};
std::atomic<int> g_menu_tab{0};  // 1 = Display/Quality, 2 = Vehicle, 3 = Lobby
std::atomic<uint64_t> g_garage_hint_until{0};
std::atomic<uint64_t> g_garage_navigation_until{0};

void Deadzone(int16_t& x, int16_t& y) {
    const app::Settings& s = app::Get();
    const float inner = std::clamp(s.deadzone_inner, 0.0f, 0.9f);
    const float outer = std::clamp(s.deadzone_outer, inner + 0.05f, 1.0f);
    if (inner <= 0.0f && outer >= 1.0f) return;
    const float fx = std::max(x / 32767.0f, -1.0f), fy = std::max(y / 32767.0f, -1.0f);
    const float len = std::sqrt(fx * fx + fy * fy);
    if (len <= inner) {
        x = y = 0;
        return;
    }
    const float k = (len - inner) / (outer - inner) / len;
    x = int16_t(std::lround(std::clamp(fx * k, -1.0f, 1.0f) * 32767.0f));
    y = int16_t(std::lround(std::clamp(fy * k, -1.0f, 1.0f) * 32767.0f));
}
}  // namespace

namespace mc3 {
uint16_t WebKeyboardArrows() {
    std::lock_guard lock(g_pad_lock);
    return g_kb_arrows;
}
}  // namespace mc3

// buttons: dpad up/down/left/right, start, back, left thumb, right thumb (input::Pad bits);
// bits 8..11 of `buttons` carry browser keyboard arrow state (kUp=1, kDown=2, kLeft=4, kRight=8) for ArrowDriving;
// analog: A B X Y black white LT RT, 0-255; sticks -32768..32767
extern "C" EMSCRIPTEN_KEEPALIVE void web_set_pad(int connected, int buttons, const uint8_t* analog, int lx, int ly,
                                                 int rx, int ry) {
    std::lock_guard lock(g_pad_lock);
    g_pad.connected = connected != 0;
    g_pad.buttons = uint16_t(buttons & 0xFF);
    g_kb_arrows = uint16_t((buttons >> 8) & 0x0F);
    if (analog) std::memcpy(g_pad.analog, analog, sizeof(g_pad.analog));
    g_pad.lx = int16_t(lx);
    g_pad.ly = int16_t(ly);
    g_pad.rx = int16_t(rx);
    g_pad.ry = int16_t(ry);
}

namespace input {
void Init() {}
void KeyboardWindowMessage(void*, uint32_t, uintptr_t, intptr_t) {}
void KeyboardMenuChanged(bool) {}

Pad Poll(int port, bool) {
    if (port != 0) return {};
    Pad p;
    bool toggle_menu = false;
    {
        std::lock_guard lock(g_pad_lock);
        p = g_pad;
        Deadzone(p.lx, p.ly);
        Deadzone(p.rx, p.ry);
        g_raw = p;
        const bool combo = (p.buttons & 32) && p.analog[3] > 128;
        toggle_menu = combo && !g_menu_combo;
        g_menu_combo = combo;
        p = g_garage_navigation.Apply(p, !app::MenuOpen() && app::GarageNavigationActive(), true, GetTickCount64());
    }
    if (toggle_menu) app::ToggleMenu();
    if (app::MenuOpen()) {
        Pad idle;
        idle.connected = p.connected;
        return idle;
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

Pad RawPad(int port) {
    std::lock_guard lock(g_pad_lock);
    return port == 0 ? g_raw : Pad{};
}

uint16_t HeldOnAnyController() {
    std::lock_guard lock(g_pad_lock);
    uint16_t held = 0;
    if (g_raw.analog[0] > 128) held |= 0x1000;
    if (g_raw.analog[1] > 128) held |= 0x2000;
    if (g_raw.analog[2] > 128) held |= 0x4000;
    if (g_raw.analog[3] > 128) held |= 0x8000;
    return held;
}

void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
}
}  // namespace input

namespace app {
bool MenuOpen() { return g_menu_open.load(std::memory_order_relaxed); }

void ToggleMenu() {
    std::lock_guard lock(UiLock());
    const bool was = g_menu_open.load(std::memory_order_relaxed);
    g_menu_open.store(!was, std::memory_order_relaxed);
    if (!was && g_menu_tab.load(std::memory_order_relaxed) == 0)
        g_menu_tab.store(1, std::memory_order_relaxed);
    if (was) Save();
}

void OpenVehicleMenu() {
    std::lock_guard lock(UiLock());
    g_menu_tab.store(2, std::memory_order_relaxed);
    g_menu_open.store(true, std::memory_order_relaxed);
}

void OpenLobby() {
    std::lock_guard lock(UiLock());
    g_menu_tab.store(3, std::memory_order_relaxed);
    g_menu_open.store(true, std::memory_order_relaxed);
}

void GarageTuningActive() {
    const uint64_t now = GetTickCount64();
    g_garage_hint_until.store(now + 750, std::memory_order_relaxed);
    g_garage_navigation_until.store(now + 100, std::memory_order_relaxed);
}

bool GarageNavigationActive() {
    return GetTickCount64() < g_garage_navigation_until.load(std::memory_order_relaxed);
}

void EndGarageNavigation() {
    g_garage_navigation_until.store(0, std::memory_order_relaxed);
}
}  // namespace app

// ---------- Browser UI / Settings / Tuning / World / Economy C Exports ----------

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_desktop(int w, int h) {
    app::SetDesktopForWeb(uint32_t(std::max(w, 320)), uint32_t(std::max(h, 240)));
}

extern "C" EMSCRIPTEN_KEEPALIVE int web_ui_state() {
    // bit 0: menu open, bit 1: garage tuning hint active, bits 2..3: requested tab (1=Settings, 2=Vehicle, 3=Lobby)
    int state = app::MenuOpen() ? 1 : 0;
    if (GetTickCount64() < g_garage_hint_until.load(std::memory_order_relaxed)) state |= 2;
    state |= (g_menu_tab.load(std::memory_order_relaxed) & 3) << 2;
    return state;
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_menu(int open, int tab) {
    std::lock_guard lock(app::UiLock());
    const bool was = g_menu_open.load(std::memory_order_relaxed);
    if (tab > 0) g_menu_tab.store(tab, std::memory_order_relaxed);
    g_menu_open.store(open != 0, std::memory_order_relaxed);
    if (was && !open) app::Save();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* web_get_settings_json() {
    static std::string buf;
    std::lock_guard lock(app::UiLock());
    const auto& s = app::Get();
    const auto& g = s.graphics;
    const auto prof = app::tuning_profiles::Snapshot();
    const auto& t = app::tuning_profiles::Editor();
    const auto econ = mc3::economy::ReadSnapshot();
    const auto world = mc3::world::GetSnapshot();
    const auto out = app::Output();
    const auto req_scene = app::RenderResolution();
    const auto act_scene = app::ActualRenderResolution();

    std::ostringstream os;
    os << "{";
    os << "\"menu_open\":" << (app::MenuOpen() ? "true" : "false") << ",";
    os << "\"menu_tab\":" << g_menu_tab.load(std::memory_order_relaxed) << ",";
    os << "\"garage_hint\":" << (GetTickCount64() < g_garage_hint_until.load(std::memory_order_relaxed) ? "true" : "false") << ",";
    // Display & Resolution
    os << "\"width\":" << s.width << ",\"height\":" << s.height << ",";
    os << "\"render_percent\":" << s.render_percent << ",";
    os << "\"scene_width\":" << g.scene_width << ",\"scene_height\":" << g.scene_height << ",";
    os << "\"force_1080p\":" << (s.force_1080p ? "true" : "false") << ",";
    os << "\"force_widescreen\":" << (s.force_widescreen ? "true" : "false") << ",";
    os << "\"out_w\":" << out.width << ",\"out_h\":" << out.height << ",";
    os << "\"req_w\":" << req_scene.width << ",\"req_h\":" << req_scene.height << ",";
    os << "\"act_w\":" << act_scene.width << ",\"act_h\":" << act_scene.height << ",";
    os << "\"sixty_fps\":" << (g.sixty ? "true" : "false") << ",";
    os << "\"show_fps\":" << (g.show_fps ? "true" : "false") << ",";
    // Quality & Effects
    os << "\"aniso\":" << s.aniso << ",";
    os << "\"texture_detail\":" << g.texture_detail << ",";
    os << "\"offscreen_scale\":" << g.offscreen_scale << ",";
    os << "\"aa\":" << g.aa << ",";
    os << "\"aa_strength\":" << g.aa_strength << ",";
    os << "\"sharpen\":" << g.sharpen << ",";
    os << "\"motion_blur\":" << g.motion_blur << ",";
    os << "\"glow\":" << g.glow << ",";
    os << "\"headlights\":" << g.headlights << ",";
    os << "\"neon\":" << g.neon << ",";
    os << "\"lod\":" << g.lod << ",";
    os << "\"fov_scale\":" << g.fov_scale << ",";
    // Enhanced Car Paint
    os << "\"car_paint\":" << (g.car_paint ? "true" : "false") << ",";
    os << "\"car_relight\":" << g.car_relight << ",";
    os << "\"car_specular\":" << g.car_specular << ",";
    os << "\"car_roughness\":" << g.car_roughness << ",";
    os << "\"car_reflection\":" << g.car_reflection << ",";
    // Picture / Post-Processing
    os << "\"post\":" << (g.post ? "true" : "false") << ",";
    os << "\"brightness\":" << g.brightness << ",";
    os << "\"contrast\":" << g.contrast << ",";
    os << "\"saturation\":" << g.saturation << ",";
    os << "\"vibrance\":" << g.vibrance << ",";
    os << "\"gamma\":" << g.gamma << ",";
    // Input
    os << "\"deadzone_inner\":" << s.deadzone_inner << ",";
    os << "\"deadzone_outer\":" << s.deadzone_outer << ",";
    // World
    os << "\"traffic_multiplier\":" << s.world.traffic_multiplier << ",";
    os << "\"pedestrian_multiplier\":" << s.world.pedestrian_multiplier << ",";
    os << "\"world_active\":" << (world.free_roam_player ? "true" : "false") << ",";
    os << "\"world_roads\":" << world.traffic_roads_ready << ",";
    os << "\"world_peds\":" << (world.pedestrians_ready ? "true" : "false") << ",";
    // Career Money
    os << "\"econ_ready\":" << (econ.ready ? "true" : "false") << ",";
    os << "\"econ_pending\":" << (econ.pending ? "true" : "false") << ",";
    os << "\"econ_money\":" << econ.money << ",";
    os << "\"econ_token\":" << econ.career_token << ",";
    os << "\"econ_status\":" << json::Quote(mc3::economy::StatusText(econ.status)) << ",";
    // Vehicle Tuning & Per-Car Profile
    os << "\"prof_ready\":" << (prof.ready ? "true" : "false") << ",";
    os << "\"prof_enabled\":" << (prof.profiles_enabled ? "true" : "false") << ",";
    os << "\"prof_global\":" << (prof.global_editing ? "true" : "false") << ",";
    os << "\"prof_override\":" << (prof.override_enabled ? "true" : "false") << ",";
    os << "\"prof_readonly\":" << (prof.storage_read_only ? "true" : "false") << ",";
    os << "\"prof_slot\":" << prof.slot << ",";
    os << "\"prof_model\":" << json::Quote(prof.model) << ",";
    os << "\"prof_career\":" << json::Quote(prof.profile) << ",";
    os << "\"prof_status\":" << json::Quote(prof.status) << ",";
    os << "\"garage_extended\":" << (s.tuning.garage_extended ? "true" : "false") << ",";
    os << "\"wheel_limit\":" << s.tuning.wheel_limit << ",";
    os << "\"ride_front\":" << t.ride_front << ",\"ride_rear\":" << t.ride_rear << ",";
    os << "\"wheel_tuck\":" << (t.wheel_tuck ? "true" : "false") << ",";
    os << "\"camber_front\":" << t.camber_front << ",\"camber_rear\":" << t.camber_rear << ",";
    os << "\"offset_front\":" << t.offset_front << ",\"offset_rear\":" << t.offset_rear << ",";
    os << "\"airbag_match_ride\":" << (t.airbag_match_ride ? "true" : "false") << ",";
    os << "\"spring_front\":" << t.spring_front << ",\"spring_rear\":" << t.spring_rear << ",";
    os << "\"damper_front\":" << t.damper_front << ",\"damper_rear\":" << t.damper_rear << ",";
    os << "\"brake_front\":" << t.brake_front << ",\"brake_rear\":" << t.brake_rear << ",";
    os << "\"grip_front\":" << t.grip_front << ",\"grip_rear\":" << t.grip_rear << ",";
    os << "\"engine_power\":" << t.engine_power;
    os << "}";
    buf = os.str();
    return buf.c_str();
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_set_setting(const char* key_cstr, double val) {
    if (!key_cstr) return;
    std::string k(key_cstr);
    std::lock_guard lock(app::UiLock());
    auto& s = app::Get();
    auto& g = s.graphics;
    auto& t = app::tuning_profiles::Editor();
    const auto prev_t = t;
    bool b = val != 0.0;
    float f = float(val);
    uint32_t u = uint32_t(std::max(0.0, val));

    // Display & Resolution
    if (k == "width") s.width = std::min(u, app::resolution::kMaxDimension);
    else if (k == "height") s.height = std::min(u, app::resolution::kMaxDimension);
    else if (k == "render_percent") s.render_percent = std::clamp(u, 25u, 400u);
    else if (k == "scene_width") g.scene_width = std::min(u, app::resolution::kMaxDimension);
    else if (k == "scene_height") g.scene_height = std::min(u, app::resolution::kMaxDimension);
    else if (k == "force_1080p") s.force_1080p = b;
    else if (k == "force_widescreen") s.force_widescreen = b;
    else if (k == "sixty_fps") { g.sixty = b; s.fps_cap = b ? 60 : 30; }
    else if (k == "show_fps") g.show_fps = b;
    // Quality & Effects
    else if (k == "aniso") s.aniso = std::min(u, 16u);
    else if (k == "texture_detail") g.texture_detail = std::min(u, 2u);
    else if (k == "offscreen_scale") g.offscreen_scale = std::clamp(f, 1.0f, 3.0f);
    else if (k == "aa") g.aa = std::min(u, 1u);
    else if (k == "aa_strength") g.aa_strength = std::clamp(f, 0.0f, 1.0f);
    else if (k == "sharpen") g.sharpen = std::clamp(f, 0.0f, 1.0f);
    else if (k == "motion_blur") g.motion_blur = std::clamp(f, 0.0f, 2.0f);
    else if (k == "glow") g.glow = std::clamp(f, 0.0f, 2.0f);
    else if (k == "headlights") g.headlights = std::clamp(f, 0.0f, 2.0f);
    else if (k == "neon") g.neon = std::clamp(f, 0.0f, 4.0f);
    else if (k == "lod") g.lod = std::clamp(f, 0.25f, 4.0f);
    else if (k == "fov_scale") g.fov_scale = std::clamp(f, 0.5f, 2.0f);
    // Enhanced Car Paint
    else if (k == "car_paint") g.car_paint = b;
    else if (k == "car_relight") g.car_relight = std::clamp(f, 0.0f, 1.0f);
    else if (k == "car_specular") g.car_specular = std::clamp(f, 0.0f, 3.0f);
    else if (k == "car_roughness") g.car_roughness = std::clamp(f, 0.05f, 0.8f);
    else if (k == "car_reflection") g.car_reflection = std::clamp(f, 0.0f, 2.0f);
    // Picture / Post-Processing
    else if (k == "post") g.post = b;
    else if (k == "brightness") g.brightness = std::clamp(f, -0.5f, 0.5f);
    else if (k == "contrast") g.contrast = std::clamp(f, 0.5f, 1.5f);
    else if (k == "saturation") g.saturation = std::clamp(f, 0.0f, 2.0f);
    else if (k == "vibrance") g.vibrance = std::clamp(f, -1.0f, 1.0f);
    else if (k == "gamma") g.gamma = std::clamp(f, 0.5f, 2.0f);
    // Input
    else if (k == "deadzone_inner") {
        s.deadzone_inner = std::clamp(f, 0.0f, 0.9f);
        s.deadzone_outer = std::max(s.deadzone_outer, s.deadzone_inner + 0.05f);
    } else if (k == "deadzone_outer") {
        s.deadzone_outer = std::clamp(f, s.deadzone_inner + 0.05f, 1.0f);
    }
    // World
    else if (k == "traffic_multiplier") s.world.traffic_multiplier = std::clamp(f, 0.0f, 4.0f);
    else if (k == "pedestrian_multiplier") s.world.pedestrian_multiplier = std::clamp(f, 0.0f, 4.0f);
    // Vehicle Tuning & Profile
    else if (k == "prof_enabled") {
        app::tuning_profiles::SetProfilesEnabled(b);
        app::tuning_profiles::SetGlobalEditing(!b);
    } else if (k == "prof_global") {
        app::tuning_profiles::SetGlobalEditing(b);
    } else if (k == "garage_extended") s.tuning.garage_extended = b;
    else if (k == "wheel_limit") s.tuning.wheel_limit = std::clamp(f, 1.0f, 3.0f);
    else if (k == "ride_front") t.ride_front = std::clamp(f, -2.0f, 2.0f);
    else if (k == "ride_rear") t.ride_rear = std::clamp(f, -2.0f, 2.0f);
    else if (k == "wheel_tuck") t.wheel_tuck = b;
    else if (k == "camber_front") t.camber_front = std::clamp(f, -30.0f, 20.0f);
    else if (k == "camber_rear") t.camber_rear = std::clamp(f, -30.0f, 20.0f);
    else if (k == "offset_front") t.offset_front = std::clamp(f, -0.3f, 0.3f);
    else if (k == "offset_rear") t.offset_rear = std::clamp(f, -0.3f, 0.3f);
    else if (k == "airbag_match_ride") t.airbag_match_ride = b;
    else if (k == "spring_front") t.spring_front = std::clamp(f, 0.25f, 3.0f);
    else if (k == "spring_rear") t.spring_rear = std::clamp(f, 0.25f, 3.0f);
    else if (k == "damper_front") t.damper_front = std::clamp(f, 0.25f, 3.0f);
    else if (k == "damper_rear") t.damper_rear = std::clamp(f, 0.25f, 3.0f);
    else if (k == "brake_front") t.brake_front = std::clamp(f, 0.25f, 3.0f);
    else if (k == "brake_rear") t.brake_rear = std::clamp(f, 0.25f, 3.0f);
    else if (k == "grip_front") t.grip_front = std::clamp(f, 0.5f, 2.0f);
    else if (k == "grip_rear") t.grip_rear = std::clamp(f, 0.5f, 2.0f);
    else if (k == "engine_power") t.engine_power = std::clamp(f, 0.5f, 3.0f);

    if (!(t == prev_t)) app::tuning_profiles::CommitEditor();
}

extern "C" EMSCRIPTEN_KEEPALIVE void web_action(const char* act_cstr, uint32_t arg, uint32_t token) {
    if (!act_cstr) return;
    std::string a(act_cstr);
    std::lock_guard lock(app::UiLock());
    auto& s = app::Get();
    auto& g = s.graphics;
    if (a == "save") {
        app::Save();
    } else if (a == "preset_perf") {
        s.aniso = 4; g.aa = 1; g.aa_strength = 0.75f; g.sharpen = 0.2f; g.texture_detail = 1; g.lod = 0.85f;
        app::Save();
    } else if (a == "preset_bal") {
        s.aniso = 16; g.aa = 1; g.aa_strength = 1.0f; g.sharpen = 0.35f; g.texture_detail = 1; g.lod = 1.0f;
        app::Save();
    } else if (a == "preset_high") {
        s.aniso = 16; g.aa = 1; g.aa_strength = 1.0f; g.sharpen = 0.25f; g.texture_detail = 1; g.lod = 1.5f;
        app::Save();
    } else if (a == "reset_picture") {
        g.brightness = 0; g.contrast = 1; g.saturation = 1; g.vibrance = 0; g.gamma = 1;
        app::Save();
    } else if (a == "reset_deadzone") {
        const app::Settings def;
        s.deadzone_inner = def.deadzone_inner;
        s.deadzone_outer = def.deadzone_outer;
        app::Save();
    } else if (a == "reset_tuning") {
        const app::Settings def;
        auto& t = app::tuning_profiles::Editor();
        const bool ext = t.garage_extended;
        t = def.tuning;
        t.garage_extended = ext;
        app::tuning_profiles::CommitEditor();
        app::Save();
    } else if (a == "prof_copy_global") {
        app::tuning_profiles::CreateOverride();
        app::Save();
    } else if (a == "prof_use_global") {
        app::tuning_profiles::UseGlobal();
        app::Save();
    } else if (a == "set_money") {
        const auto snap = mc3::economy::ReadSnapshot();
        if (snap.ready && uint32_t(snap.career_token) == token)
            mc3::economy::SetMoney(arg, snap.career_token);
    }
}

namespace gpu::video {
bool Open(uint32_t, const std::string&) { return false; }
void Frame(uint32_t, uint32_t) {}
void Close(uint32_t) {}
}  // namespace gpu::video

// ---------- runtime/app/ stubs replaced by the browser ----------
namespace app {

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {}
void Save() {}
Mode Desktop() { return {}; }
Mode Output() { return {}; }
Mode RenderResolution() { return {}; }
Mode ActualRenderResolution() { return {}; }
float RenderScale(uint32_t) { return 1.0f; }
void ReportRenderResolution(Mode) {}
bool Widescreen() { return false; }

void SetDesktopForWeb(uint32_t w, uint32_t h) {
    Get().width = std::max(w, 320u);
    Get().height = std::max(h, 240u);
}

}  // namespace app

namespace app::tuning_profiles {

void Load() {}
bool Save() { return true; }
void ClearSelection() {}
std::string KeyForSlot(uint32_t) { return {}; }
void Observe(const std::string&, const std::string&, const std::vector<Car>&, uint32_t) {}
View Snapshot() { return {}; }
Settings::Tuning& Editor() {
    static Settings::Tuning t;
    return t;
}
bool CommitEditor() { return true; }
Settings::Tuning Effective() { return {}; }
Settings::Tuning EffectiveForKey(const std::string&) { return {}; }
bool SetGlobalEditing(bool) { return true; }
bool SetProfilesEnabled(bool) { return true; }
bool CreateOverride() { return true; }
bool UseGlobal() { return true; }
bool DeleteOverride() { return true; }
bool ResetOverride() { return true; }
bool Removed(const std::string&, uint32_t, const std::vector<Car>&, const std::vector<Car>&, uint32_t) { return false; }

}  // namespace app::tuning_profiles

#ifdef __EMSCRIPTEN__
extern "C" int pthread_setname_np(pthread_t, const char*) { return 0; }
#endif
