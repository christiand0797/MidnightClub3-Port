#pragma once
#include <cstdint>

// Run after the original CarCfg-to-render refresh, before any mesh resource lookup.
void GarageNormalizeProfileMeshes(uint32_t render, uint32_t cfg);
