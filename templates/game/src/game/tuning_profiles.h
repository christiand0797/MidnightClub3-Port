#pragma once
#include "app/app.h"
#include <cstdint>
#include <unordered_map>

namespace mc3::car_tuning {
// Capture before taking tuning.cpp's lock. The resulting bank is immutable host data.
struct Bank {
    app::Settings::Tuning global;
    std::unordered_map<uint32_t, app::Settings::Tuning> configs;
    const app::Settings::Tuning& ForCfg(uint32_t cfg) const {
        const auto it = configs.find(cfg);
        return it == configs.end() ? global : it->second;
    }
};
void ObserveFrame();
Bank CaptureBank();
app::Settings::Tuning EffectiveForCfg(uint32_t cfg);
}  // namespace mc3::car_tuning
