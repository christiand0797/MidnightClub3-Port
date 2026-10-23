#include "core/input_mapping.h"

#include <cassert>

int main() {
    input::KeyboardState keys;
    auto pad = input::KeyboardPad(keys, true);
    assert(pad.connected && pad.buttons == 0 && pad.lx == 0);
    for (auto value : pad.analog) assert(value == 0);

    keys.enter = keys.space = keys.escape = keys.backspace = true;
    keys.up = keys.left = keys.w = keys.a = true;
    keys.q = keys.e = keys.x = keys.y = true;
    pad = input::KeyboardPad(keys, true);
    assert(pad.buttons == (1 | 4 | 16 | 32));
    assert(pad.analog[0] == 255 && pad.analog[1] == 255);
    for (int i = 2; i < 6; ++i) assert(pad.analog[i] == 255);
    assert(pad.analog[6] == 0 && pad.analog[7] == 255 && pad.lx == -32767);
    assert(pad.ly == 0 && pad.rx == 0 && pad.ry == 0);

    pad = input::KeyboardPad(keys, false);
    assert(pad.connected && pad.buttons == 0 && pad.lx == 0);
    for (auto value : pad.analog) assert(value == 0);
    input::Pad controller;
    controller.connected = true;
    controller.buttons = 16;
    controller.analog[7] = 120;
    controller.lx = 1234;
    auto merged = input::MergeKeyboard(controller, pad);
    assert(merged.buttons == 16 && merged.analog[7] == 120 && merged.lx == 1234);
    input::KeyboardState steering;
    steering.a = steering.space = true;
    merged = input::MergeKeyboard(controller, input::KeyboardPad(steering, true));
    assert(merged.buttons == 16 && merged.analog[0] == 255 && merged.analog[7] == 120 && merged.lx == -32767);

    keys.down = keys.right = keys.d = keys.s = true;
    pad = input::KeyboardPad(keys, true);
    assert(pad.buttons == (16 | 32) && pad.lx == 0);
    assert(pad.analog[6] == 255 && pad.analog[7] == 255);

    input::PadScriptPress first, second;
    using Result = input::PadScriptLine;
    assert(input::ParsePadScriptLine(" # comment", first) == Result::Empty);
    assert(input::ParsePadScriptLine("100 START+A 50 # hold", first) == Result::Valid);
    assert(input::ParsePadScriptLine("\xEF\xBB\xBF" "100 START+A 50", first) == Result::Valid);
    assert(input::ParsePadScriptLine("120 RT+LSL+BLACK+WHITE 100", second) == Result::Valid);
    const std::vector<input::PadScriptPress> script{first, second};
    pad = input::ScriptPad(script, 99);
    assert(pad.connected && pad.buttons == 0 && pad.analog[0] == 0);
    pad = input::ScriptPad(script, 100);
    assert(pad.buttons == 16 && pad.analog[0] == 255);
    pad = input::ScriptPad(script, 120);
    assert(pad.buttons == 16 && pad.analog[0] == 255 && pad.analog[7] == 255 && pad.lx == -32767);
    assert(pad.analog[4] == 255 && pad.analog[5] == 255);
    pad = input::ScriptPad(script, 150);
    assert(pad.buttons == 0 && pad.analog[0] == 0 && pad.analog[7] == 255);
    pad = input::ScriptPad(script, 220);
    assert(pad.connected && pad.buttons == 0 && pad.analog[7] == 0 && pad.lx == 0);

    for (const char* invalid : {"-1 A 10", "0 A 0", "0 A -1", "0 INVALID 10", "0 A+ 10", "0 A++B 10",
                                "0 A 10 extra", "0 A", "9223372036854775807 A 10"})
        assert(input::ParsePadScriptLine(invalid, first) == Result::Invalid);

    input::Pad unchanged;
    unchanged.buttons = 32;
    assert(!input::ApplyPadButtons("A+INVALID", unchanged));
    assert(unchanged.buttons == 32 && unchanged.analog[0] == 0);
    assert(input::ApplyPadButtons("UP+DOWN+LEFT+RIGHT+START+BACK+A+B+X+Y+LT+RT+LS+LSD+LSL+LSR", unchanged));
    assert(unchanged.buttons == 63 && unchanged.analog[0] == 255 && unchanged.analog[7] == 255);
    assert(unchanged.ly == -32767 && unchanged.lx == 32767);
}
