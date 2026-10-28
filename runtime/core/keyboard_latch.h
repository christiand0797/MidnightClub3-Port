#pragma once

#include "core/input_mapping.h"

#include <array>
#include <cstdint>

namespace input {

enum class KeyboardKey : uint8_t {
    Enter, Space, Escape, Backspace, Up, Down, Left, Right, W, S, A, D, Q, E, X, Y
};

// A window down/up pair may finish between two guest controller polls. Retain one
// pending down edge per mapped key until a real state read, alongside (never in
// place of) the current physical held state. Repeats coalesce instead of becoming
// a backlog of actions. The owner serializes window events and polling.
class KeyboardEdgeLatch {
public:
    void Reset() { pending_ = down_ = 0; context_valid_ = false; }

    // A complete F1 open/close can happen while the game is loading and does not poll.
    // Notify both transitions explicitly; comparing only the final bool cannot observe that boundary.
    // Held physical keys remain authoritative and down/repeat tracking is retained.
    void MenuChanged(bool menu_open) {
        pending_ = 0;
        context_valid_ = true;
        menu_open_ = menu_open;
    }

    void Event(KeyboardKey key, bool pressed, bool repeat, bool foreground, bool menu_open) {
        if (!foreground) { Reset(); return; }
        Context(menu_open);
        const uint32_t index = uint32_t(key);
        if (index >= kMembers.size()) return;
        const uint32_t bit = 1u << index;
        if (!pressed) { down_ &= ~bit; return; }
        // ImGui receives its own window events. Never enqueue a host-widget tap
        // for the game, even if F1 closes between a controller sample and return.
        if (!menu_open && !repeat && !(down_ & bit)) pending_ |= bit;
        down_ |= bit;
    }

    KeyboardState Sample(KeyboardState held, bool foreground, bool menu_open, bool consume_edges = true) {
        if (!foreground) { Reset(); return {}; }
        Context(menu_open);
        // Connection discovery still reads physical keys/controllers, but must
        // not consume a tap before XInputGetState actually delivers it.
        if (consume_edges) {
            for (uint32_t i = 0; i < kMembers.size(); ++i)
                if (pending_ & (1u << i)) held.*kMembers[i] = true;
            pending_ = 0;
        }
        return held;
    }

private:
    static constexpr std::array<bool KeyboardState::*, 16> kMembers{
        &KeyboardState::enter, &KeyboardState::space, &KeyboardState::escape, &KeyboardState::backspace,
        &KeyboardState::up, &KeyboardState::down, &KeyboardState::left, &KeyboardState::right,
        &KeyboardState::w, &KeyboardState::s, &KeyboardState::a, &KeyboardState::d,
        &KeyboardState::q, &KeyboardState::e, &KeyboardState::x, &KeyboardState::y
    };
    uint32_t pending_ = 0, down_ = 0;
    bool context_valid_ = false, menu_open_ = false;

    void Context(bool menu_open) {
        if (!context_valid_ || menu_open_ != menu_open) pending_ = 0;
        context_valid_ = true;
        menu_open_ = menu_open;
    }
};

}  // namespace input
