// Arrow keys drive while the player is driving.
//
// The keyboard fallback maps the arrows to the D-pad, which the menus need. While driving, though, D-pad left/right
// changes the radio song (a new music stream each press), so a player steering with the arrows skipped songs on
// every hard turn. While the player drives, held arrows become the controls a driver means: left/right steer (the left
// stick), up gas, down brake, and their D-pad bits are taken back. A controller's own D-pad is untouched; the arrows
// are read from the keyboard itself.
//
// Driving is the human car controller updating: its vtable (0x47684C, garage.cpp) method 8, 0x2C8BC0, runs once a
// frame while the player has the car, standing still included, and stops the moment the pause menu (or any menu)
// takes over; methods 2, 7 and 14 do the same. Each call renews a short lease, so the arrows are the D-pad again once
// it lapses. Game time and the time scale (0x4B2D6C) both keep running under the pause menu, so neither can tell.
#include <atomic>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace mc3 {
namespace {

constexpr uint32_t kHumanController = 0x47684C;  // the player's car controller vtable
constexpr uint64_t kLeaseMs = 150;  // several 60 Hz updates, so one late frame does not hand the arrows back
// D-pad bits and pad slots, as input::KeyboardPad sets them
constexpr uint16_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8;
constexpr int kBrake = 6, kGas = 7;

std::atomic<uint64_t> g_driving_until{0};

bool Driving() {
    const bool driving = GetTickCount64() < g_driving_until.load(std::memory_order_relaxed);
    static std::atomic<bool> was{false};
    if (was.exchange(driving) != driving) {
        static std::atomic<unsigned> changes{0};
        if (changes.fetch_add(1) < 64) LOGR("input: arrow keys %s", driving ? "drive (steer, gas, brake)" : "are the D-pad");
    }
    return driving;
}

}  // namespace

// GameInfo::pad: the first controller's arrows as driving controls while driving
void ArrowDriving(int port, input::Pad& pad) {
    if (port != 0 || !Driving()) return;
    const HWND window = static_cast<HWND>(app::Window());
    if (!window || window != GetForegroundWindow()) return;
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const bool left = down(VK_LEFT), right = down(VK_RIGHT), up = down(VK_UP), back = down(VK_DOWN);
    if (left || right) {
        pad.buttons &= uint16_t(~(kLeft | kRight));
        if (left != right && !pad.lx) pad.lx = int16_t(right ? 32767 : -32767);
    }
    if (up || back) {
        pad.buttons &= uint16_t(~(kUp | kDown));
        if (up && !back) pad.analog[kGas] = 255;
        if (back && !up) pad.analog[kBrake] = 255;
    }
}

}  // namespace mc3

// the human car controller's per-frame update: the player is driving
X86_EXTERN_FUNC(__imp__sub_002C8BC0);
X86_FUNC(sub_002C8BC0) {
    const uint32_t self = ctx.ecx;
    if (self && mem::IsCommitted(self + 4) && kernel::R32(self) == mc3::kHumanController)
        mc3::g_driving_until.store(GetTickCount64() + mc3::kLeaseMs, std::memory_order_relaxed);
    __imp__sub_002C8BC0(ctx, base);
}
