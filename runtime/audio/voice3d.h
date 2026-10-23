#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audio {

// XDK DS3DVOICEDATA, recovered from the title's Set3DVoiceData implementation.
// The mask updates groups independently; values absent from an update retain their previous state.
struct Voice3DData {
    std::array<uint32_t, 14> words{};
    bool enabled = false;

    void Set(const uint32_t* update) {
        constexpr uint8_t first[] = {1, 2, 3, 5, 7, 9, 10, 12};
        constexpr uint8_t count[] = {1, 1, 2, 2, 2, 1, 2, 2};
        words[0] = update[0];
        for (unsigned group = 0; group < 8; ++group)
            if (update[0] & (1u << group))
                for (unsigned i = 0; i < count[group]; ++i) words[first[group] + i] = update[first[group] + i];
    }

    int32_t DistanceVolume() const { return static_cast<int32_t>(words[1]); }
    int32_t ConeVolume() const { return static_cast<int32_t>(words[2]); }
    int32_t DopplerPitch() const { return static_cast<int32_t>(words[9]); }
    int32_t Attenuation(uint32_t bin = 0) const {
        if (!enabled) return 0;
        int64_t mb = std::clamp<int64_t>(int64_t(DistanceVolume()) + ConeVolume(), -10000, 0);
        auto value = [&](unsigned i) { return int64_t(int32_t(words[i])); };
        if (bin == 6 || bin == 7) mb += value(7) + value(3) + value(5);
        else if (bin == 8 || bin == 9) mb += value(7) + value(4);
        else if (bin == 2) mb += value(7) + value(3) + value(6);
        else if (bin == 10) mb += value(8);
        return int32_t(std::clamp<int64_t>(mb, -10000, 0));
    }
    float Gain(uint32_t bin = 0) const {
        int32_t mb = Attenuation(bin);
        return mb <= -10000 ? 0.0f : std::pow(10.0f, float(mb) / 2000.0f);
    }
    double PitchRatio(double base_pitch = 0) const {
        // Add Doppler before the hardware's final clamp; opposite pitch shifts may cancel.
        double pitch = base_pitch + (enabled ? DopplerPitch() : 0);
        return std::exp2(std::clamp(pitch, -32767.0, 8191.0) / 4096.0);
    }
};

}  // namespace audio
