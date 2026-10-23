#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audio {

// MCP-X voice envelopes and LFOs as the XDK 5849 library programs them (CMcpxVoiceClient::SetEG 0x3C6F1B and
// SetLFO 0x3C6DEC). The hardware steps them once a 32 sample frame of its 48 kHz output; their times count units of
// 16 frames, 512 samples. The curves follow xemu's voice processor (hw/xbox/mcpx/apu/vp/vp.c).
constexpr uint32_t kFrameSamples = 32, kUnitFrames = 16;

// DSEG_MODE_*, which are also the hardware's phase numbers
enum EnvelopePhase : uint8_t { kEgOff, kEgDelay, kEgAttack, kEgHold, kEgDecay, kEgSustain, kEgRelease, kEgForceRelease };

// one generator of a DSENVELOPEDESC, with the field widths the library packs into the voice registers
struct Envelope {
    uint8_t mode = kEgOff;  // phase entered at voice on
    uint16_t delay = 0, attack = 0, hold = 0, decay = 0, release = 0;  // 12 bits
    uint8_t sustain = 0;
    uint8_t phase = kEgOff;
    uint32_t count = 0;  // frames left in delay, hold and release, frames into attack and decay
    float level = 255;   // the voice's level register, release falls from where it was
    float value = 1;     // the last frame's output, 0..1

    // VOICE_ON: the descriptor's mode is the first phase; the library masks it to the register's 4 bits
    void On() {
        phase = mode <= kEgForceRelease ? mode : uint8_t(kEgOff);
        level = phase == kEgDelay || phase == kEgAttack ? 0.0f : 255.0f;
        count = phase == kEgDelay ? delay * kUnitFrames
                : phase == kEgHold ? hold * kUnitFrames
                : phase == kEgRelease ? release * kUnitFrames
                                     : 0;
    }
    // VOICE_RELEASE, from whatever phase it is in; one already releasing carries on
    void Release() {
        if (phase >= kEgRelease) return;
        phase = kEgRelease;
        count = release * kUnitFrames;
    }
    float Step() {
        switch (phase) {
        case kEgOff:
            level = 255;
            return value = 1;
        case kEgDelay:
            level = 0;
            if (count) --count;
            else phase = kEgAttack;
            return value = 0;
        case kEgAttack: {
            // linear from silence to full
            uint32_t frames = attack * kUnitFrames;
            level = frames && count < frames ? count * 255.0f / frames : 255.0f;
            if (count >= frames) {
                phase = kEgHold;
                count = hold * kUnitFrames;
            } else {
                ++count;
            }
            return value = level / 255;
        }
        case kEgHold:
            level = 255;
            if (count) {
                --count;
            } else {
                phase = kEgDecay;
                count = 0;
            }
            return value = 1;
        case kEgDecay: {
            // about 64 dB down over the decay time, it stops at the sustain level
            float x = decay ? 255.0f * std::pow(0.99988799f, float(count) * 4096.0f / decay) : 0.0f;
            if (x <= sustain + 0.2f) {
                phase = kEgSustain;
                level = sustain;
            } else {
                ++count;
                level = x;
            }
            return value = level / 255;
        }
        case kEgSustain:
            level = sustain;
            return value = level / 255;
        case kEgRelease: {
            uint32_t frames = release * kUnitFrames;
            if (!frames || !count) {
                phase = kEgForceRelease;
                return value = 0;
            }
            // the level held at release, 60 dB down over the release time
            float at = std::clamp(1.0f - float(count) / frames, 0.0f, 1.0f);
            --count;
            return value = std::exp(-6.91f * at) * level / 255;
        }
        default:
            return value = 0;
        }
    }
};

// one DSLFODESC oscillator: a 16 bit phase that moves dwDelta each frame once dwDelay units from voice on are over
struct Lfo {
    bool enabled = false;          // the library sets the enable bit for a nonzero dwDelay
    uint16_t delay = 0, delta = 0;  // 15 and 10 bits
    int8_t pitch = 0, cutoff = 0, amplitude = 0;
    uint32_t count = 0;
    uint16_t phase = 0;
    float value = 0;  // -1..1

    void On() {
        count = delay * kUnitFrames;
        phase = 0;
        value = 0;
    }
    float Step() {
        if (count) {
            --count;
            return value = 0;
        }
        if (!enabled) return value = 0;
        phase = uint16_t(phase + delta);
        // triangle, up to 1 at a quarter, down to -1 at three quarters, back to 0
        float x = phase / 16384.0f;
        return value = x < 1 ? x : x < 3 ? 2 - x : x - 4;
    }
};

// everything that moves a voice's level and pitch between the title's own calls
struct Modulation {
    Envelope multi, amp;                // generator 0 (DSEG_MULTI: pitch, filter), generator 1 (DSEG_AMPLITUDE)
    int8_t pitch_scale = 0, cutoff_scale = 0;  // generator 0's
    Lfo multi_lfo, pitch_lfo;           // DSLFO_MULTI (pitch, filter, amplitude), DSLFO_PITCH
    uint32_t sample = 0;                // output samples into the current frame
    float gain = 1;
    double pitch = 0;  // 4096 an octave, added to the voice's pitch

    void On() {
        multi.On();
        amp.On();
        multi_lfo.On();
        pitch_lfo.On();
        sample = 0;
    }
    void Release() {
        multi.Release();
        amp.Release();
    }
    bool Releasing() const { return amp.phase >= kEgRelease; }
    // a release that turns the voice off within the frame, as the library's zero release time does
    bool Instant() const { return !amp.release || amp.phase == kEgForceRelease; }

    // the next frame; false once the amplitude envelope has run out and the hardware turns the voice off.
    // Cutoff scales are kept with their descriptors: the voice filter (SetFilter) is not run on the host.
    bool Step() {
        // A disabled oscillator or one still in its delay must not attenuate a voice merely because its
        // descriptor retains an amplitude depth. Zero oscillator output is the triangle's midpoint, not bypass.
        const bool amplitude_lfo_active = multi_lfo.enabled && multi_lfo.count == 0;
        float ef = multi.Step(), ea = amp.Step();
        float a = multi_lfo.Step(), p = pitch_lfo.Step();
        // pitch scales and modulation are 32 pitch units a step, so 127 is about an octave
        pitch = 32.0 * (double(pitch_scale) * ef + double(multi_lfo.pitch) * a + double(pitch_lfo.pitch) * p);
        // amplitude modulation only attenuates: full at the LFO's peak (its trough for a negative depth), down by
        // the depth at the other extreme
        float depth = multi_lfo.amplitude / 128.0f;
        float swing = depth < 0 ? -a : a;
        gain = ea * (amplitude_lfo_active ? 1.0f - std::fabs(depth) * (1.0f - swing) * 0.5f : 1.0f);
        return amp.phase != kEgForceRelease;
    }
};

}  // namespace audio
