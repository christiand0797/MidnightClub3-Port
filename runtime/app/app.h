#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks

    // [graphics] in the settings file, changed live from the settings menu (settings_menu.cpp)
    struct Graphics {
        bool post = true;           // the present runs the chain below; off: the console's picture as it was
        uint32_t aa = 1;            // 0 off, 1 FXAA
        float aa_strength = 1.0f;   // 0..1
        float sharpen = 0.35f;      // AMD CAS, 0 off .. 1
        float brightness = 0.0f;    // -0.5..0.5 added
        float contrast = 1.0f;      // 0.5..1.5 about mid grey
        float saturation = 1.0f;    // 0..2
        float vibrance = 0.0f;      // -1..1, boosts dull colours more than vivid ones
        float gamma = 1.0f;         // 0.5..2 exponent applied before the rest
        bool sixty = true;          // 60 fps game timing; false: the console's 30 fps lock
        float motion_blur = 1.0f;   // the game's speed blur, 0 off .. 1 as the console (2 doubled)
        float glow = 1.0f;          // the game's light glow, 0 off .. 2
        float headlights = 1.0f;    // the headlight pools on the road, 0 off .. 2
        float neon = 2.0f;          // car neon underglow (vp_neon / fx_car_neon_glow draws), 0 off, 1 as the console .. 4
        bool show_fps = false;      // a small counter in the corner
        bool texture_pack = true;   // textures/replace/<hash>.png in place of the console's textures
        bool texture_dump = false;  // every texture seen saved to textures/dump/<hash>.png, for making packs
        // 0 as the console; 1 full: the sharpest mip always allowed, no blurring bias, a slight sharpening one, 16x
        // anisotropic; 2 max: only the sharpest mip, whatever the distance (may shimmer)
        uint32_t texture_detail = 1;
        bool hd_movies = true;      // video_hd/<path>.mp4 shown in place of a Bink movie the title plays
        float lod = 1.0f;           // model LOD switch, prop cull and far clip distances, times (lod.cpp)
        float fov_scale = 1.0f;     // the driving camera's vertical field of view, times (fov.cpp)
    } graphics;

    // [tuning] in the settings file, changed live from the settings menu: cars past the garage's steps
    // (templates/game/src/game/tuning.cpp)
    struct Tuning {
        bool garage_extended = true;          // extended choices in the game's wheel/suspension menus
        float ride_front = 0, ride_rear = 0;  // metres added to each axle's suspension offset, + raises
        float wheel_limit = 1.6f;             // the largest wheel and tire each car takes, times the car's own
    } tuning;

    // config/mc3_enhancements.toml beside the exe (settings.cpp), changed live from the settings menu
    bool force_1080p = false;       // [render] the output is 1920x1080 whatever width/height say (app::Output)
    bool force_widescreen = false;  // [render] the console reports a widescreen TV whatever the output's shape
    // [input] radial stick deadzones, both sticks of every controller (core/input.cpp): below inner the stick reads
    // centred, from outer on it reads full, between the two it is rescaled so the whole range stays usable
    float deadzone_inner = 0.15f;   // 0..0.9 of full deflection
    float deadzone_outer = 0.95f;   // inner + 0.05 .. 1
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// the in-game graphics settings menu (settings_menu.cpp): F1, or Back + Y on a controller. While it is open the
// game's controllers read neutral and the menu takes the input.
bool MenuOpen();
void ToggleMenu();
void DrawSettingsMenu(float fps);  // the renderer, inside its ImGui frame, under UiLock

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
