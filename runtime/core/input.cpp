// SDL controllers, foreground keyboard fallback, and optional deterministic controller scripts.
#include "core/input.h"
#include "core/input_mapping.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <SDL3/SDL.h>

#include "app/app.h"
#include "core/log.h"
#include "core/host_path.h"
#include "gpu/backend.h"

namespace input {

namespace {

// a controller takes the lowest free port when it is plugged in and keeps it until it is unplugged
std::mutex g_lock;
bool g_sdl = false;
SDL_Gamepad* g_pads[4] = {};
bool g_script_enabled = false;
Pad g_raw[4];             // each port as last polled, before the menu blanks it
bool g_menu_combo = false;  // Back + Y held at the last poll
GarageNavigation g_garage_navigation;
std::vector<PadScriptPress> g_script;
std::optional<std::chrono::steady_clock::time_point> g_script_start;

void LoadPadScript() {
    const wchar_t* value = _wgetenv(L"XBE_PAD_SCRIPT");
    if (!value || !*value) return;
    // Script playback is optional and additive; an empty/invalid script never takes physical input away.
    g_script_enabled = true;
    std::filesystem::path path(value);
    if (!path.is_absolute()) {
        LOGE("input: XBE_PAD_SCRIPT requires an absolute path");
        return;
    }
    std::ifstream file(path);
    if (!file) {
        LOGE("input: could not open XBE_PAD_SCRIPT");
        return;
    }
    std::vector<PadScriptPress> presses;
    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        PadScriptPress press;
        switch (ParsePadScriptLine(line, press)) {
        case PadScriptLine::Empty: break;
        case PadScriptLine::Valid: presses.push_back(std::move(press)); break;
        case PadScriptLine::Invalid:
            LOGE("input: invalid XBE_PAD_SCRIPT line %zu; expected milliseconds buttons duration_ms", line_number);
            return;
        }
    }
    if (file.bad()) {
        LOGE("input: failed while reading XBE_PAD_SCRIPT");
        return;
    }
    g_script = std::move(presses);
    LOGR("input: controller script enabled, %zu presses; time starts at first port 0 poll", g_script.size());
}

// XBE_PAD_LIVE: a file a test harness appends "buttons duration_ms" lines to; each new line is pressed on port 0
// from the moment it is read, on top of the player's controller and keyboard. Lets a run be driven step by step
// without window focus or a fixed timeline, and without taking the controls away from the player.
std::filesystem::path g_live_path;
std::vector<PadScriptPress> g_live;
const auto g_live_start = std::chrono::steady_clock::now();
uintmax_t g_live_read = 0;
std::string g_live_partial;
std::chrono::steady_clock::time_point g_live_checked;

void LoadPadLive() {
    const wchar_t* value = _wgetenv(L"XBE_PAD_LIVE");
    if (!value || !*value) return;
    g_live_path = value;
    std::error_code ec;
    g_live_read = std::filesystem::exists(g_live_path, ec) ? std::filesystem::file_size(g_live_path, ec) : 0;
    LOGR("input: live controller file %s, its presses add to port 0", hostpath::Utf8(g_live_path).c_str());
}

// caller holds g_lock; the presses held now
Pad LivePad() {
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_live_start).count();
    if (now - g_live_checked < std::chrono::milliseconds(20)) return ScriptPad(g_live, elapsed_ms);
    g_live_checked = now;
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(g_live_path, ec);
    if (ec || size <= g_live_read) {
        if (!ec && size < g_live_read) g_live_read = size;  // truncated: start over from its end
        return ScriptPad(g_live, elapsed_ms);
    }
    std::ifstream file(g_live_path, std::ios::binary);
    file.seekg(std::streamoff(g_live_read));
    std::string chunk(size_t(size - g_live_read), '\0');
    file.read(chunk.data(), std::streamsize(chunk.size()));
    g_live_read += uintmax_t(file.gcount());
    g_live_partial.append(chunk.data(), size_t(file.gcount()));
    size_t nl;
    while ((nl = g_live_partial.find('\n')) != std::string::npos) {
        std::string line = g_live_partial.substr(0, nl);
        g_live_partial.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "CAPTURE") {  // the next frame is saved as F3 would, with XBE_CAPTURE_DIR set
            LOGR("input: live capture requested");
            gpu::g_capture = true;
            continue;
        }
        PadScriptPress press;
        switch (ParsePadScriptLine("0 " + line, press)) {
            case PadScriptLine::Empty: break;
            case PadScriptLine::Valid:
                press.milliseconds = elapsed_ms;
                LOGR("input: live press %s for %lld ms", press.buttons.c_str(), (long long)press.duration_ms);
                g_live.push_back(std::move(press));
                break;
            case PadScriptLine::Invalid: LOGE("input: live controller line not understood: %s", line.c_str()); break;
        }
    }
    // presses that ended long ago no longer matter
    std::erase_if(g_live, [&](const PadScriptPress& x) { return x.milliseconds + x.duration_ms < elapsed_ms - 1000; });
    return ScriptPad(g_live, elapsed_ms);
}

Pad PollKeyboard() {
    HWND window = static_cast<HWND>(app::Window());
    const bool foreground = window && window == GetForegroundWindow();
    if (!foreground) return KeyboardPad({}, false);
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    KeyboardState keys;
    keys.enter = down(VK_RETURN);
    keys.space = down(VK_SPACE);
    keys.escape = down(VK_ESCAPE);
    keys.backspace = down(VK_BACK);
    keys.up = down(VK_UP);
    keys.down = down(VK_DOWN);
    keys.left = down(VK_LEFT);
    keys.right = down(VK_RIGHT);
    keys.w = down('W');
    keys.s = down('S');
    keys.a = down('A');
    keys.d = down('D');
    keys.q = down('Q');
    keys.e = down('E');
    keys.x = down('X');
    keys.y = down('Y');
    return KeyboardPad(keys, true);
}

void AddPad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == id) return;
    SDL_Gamepad** slot = std::find(std::begin(g_pads), std::end(g_pads), nullptr);
    if (slot == std::end(g_pads)) return;
    *slot = SDL_OpenGamepad(id);
    if (slot == g_pads) g_garage_navigation.Reset();
    const char* name = *slot ? SDL_GetGamepadName(*slot) : SDL_GetError();
    LOGI("input: controller %d %s: %s", int(slot - g_pads) + 1, *slot ? "plugged in" : "failed to open", name ? name : "");
}

void RemovePad(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) {
        if (!g_pads[i] || SDL_GetGamepadID(g_pads[i]) != id) continue;
        SDL_CloseGamepad(g_pads[i]);
        g_pads[i] = nullptr;
        if (i == 0) g_garage_navigation.Reset();
        LOGI("input: controller %d unplugged", i + 1);
    }
}

// app::Settings deadzone_inner/outer on one stick, radial: the direction is kept and the length remapped so inner
// reads centred and outer reads full. Each axis is clamped after, not the length, so a pad whose gate reaches the
// corners still reads full on both axes there.
void Deadzone(int16_t& x, int16_t& y) {
    const app::Settings& s = app::Get();
    const float inner = std::clamp(s.deadzone_inner, 0.0f, 0.9f);
    const float outer = std::clamp(s.deadzone_outer, inner + 0.05f, 1.0f);
    if (inner <= 0.0f && outer >= 1.0f) return;  // as the pad reports it
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

// once a millisecond at most, however many ports and threads read
void UpdatePads() {
    static uint64_t last = 0;
    uint64_t now = SDL_GetTicks();
    if (!g_sdl || now == last) return;
    last = now;
    // Guest threads poll controllers; SDL_PumpEvents is restricted to the main thread.
    // This API explicitly supports any thread and refreshes joystick/gamepad state directly.
    SDL_UpdateGamepads();
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) == 1) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) AddPad(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) RemovePad(e.gdevice.which);
    }
}

}  // namespace

void Init() {
    {
        std::lock_guard lock(g_lock);
        LoadPadScript();
        LoadPadLive();
    }
    LOGR("input: keyboard port 0 fallback: Enter Start, Space A, Escape B, Backspace Back, arrows D-pad, W/S gas/brake, A/D steer, Q/E black/white, X/Y");
    // WGI reads the triggers apart and HIDAPI takes PlayStation and Switch pads. Reads come from the guest's threads,
    // so SDL's own thread takes the device notices.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOGE("input: SDL controller initialization failed; keyboard fallback remains available: %s", SDL_GetError());
        return;
    }
    int v = SDL_GetVersion();
    LOGI("input: SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    std::lock_guard lock(g_lock);
    g_sdl = true;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; i++) AddPad(ids[i]);
        SDL_free(ids);
    }
}

Pad Poll(int port) {
    Pad p;
    {
        std::lock_guard lock(g_lock);
        UpdatePads();
        SDL_Gamepad* pad = port >= 0 && port < 4 ? g_pads[port] : nullptr;
        if (pad) {
            p.connected = true;
            constexpr SDL_GamepadButton kButtons[] = {
                SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
                SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
            for (int i = 0; i < 8; i++) p.buttons |= uint16_t(SDL_GetGamepadButton(pad, kButtons[i]) << i);
            // A B X Y black white
            constexpr SDL_GamepadButton kAnalog[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                                     SDL_GAMEPAD_BUTTON_WEST,  SDL_GAMEPAD_BUTTON_NORTH,
                                                     SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
            for (int i = 0; i < 6; i++) p.analog[i] = SDL_GetGamepadButton(pad, kAnalog[i]) ? 0xFF : 0;
            auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
            // 0 to 32767 into 0 to 255
            p.analog[6] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), 0) >> 7);
            p.analog[7] = uint8_t(std::max(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), 0) >> 7);
            // SDL's sticks have y down
            // not plain negation: -32768 has no positive
            p.lx = int16_t(axis(SDL_GAMEPAD_AXIS_LEFTX));
            p.ly = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_LEFTY));
            p.rx = int16_t(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            p.ry = int16_t(-1 - axis(SDL_GAMEPAD_AXIS_RIGHTY));
            Deadzone(p.lx, p.ly);
            Deadzone(p.rx, p.ry);
        }
        // Keyboard remains usable while a controller is paired. Neutral keys preserve its analog sticks/triggers.
        if (port == 0) {
            p = MergeKeyboard(p, PollKeyboard());
            if (g_script_enabled) {
                const auto now = std::chrono::steady_clock::now();
                if (!g_script_start) g_script_start = now;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *g_script_start).count();
                p = MergeKeyboard(p, ScriptPad(g_script, elapsed));
            }
            if (!g_live_path.empty()) p = MergeKeyboard(p, LivePad());
        }
    }
    bool toggle_menu = false;
    {
        std::lock_guard lock(g_lock);
        if (port >= 0 && port < 4) g_raw[port] = p;
        // Back + Y on the first controller opens and closes the settings menu
        if (port == 0) {
            bool combo = (p.buttons & 32) && p.analog[3] > 128;
            toggle_menu = combo && !g_menu_combo;
            g_menu_combo = combo;
            const HWND window = static_cast<HWND>(app::Window());
            const bool foreground = window && window == GetForegroundWindow();
            // RawPad above keeps the complete physical/keyboard/script merge for the PC GUI. Shape only the
            // original garage's direction navigation, after the merge, and only while its fresh lease is live.
            p = g_garage_navigation.Apply(p, !app::MenuOpen() && app::GarageNavigationActive(), foreground,
                                         GetTickCount64());
        }
    }
    // The UI samples RawPad while holding UiLock; never acquire that lock while g_lock is held.
    if (toggle_menu) app::ToggleMenu();
    // the menu has the controls: the game sees a pad with nothing held
    if (app::MenuOpen()) {
        Pad idle;
        idle.connected = p.connected;
        return idle;
    }
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

Pad RawPad(int port) {
    std::lock_guard lock(g_lock);
    return port >= 0 && port < 4 ? g_raw[port] : Pad{};
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    std::lock_guard lock(g_lock);
    if (port >= 0 && port < 4 && g_pads[port]) SDL_RumbleGamepad(g_pads[port], left, right, 0);
}

}  // namespace input
