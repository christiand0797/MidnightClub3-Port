#pragma once
#include <cstdint>

struct X86Context;

namespace mc3::world {
// Population controls affect ordinary free-roam spawners. Scripted modes and the
// original pools/region caps retain authority; a multiplier is not an actor count.
inline constexpr float kMaxMultiplier = 4.0f;
struct Snapshot {
    bool free_roam_player = false;
    bool pedestrians_ready = false;
    uint32_t traffic_roads_ready = 0;
    float traffic_multiplier = 1.0f;
    float pedestrian_multiplier = 1.0f;
};
Snapshot GetSnapshot();
void Apply(X86Context& ctx, uint8_t* base);
}  // namespace mc3::world
