#pragma once
#include <cstdint>

// Run after the original CarCfg-to-render refresh, before any mesh resource lookup.
void GarageNormalizeProfileMeshes(uint32_t render, uint32_t cfg);

// Validate a concrete owned car and the actual ordinary tire resource selected for one axle.
// Ratios describe the existing mesh opening and the selected rim/overall-diameter ratio.
bool GarageTireFitForDraw(uint32_t render, uint32_t resources, uint32_t handle, unsigned axle,
                         float& source_ratio, float& target_ratio);
