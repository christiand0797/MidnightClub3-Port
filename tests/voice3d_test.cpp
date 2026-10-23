#include "audio/voice3d.h"
#include <cassert>
#include <cmath>

int main() {
    audio::Voice3DData data;
    uint32_t update[14]{};
    // XACT changes cone gain and Doppler independently. Cone gain is never a pitch value.
    update[0] = 2;
    update[2] = uint32_t(-600);
    data.Set(update);
    assert(data.Gain() == 1.0f && data.PitchRatio() == 1.0);
    data.enabled = true;
    assert(std::abs(data.Gain() - std::pow(10.0f, -0.3f)) < 1e-6f);
    assert(data.PitchRatio() == 1.0);
    update[0] = 0x20;
    update[2] = 0;  // not selected: preserve cone gain
    update[9] = 4096;
    data.Set(update);
    assert(data.ConeVolume() == -600 && data.PitchRatio() == 2.0);
    update[9] = uint32_t(-8192);
    data.Set(update);
    assert(std::abs(data.PitchRatio(16384) - std::exp2(8191.0 / 4096.0)) < 1e-9);
    assert(data.PitchRatio(4096) == 0.5);
    update[0] = 1;
    update[1] = uint32_t(-10000);
    data.Set(update);
    assert(data.Gain() == 0.0f);
    data.enabled = false;
    assert(data.Gain() == 1.0f && data.PitchRatio() == 1.0);
    audio::Voice3DData spatial;
    spatial.enabled = true;
    spatial.words[1] = uint32_t(-100);
    spatial.words[2] = uint32_t(-200);
    spatial.words[3] = uint32_t(-300);
    spatial.words[4] = uint32_t(-400);
    spatial.words[5] = uint32_t(-500);
    spatial.words[6] = uint32_t(-600);
    spatial.words[7] = uint32_t(-700);
    spatial.words[8] = uint32_t(-800);
    assert(spatial.Attenuation(0) == -300);
    assert(spatial.Attenuation(2) == -1900);
    assert(spatial.Attenuation(6) == -1800);
    assert(spatial.Attenuation(8) == -1400);
    assert(spatial.Attenuation(10) == -1100);
    // Every update bit touches exactly its documented dword group.
    constexpr unsigned first[] = {1, 2, 3, 5, 7, 9, 10, 12};
    constexpr unsigned count[] = {1, 1, 2, 2, 2, 1, 2, 2};
    for (unsigned group = 0; group < 8; ++group) {
        audio::Voice3DData selected;
        update[0] = 1u << group;
        for (unsigned i = 1; i < 14; ++i) update[i] = 0x1000 + i;
        selected.Set(update);
        for (unsigned i = 1; i < 14; ++i)
            assert(selected.words[i] == ((i >= first[group] && i < first[group] + count[group]) ? update[i] : 0));
    }
}
