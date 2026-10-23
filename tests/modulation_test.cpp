#include "audio/modulation.h"
#include <cassert>
#include <cmath>

static bool Near(double a, double b) { return std::abs(a - b) < 1e-5; }

int main() {
    audio::Modulation v;
    v.On();
    for (int i = 0; i < 100; ++i) {
        assert(v.Step());
        assert(v.gain == 1 && v.pitch == 0);
    }

    // Retaining a descriptor depth does not activate a disabled oscillator.
    v.multi_lfo.amplitude = 100;
    v.multi_lfo.pitch = -50;
    v.On();
    assert(v.Step() && v.gain == 1 && v.pitch == 0);
    v.multi_lfo.enabled = true;
    v.multi_lfo.delay = 1;
    v.multi_lfo.delta = 512;
    v.On();
    for (unsigned i = 0; i < audio::kUnitFrames; ++i)
        assert(v.Step() && v.gain == 1 && v.pitch == 0);
    assert(v.Step() && v.gain < 1 && v.gain >= 0 && v.pitch < 0);
    v.multi_lfo.enabled = false;
    assert(v.Step() && v.gain == 1 && v.pitch == 0);

    // EF changes pitch without changing amplitude. The descriptor's generator roles stay independent.
    v = {};
    v.multi.mode = audio::kEgSustain;
    v.multi.sustain = 128;
    v.pitch_scale = 12;
    v.On();
    assert(v.Step() && v.gain == 1);
    assert(Near(v.pitch, 32.0 * 12 * (128.0f / 255)));

    // Run a complete amplitude envelope under the documented host curve model.
    v = {};
    v.amp.mode = audio::kEgDelay;
    v.amp.delay = v.amp.attack = v.amp.hold = v.amp.decay = 1;
    v.amp.sustain = 64;
    v.amp.release = 2;
    v.On();
    assert(v.Step() && v.gain == 0);
    bool saw_attack = false, saw_hold = false;
    for (int i = 0; i < 1000 && v.amp.phase != audio::kEgSustain; ++i) {
        assert(v.Step());
        saw_attack |= v.amp.phase == audio::kEgAttack && v.gain > 0 && v.gain < 1;
        saw_hold |= v.amp.phase == audio::kEgHold && v.gain == 1;
        assert(std::isfinite(v.gain) && v.gain >= 0 && v.gain <= 1 && v.pitch == 0);
    }
    assert(saw_attack && saw_hold && v.amp.phase == audio::kEgSustain);
    assert(Near(v.gain, 64.0f / 255));
    v.Release();
    float previous = v.gain;
    for (unsigned i = 0; i < 2 * audio::kUnitFrames; ++i) {
        assert(v.Step());
        assert(v.gain <= previous && v.gain >= 0);
        previous = v.gain;
    }
    assert(!v.Step() && v.gain == 0);

    // A voice without an envelope preserves immediate stop semantics, and a later voice-on restarts cleanly.
    v = {};
    v.On();
    v.Step();
    v.Release();
    assert(v.Instant() && !v.Step());
    v.On();
    assert(v.Step() && v.gain == 1);
    v.amp.mode = 15;
    v.On();
    assert(v.Step() && v.gain == 1);
}
