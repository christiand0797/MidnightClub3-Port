// Browser port: the host pieces the WebAssembly build supplies in place of Windows-only runtime parts
// (docs/notes/browser_port_plan.md).
//
// - input: player 1's pad comes from the page (keyboard/gamepad) through web_set_pad, exported to JavaScript.
// - tuning profiles / garage tuning button: PC metadata kept beside settings on Windows; not in the browser yet.
// - HD movies: the Windows build swaps Bink movies for HD files through Media Foundation; the browser plays the
//   original Bink video (Open returns false).
// - pthread_setname_np: thread names are a debugging aid; Emscripten's pthreads have none.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>

#include "app/tuning_profiles.h"
#include "core/input.h"
#include "gpu/video_hd.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {
std::mutex g_pad_lock;
input::Pad g_pad;  // port 0, as the page last reported it
}  // namespace

// buttons: dpad up/down/left/right, start, back, left thumb, right thumb (input::Pad bits); analog: A B X Y black white
// LT RT, 0-255; sticks -32768..32767
extern "C" EMSCRIPTEN_KEEPALIVE void web_set_pad(int connected, int buttons, const uint8_t* analog, int lx, int ly,
                                                 int rx, int ry) {
    std::lock_guard lock(g_pad_lock);
    g_pad.connected = connected != 0;
    g_pad.buttons = uint16_t(buttons);
    if (analog) std::memcpy(g_pad.analog, analog, sizeof(g_pad.analog));
    g_pad.lx = int16_t(lx);
    g_pad.ly = int16_t(ly);
    g_pad.rx = int16_t(rx);
    g_pad.ry = int16_t(ry);
}

namespace input {
Pad Poll(int port, bool) {
    if (port != 0) return {};
    std::lock_guard lock(g_pad_lock);
    return g_pad;
}
}  // namespace input

namespace app {
void GarageTuningActive() {}
void EndGarageNavigation() {}
}  // namespace app

namespace app::tuning_profiles {
void Load() {}
bool Save() { return true; }
Settings::Tuning EffectiveForKey(const std::string&) { return Get().tuning; }
void Observe(const std::string&, const std::string&, const std::vector<Car>&, uint32_t) {}
void ClearSelection() {}
std::string KeyForSlot(uint32_t) { return {}; }
bool Removed(const std::string&, uint32_t, const std::vector<Car>&, const std::vector<Car>&, uint32_t) { return false; }
}  // namespace app::tuning_profiles

namespace gpu::video {
bool Open(uint32_t, const std::string&) { return false; }
void Frame(uint32_t, uint32_t) {}
void Close(uint32_t) {}
}  // namespace gpu::video

#ifdef __EMSCRIPTEN__
extern "C" int pthread_setname_np(pthread_t, const char*) { return 0; }
#endif
