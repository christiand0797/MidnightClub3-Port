#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot
// the controller on that port, then GameInfo::pad. Connection-only scans must not consume pending keyboard taps.
Pad Poll(int port, bool consume_keyboard_edges = true);
// Window thread, before ImGui's handler: mapped key transitions/focus loss only. Never suppresses a message.
void KeyboardWindowMessage(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);
// Caller holds UiLock and notifies before publishing the new menu state. Drops queued edges, never held input.
void KeyboardMenuChanged(bool menu_open);
// the port as the player holds it, also while the settings menu has the game reading neutral pads
Pad RawPad(int port);
void Rumble(int port, uint16_t left, uint16_t right);

}  // namespace input
