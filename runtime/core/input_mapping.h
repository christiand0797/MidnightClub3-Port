#pragma once

#include "core/input.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace input {

// Physical key states, independent of Win32 so the mapping can be checked without polling the desktop.
struct KeyboardState {
    bool enter = false, space = false, escape = false, backspace = false;
    bool up = false, down = false, left = false, right = false;
    bool w = false, s = false, a = false, d = false;
    bool q = false, e = false, x = false, y = false;
};

inline Pad KeyboardPad(const KeyboardState& keys, bool foreground) {
    Pad pad;
    // Losing focus must release every control without pretending the controller was unplugged.
    pad.connected = true;
    if (!foreground) return pad;
    if (keys.up && !keys.down) pad.buttons |= 1;
    if (keys.down && !keys.up) pad.buttons |= 2;
    if (keys.left && !keys.right) pad.buttons |= 4;
    if (keys.right && !keys.left) pad.buttons |= 8;
    if (keys.enter) pad.buttons |= 16;
    if (keys.backspace) pad.buttons |= 32;
    pad.analog[0] = keys.space ? 255 : 0;
    pad.analog[1] = keys.escape ? 255 : 0;
    pad.analog[2] = keys.x ? 255 : 0;
    pad.analog[3] = keys.y ? 255 : 0;
    pad.analog[4] = keys.q ? 255 : 0;
    pad.analog[5] = keys.e ? 255 : 0;
    pad.analog[6] = keys.s ? 255 : 0;
    pad.analog[7] = keys.w ? 255 : 0;
    pad.lx = int16_t((int(keys.d) - int(keys.a)) * 32767);
    return pad;
}

inline Pad MergeKeyboard(Pad controller, const Pad& keyboard) {
    controller.connected = controller.connected || keyboard.connected;
    controller.buttons |= keyboard.buttons;
    for (unsigned i = 0; i < 8; ++i)
        if (keyboard.analog[i] > controller.analog[i]) controller.analog[i] = keyboard.analog[i];
    if (keyboard.lx) controller.lx = keyboard.lx;
    if (keyboard.ly) controller.ly = keyboard.ly;
    return controller;
}

// The harness token convention: LS/LSD/LSL/LSR are left-stick directions.
// BLACK and WHITE also expose the original Xbox's extra analog buttons.
inline bool ApplyPadButtons(std::string_view buttons, Pad& pad) {
    Pad result = pad;
    size_t start = 0;
    while (start < buttons.size()) {
        size_t end = buttons.find('+', start);
        if (end == std::string_view::npos) end = buttons.size();
        std::string_view button = buttons.substr(start, end - start);
        if (button == "UP") result.buttons |= 1;
        else if (button == "DOWN") result.buttons |= 2;
        else if (button == "LEFT") result.buttons |= 4;
        else if (button == "RIGHT") result.buttons |= 8;
        else if (button == "START") result.buttons |= 16;
        else if (button == "BACK") result.buttons |= 32;
        else if (button == "A") result.analog[0] = 255;
        else if (button == "B") result.analog[1] = 255;
        else if (button == "X") result.analog[2] = 255;
        else if (button == "Y") result.analog[3] = 255;
        else if (button == "BLACK") result.analog[4] = 255;
        else if (button == "WHITE") result.analog[5] = 255;
        else if (button == "LT") result.analog[6] = 255;
        else if (button == "RT") result.analog[7] = 255;
        else if (button == "LS") result.ly = 32767;
        else if (button == "LSD") result.ly = -32767;
        else if (button == "LSL") result.lx = -32767;
        else if (button == "LSR") result.lx = 32767;
        else return false;
        if (end == buttons.size()) {
            pad = result;
            return true;
        }
        start = end + 1;
    }
    return false;  // Empty strings and trailing '+' are malformed.
}

struct PadScriptPress {
    int64_t milliseconds = 0;
    std::string buttons;
    int64_t duration_ms = 0;
};

enum class PadScriptLine { Empty, Valid, Invalid };

// Lines are "milliseconds buttons duration_ms"; '#' begins a comment.
inline PadScriptLine ParsePadScriptLine(std::string_view line, PadScriptPress& press) {
    if (line.starts_with("\xEF\xBB\xBF")) line.remove_prefix(3);  // UTF-8 files written by Windows PowerShell.
    line = line.substr(0, line.find('#'));
    std::istringstream stream{std::string(line)};
    stream >> std::ws;
    if (stream.eof()) return PadScriptLine::Empty;
    PadScriptPress result;
    if (!(stream >> result.milliseconds >> result.buttons >> result.duration_ms)) return PadScriptLine::Invalid;
    stream >> std::ws;
    if (!stream.eof() || result.milliseconds < 0 || result.duration_ms <= 0 ||
        result.milliseconds > std::numeric_limits<int64_t>::max() - result.duration_ms)
        return PadScriptLine::Invalid;
    Pad check;
    if (!ApplyPadButtons(result.buttons, check)) return PadScriptLine::Invalid;
    press = result;
    return PadScriptLine::Valid;
}

inline Pad ScriptPad(const std::vector<PadScriptPress>& script, int64_t elapsed_ms) {
    Pad pad;
    pad.connected = true;
    for (const auto& press : script)
        if (elapsed_ms >= press.milliseconds && elapsed_ms - press.milliseconds < press.duration_ms)
            ApplyPadButtons(press.buttons, pad);
    return pad;
}

}  // namespace input
