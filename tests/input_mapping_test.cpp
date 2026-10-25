#include "core/input_mapping.h"

#include <algorithm>
#include <cassert>

namespace {
bool SamePad(const input::Pad& a, const input::Pad& b) {
    return a.connected == b.connected && a.buttons == b.buttons && std::equal(a.analog, a.analog + 8, b.analog) &&
           a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

void GarageNavigationChecks() {
    using input::GarageNavigation;
    input::Pad held;
    held.connected = true; held.buttons = 8 | 16 | 32 | 64 | 128;
    constexpr uint8_t analog[] = {255, 201, 178, 255, 250, 1, 134, 255};
    std::copy(analog, analog + 8, held.analog);
    held.lx = 32767; held.ly = 11000; held.rx = -12345; held.ry = 23456;
    GarageNavigation nav;
    auto read = [&](uint64_t ms) { return nav.Apply(held, true, true, ms); };
    const auto raw = held;
    auto first = read(0);
    assert(first.buttons == held.buttons && first.lx == 0 && first.ly == 0);
    assert(std::equal(first.analog, first.analog + 8, held.analog) && first.rx == held.rx && first.ry == held.ry);
    assert(SamePad(held, raw)); // the raw merged pad supplied to ImGui is not mutated
    assert((read(34).buttons & 15) == 8);
    assert((read(35).buttons & 15) == 0);
    assert((read(299).buttons & 15) == 0);
    assert((read(300).buttons & 15) == 8);
    assert((read(334).buttons & 15) == 8);
    assert((read(335).buttons & 15) == 0);
    assert((read(449).buttons & 15) == 0);
    assert((read(450).buttons & 15) == 8);
    // Focus loss, leaving the garage and disconnection all reset cadence and return the original pad.
    assert(SamePad(nav.Apply(held, true, false, 451), held));
    assert((read(452).buttons & 15) == 8);
    assert(SamePad(nav.Apply(held, false, true, 453), held));
    assert((read(454).buttons & 15) == 8);
    held.connected = false;
    assert(SamePad(nav.Apply(held, true, true, 455), held));
    held.connected = true;
    assert((read(456).buttons & 15) == 8);
    nav.Reset(); // the runtime also calls Reset on controller add/remove
    assert((read(457).buttons & 15) == 8);

    // Reversals and the other axis respond immediately; opposed digital directions cancel only their axis.
    nav.Reset(); held.buttons = 8; held.lx = held.ly = 0;
    assert((read(0).buttons & 15) == 8);
    held.buttons = 4; assert((read(10).buttons & 15) == 4);
    held.buttons = 4 | 1; assert((read(20).buttons & 15) == (4 | 1));
    held.buttons = 4 | 8 | 1;
    assert((read(25).buttons & 15) == 1);
    held.buttons = 0; assert((read(26).buttons & 15) == 0);
    held.buttons = 8; assert((read(27).buttons & 15) == 8);

    // Analog enter/release hysteresis prevents a stick hovering at the game's old threshold from chattering.
    nav.Reset(); held.buttons = 0; held.lx = GarageNavigation::kStickEnter - 1;
    assert((read(0).buttons & 15) == 0);
    held.lx = GarageNavigation::kStickEnter;
    assert((read(1).buttons & 15) == 8);
    held.lx = 18000; assert((read(10).buttons & 15) == 8);
    assert((read(36).buttons & 15) == 0);
    held.lx = GarageNavigation::kStickRelease;
    assert((read(301).buttons & 15) == 8);
    held.lx = GarageNavigation::kStickRelease - 1;
    assert((read(302).buttons & 15) == 0);
    held.lx = 18000; assert((read(303).buttons & 15) == 0);
    held.lx = -GarageNavigation::kStickEnter;
    assert((read(304).buttons & 15) == 4);
    held.lx = -GarageNavigation::kStickRelease;
    assert((read(305).buttons & 15) == 4);
    held.lx = -GarageNavigation::kStickRelease + 1;
    assert((read(306).buttons & 15) == 0);
    held.lx = 0; held.ly = -32768;
    assert((read(307).buttons & 15) == 2);
    held.ly = 32767; assert((read(308).buttons & 15) == 1);

    // Digital intent takes precedence over a conflicting analog axis; releasing it restores that stick intent.
    held.buttons = 2; assert((read(309).buttons & 15) == 2);
    held.buttons = 0; assert((read(310).buttons & 15) == 1);

    // No queued catch-up repeats after a slow frame, and a clock rollback starts a fresh immediate press.
    nav.Reset(); held.buttons = 8; held.lx = held.ly = 0;
    assert((read(0).buttons & 15) == 8);
    assert((read(900).buttons & 15) == 8);
    assert((read(934).buttons & 15) == 8);
    assert((read(935).buttons & 15) == 0);
    assert((read(1049).buttons & 15) == 0);
    assert((read(1050).buttons & 15) == 8);
    assert((read(100).buttons & 15) == 8);

    // Different polling cadences all deliver the first press at t=0, wait the initial delay, and cap repeats.
    for (uint64_t step : {1u, 8u, 16u, 33u}) {
        nav.Reset(); bool previous = false; std::vector<uint64_t> edges;
        for (uint64_t ms = 0; ms <= 1000; ms += step) {
            const auto p = read(ms);
            const bool pressed = (p.buttons & 8) != 0;
            if (pressed && !previous) edges.push_back(ms);
            previous = pressed;
            assert(std::equal(p.analog, p.analog + 8, held.analog) && p.rx == held.rx && p.ry == held.ry);
        }
        assert(edges.size() == 6 && edges.front() == 0 && edges[1] >= 300);
        for (size_t i = 2; i < edges.size(); ++i) assert(edges[i] - edges[i - 1] >= 150);
    }

    // The physical/keyboard/script merge is retained, including action buttons and analog triggers.
    input::Pad physical; physical.connected = true; physical.buttons = 1; physical.analog[0] = 255;
    input::KeyboardState keyboard; keyboard.right = true; keyboard.enter = true; keyboard.w = true;
    auto merged = input::MergeKeyboard(physical, input::KeyboardPad(keyboard, true));
    input::Pad script; script.connected = true;
    assert(input::ApplyPadButtons("BACK+LT+LSL", script));
    merged = input::MergeKeyboard(merged, script);
    nav.Reset(); const auto shaped = nav.Apply(merged, true, true, 0);
    assert(shaped.buttons == (1 | 8 | 16 | 32) && shaped.analog[0] == 255);
    assert(shaped.analog[6] == 255 && shaped.analog[7] == 255 && shaped.lx == 0 && shaped.ly == 0);
    assert(merged.lx == -32767); // script remains additive to the other inputs; no source was excluded
    for (uint64_t ms : {1u, 35u, 300u, 450u})
        assert(SamePad(nav.Apply(merged, false, true, ms), merged)); // driving and the PC GUI use raw values
}
} // namespace

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
    GarageNavigationChecks();
}
