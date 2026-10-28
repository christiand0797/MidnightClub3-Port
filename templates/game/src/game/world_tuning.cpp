// Original USA XBE population endpoints, independent of career serialization:
// 0x1CB290 sends the level's ambient density to each road's 0x2B9670 and
// pedestrian density to 0x222900 -> 0x222770. Ambient road+0x20 is density;
// 0x2B93C0 recomputes road+0x28 from authored path lengths. Pedestrian
// 0x222770 multiplies each region's authored +0x24, preserving its signed-byte
// cap and the original [0,127] target clamp. We call these original writers.
// The mission-name table 0x4B37D0 identifies mode 0 as "roam". Other modes,
// including controlled ambients in missions, are deliberately left unchanged.
#include "world_tuning.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_002B9670);
X86_EXTERN_FUNC(__imp__sub_00222770);
X86_EXTERN_FUNC(__imp__sub_002C89F0);

namespace {
using kernel::R32;
using kernel::W32;
constexpr uint32_t kMission = 0x53F2E0, kTraffic = 0x5587E0, kPedestrian = 0x53891C;
constexpr uint32_t kRoadStride = 0x34, kMaxRoads = 4096, kMaxRegions = 4096;
constexpr uint32_t kHumanControllerVtable = 0x47684C;

bool Committed(uint32_t va, uint32_t size) {
    const uint64_t end = uint64_t(va) + size;
    if (!size || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
float F32(uint32_t va) { const uint32_t bits = R32(va); float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t Bits(float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; }
float Multiplier(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0f, mc3::world::kMaxMultiplier) : 1.0f;
}
bool Density(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
float Scaled(float value, float multiplier) {
    // Native source values are normalized [0,1]. The final writers/consumers
    // support larger finite targets: pedestrians cap each region at127/the
    // authored cap; traffic tests a float target before requesting a road index,
    // and its separate original pool allocator can refuse an empty pool. Keep
    // all those caps/pools intact, bound expansion to4 and preserve authored zero.
    return std::clamp(value * multiplier, 0.0f, mc3::world::kMaxMultiplier);
}
app::Settings::World Settings() { std::lock_guard ui(app::UiLock()); return app::Get().world; }
struct Players { uint32_t controller, car; };
struct TrafficIdentity { uint32_t manager = 0, roads = 0, count = 0, paths = 0; };
struct Road { float original = 0, written = 0; uint32_t nodes = 0; uint8_t count = 0; };
struct Pedestrians {
    uint32_t manager = 0, types = 0, regions = 0, count = 0;
    float original = 0, multiplier = 1;
    bool observed = false;
};
std::mutex g_lock;
std::vector<Players> g_players;
TrafficIdentity g_traffic;
std::unordered_map<uint32_t, Road> g_roads;
Pedestrians g_pedestrians;
mc3::world::Snapshot g_snapshot;

bool HumanPlayer() {
    for (const auto& player : g_players) {
        const uint32_t c = player.controller, car = player.car;
        if (Committed(c, 0x54) && Committed(car, 0x60) && R32(c) == kHumanControllerVtable &&
            R32(c + 0x50) == car && R32(car + 4) == c && kernel::G<uint8_t>(car + 0x5E) == 0)
            return true;
    }
    return false;
}
bool Active() {
    if (!Committed(kMission, 4)) return false;
    const uint32_t mission = R32(kMission);
    return Committed(mission, 0x1C) && R32(mission + 0x18) == 0 && HumanPlayer();
}
TrafficIdentity Traffic() {
    if (!Committed(kTraffic, 4)) return {};
    const uint32_t manager = R32(kTraffic);
    if (!Committed(manager, 0x40)) return {};
    const uint32_t count = kernel::G<uint16_t>(manager + 0x28), roads = R32(manager + 0x2C);
    if (!count || count > kMaxRoads || !Committed(roads, count * kRoadStride)) return {};
    return {manager, roads, count, R32(manager + 0x3C)};
}
bool Same(const TrafficIdentity& a, const TrafficIdentity& b) {
    return a.manager == b.manager && a.roads == b.roads && a.count == b.count && a.paths == b.paths;
}
bool RoadReadable(const TrafficIdentity& traffic, uint32_t road) {
    if (!traffic.manager || road < traffic.roads || uint64_t(road) >= uint64_t(traffic.roads) + traffic.count * kRoadStride ||
        (road - traffic.roads) % kRoadStride) return false;
    const uint32_t nodes = R32(road + 0x2C), count = kernel::G<uint8_t>(road + 4);
    if (count && !Committed(nodes, count * 2)) return false;
    // Validate every path-length read made by the original 0x2B93C0.
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t path = uint64_t(traffic.paths) + uint32_t(kernel::G<uint16_t>(nodes + i * 2)) * 20;
        if (path + 10 > (uint64_t(1) << 32) || !Committed(uint32_t(path), 10)) return false;
    }
    return true;
}
Pedestrians PedestrianIdentity() {
    if (!Committed(kPedestrian, 4)) return {};
    const uint32_t wrapper = R32(kPedestrian);
    if (!Committed(wrapper, 4)) return {};
    const uint32_t manager = R32(wrapper);
    if (!Committed(manager, 0x24)) return {};
    const uint32_t count = kernel::G<uint16_t>(manager + 0x20);
    const uint32_t types = R32(manager + 0x0C), regions = R32(manager + 0x1C);
    if (!count || count > kMaxRegions || !Committed(types, count * 0x28) || !Committed(regions, count * 3)) return {};
    return {manager, types, regions, count};
}
bool Same(const Pedestrians& a, const Pedestrians& b) {
    return a.manager == b.manager && a.types == b.types && a.regions == b.regions && a.count == b.count;
}
void UpdateIdentities() {
    const auto traffic = Traffic();
    if (!Same(traffic, g_traffic)) { g_roads.clear(); g_traffic = traffic; }
    const auto pedestrians = PedestrianIdentity();
    if (!Same(pedestrians, g_pedestrians)) g_pedestrians = pedestrians;
}

// Original setters need at most 28 bytes below their entry stack. Borrow and
// restore 64 committed bytes from the current guest stack, and use a copied
// CPU context: neither caller registers nor stack bytes are changed by a live edit.
bool Replay(X86Context& ctx, uint8_t* base, X86Func* setter, uint32_t object, float value) {
    if (ctx.esp < 128 || !Committed(ctx.esp - 128, 64)) return false;
    const uint32_t scratch = ctx.esp - 128;
    std::array<uint8_t, 64> saved;
    std::memcpy(saved.data(), base + scratch, saved.size());
    X86Context call = ctx;
    call.ecx = object; call.esp = scratch + 48;
    W32(call.esp, 0); W32(call.esp + 4, Bits(value));
    setter(call, base);
    std::memcpy(base + scratch, saved.data(), saved.size());
    return true;
}
}  // namespace

namespace mc3::world {
Snapshot GetSnapshot() { std::lock_guard lock(g_lock); return g_snapshot; }
void Apply(X86Context& ctx, uint8_t* base) {
    const auto settings = Settings();
    std::lock_guard lock(g_lock);
    UpdateIdentities();
    const bool active = Active();
    const float traffic_multiplier = active ? Multiplier(settings.traffic_multiplier) : 1;
    const float pedestrian_multiplier = active ? Multiplier(settings.pedestrian_multiplier) : 1;
    for (auto it = g_roads.begin(); it != g_roads.end();) {
        const uint32_t road = it->first; auto& tracked = it->second;
        if (!RoadReadable(g_traffic, road) || R32(road + 0x2C) != tracked.nodes ||
            kernel::G<uint8_t>(road + 4) != tracked.count || Bits(F32(road + 0x20)) != Bits(tracked.written)) {
            // A native write or changed topology wins; wait for an actual setter
            // observation, rather than invent a new baseline from recycled memory.
            it = g_roads.erase(it); continue;
        }
        const float wanted = Scaled(tracked.original, traffic_multiplier);
        if (Bits(wanted) != Bits(tracked.written) && Replay(ctx, base, __imp__sub_002B9670, road, wanted))
            tracked.written = wanted;
        ++it;
    }
    if (g_pedestrians.observed && pedestrian_multiplier != g_pedestrians.multiplier &&
        Replay(ctx, base, __imp__sub_00222770, g_pedestrians.manager, Scaled(g_pedestrians.original, pedestrian_multiplier)))
        g_pedestrians.multiplier = pedestrian_multiplier;
    g_snapshot = {active, g_pedestrians.observed, uint32_t(g_roads.size()), traffic_multiplier, pedestrian_multiplier};
}
}  // namespace mc3::world

X86_FUNC(sub_002B9670) {
    const auto settings = Settings();
    std::lock_guard lock(g_lock);
    UpdateIdentities();
    const uint32_t road = ctx.ecx, argument = ctx.esp + 4;
    if (!Committed(ctx.esp, 8) || !RoadReadable(g_traffic, road) || !Density(F32(argument))) {
        __imp__sub_002B9670(ctx, base); return;
    }
    const uint32_t original_bits = R32(argument);
    const float original = F32(argument), multiplier = Active() ? Multiplier(settings.traffic_multiplier) : 1;
    const float wanted = Scaled(original, multiplier);
    g_roads[road] = {original, wanted, R32(road + 0x2C), kernel::G<uint8_t>(road + 4)};
    W32(argument, Bits(wanted));
    __imp__sub_002B9670(ctx, base);
    W32(argument, original_bits);
}
X86_FUNC(sub_00222770) {
    const auto settings = Settings();
    std::lock_guard lock(g_lock);
    UpdateIdentities();
    const uint32_t argument = ctx.esp + 4;
    if (!Committed(ctx.esp, 8) || !g_pedestrians.manager || ctx.ecx != g_pedestrians.manager || !Density(F32(argument))) {
        __imp__sub_00222770(ctx, base); return;
    }
    const uint32_t original_bits = R32(argument);
    const float multiplier = Active() ? Multiplier(settings.pedestrian_multiplier) : 1;
    g_pedestrians.original = F32(argument); g_pedestrians.multiplier = multiplier; g_pedestrians.observed = true;
    W32(argument, Bits(Scaled(g_pedestrians.original, multiplier)));
    __imp__sub_00222770(ctx, base);
    W32(argument, original_bits);
}
// The original writes controller+0x50=car at0x2C8A0F; car+4 is installed by
// its caller0x2CB385. Verify both directions later, after construction completes.
X86_FUNC(sub_002C89F0) {
    const uint32_t controller = ctx.ecx;
    const uint32_t car = Committed(ctx.esp, 8) ? R32(ctx.esp + 4) : 0;
    __imp__sub_002C89F0(ctx, base);
    if (!Committed(controller, 0x54) || !Committed(car, 0x60) ||
        R32(controller) != kHumanControllerVtable || R32(controller + 0x50) != car) return;
    std::lock_guard lock(g_lock);
    g_players.erase(std::remove_if(g_players.begin(), g_players.end(), [controller, car](const Players& p) {
        return p.controller == controller || p.car == car;
    }), g_players.end());
    if (g_players.size() == 8) g_players.erase(g_players.begin());
    g_players.push_back({controller, car});
}
