// Cars past the garage's steps: ride height per axle and larger wheels and tires (Settings::Tuning, F1 > Vehicle).
//
// A car's tuning files are read by field visitors that register each field's address with the parser:
//  - vehSim (tune/vehicle/<car>_base.vehsim), sub_000A3CA0: its WheelFront block pointer at +0x54, WheelBack at +0x58;
//    each block's visitor sub_000A6DA0 has TravelLimit +0x40, TravelExtent +0x44, SuspensionOffset +0x48,
//    SuspensionLimit +0x4C, SuspensionExtent +0x50 (floats, metres; SuspensionOffset 0 on every stock car).
//  - vehModel (tune/vehicle/<car>.vehmodel), sub_000FB760: SuspensionLimitFront/Rear +0x0C/+0x10,
//    TireDiameterLimitFront/Rear +0x14/+0x18, TireExtentFront/Rear +0x1C/+0x20, SuspensionClearanceFront/Rear
//    +0x24/+0x28, TireSteerLimit +0x2C, GroundClearance +0x30. TireDiameterLimit caps the wheel and tire the garage fits.
// The visitors register field addresses; they run BEFORE the parser loads the values. Pending fields become live
// only when that parser's sub_0032F490 returns.
//
// Every vehSim is a per-car instance (the car component's constructor 0x0FF160 allocates it at car+0x50). Spawning
// and resetting a car runs vehSim::ApplyCarCfg 0x121970, which OVERWRITES each wheel's SuspensionOffset with the
// garage step of the car's CarCfg (CarCfg::GetRideHeight 0x270BE0: table 0x471A90[RideHeight byte]). garage.cpp hooks
// that and calls TuningCarCfgApplied, which takes the garage value as the new original and adds this file's offset at
// once, before the physics setup that follows (0x0FF220) reads it.
//
// Ownership: ride height is changed only on vehSims that garage.cpp registered as a human player's
// (TuningSetPlayerSim: mcPlayerFactory 0x2CBDE0 makes those cars). Traffic and opponents keep their values. The
// wheel and tire size limit is per car model data used by the garage's fit check, so it still applies to every model.
// See docs/notes/garage_tuning.md.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using kernel::R32;
using kernel::W32;

namespace {
constexpr uint32_t kSimWheelFront = 0x54, kSimWheelBack = 0x58;
constexpr uint32_t kWheelTravelLimit = 0x40, kWheelSuspensionOffset = 0x48, kWheelSuspensionExtent = 0x50;
constexpr uint32_t kWheelSuspensionLimit = 0x4C, kWheelTravelExtent = 0x44;
// Lowering past this (the garage's own deepest step, rh_800) widens the wheel's travel by the extra lowering, both
// ways. The wheel suspension 0xA7200 keeps compression in [-TravelExtent, SuspensionLimit] and draws the wheel there:
//  - a slammed body comes down onto its chassis bound, which then holds it on the road (scraping: sparks and sound
//    from the damage handler 0x1027D0); the wheels must extend further than stock to stay on the road, and compress
//    further (up into the arches) when the body is pushed down;
//  - with stock limits the wheel is clamped instead and drawn through the road. (The previous build lowered the bump
//    stop, which clamped compression below the static sag: the wheels were drawn through the garage floor.)
constexpr float kFreeLowering = 0.15f;
constexpr uint32_t kModelTireDiameterFront = 0x14, kModelTireDiameterRear = 0x18, kModelTireExtentFront = 0x1C;

float F32(uint32_t va) {
    float f;
    uint32_t b = R32(va);
    std::memcpy(&f, &b, 4);
    return f;
}
void WF32(uint32_t va, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    W32(va, b);
}

// a float the game loaded and this file changes: the loaded value, what was last written, and neighbours that must
// still hold what they held, so a freed and reused block is let go instead of written
struct Tuned {
    float original, written;
    uint32_t check_a, check_b;  // addresses of the neighbours
    uint32_t value_a, value_b;
    int kind;  // 0 ride front, 1 ride rear, 2 wheel limit, 3/4 SuspensionLimit front/rear, 5/6 TravelExtent front/rear
    uint32_t owner;             // the vehSim of a ride height field, 0 for model data
};
std::mutex g_lock;
std::unordered_map<uint32_t, Tuned> g_tuned;
std::unordered_set<uint32_t> g_player_sims;  // vehSims of human players' cars (garage.cpp)
struct ContactSample { uint32_t owner = 0, offset = 0, stop = 0, extent = 0, calls = 0; };
std::unordered_map<uint32_t, ContactSample> g_contact_samples; // runtime wheel -> bounded diagnostic sampling

struct Pending {
    uint32_t va, check_a, check_b;
    int kind;
    uint32_t owner;
};
std::unordered_map<uint32_t, std::vector<Pending>> g_pending;  // parser -> fields it will load

bool Committed(uint32_t va, uint32_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!bytes || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}

bool Plausible(uint32_t va) { return va >= 0x10000 && va < 0x84000000 && (va & 3) == 0; }

bool Readable(uint32_t va, uint32_t check_a, uint32_t check_b) {
    return Plausible(va) && Plausible(check_a) && Plausible(check_b) && Committed(va, 4) && Committed(check_a, 4) &&
           Committed(check_b, 4);
}
bool Readable(const Pending& p) { return Readable(p.va, p.check_a, p.check_b); }

void Queue(uint32_t parser, Pending p) {
    if (!parser || !Readable(p)) return;
    std::lock_guard<std::mutex> lock(g_lock);
    auto& fields = g_pending[parser];
    for (Pending& field : fields)
        if (field.va == p.va) {
            field = p;
            return;
        }
    fields.push_back(p);
}

// Caller holds g_lock. Record the parsed value with two neighbours as a reuse check. This does not establish
// allocation lifetime; committed-page checks are also required before each later access.
Tuned* Track(const Pending& p) {
    if (!Readable(p)) return nullptr;
    float v = F32(p.va);
    if (!std::isfinite(v)) return nullptr;
    const uint32_t value_a = R32(p.check_a), value_b = R32(p.check_b);
    auto it = g_tuned.find(p.va);
    // loaded again (or saved, which visits the same fields): a value other than the one written is the new original
    if (it != g_tuned.end()) {
        Tuned& t = it->second;
        if (t.written == v && t.kind == p.kind && t.check_a == p.check_a && t.check_b == p.check_b &&
            t.value_a == value_a && t.value_b == value_b) {
            t.owner = p.owner;
            return &t;
        }
    }
    Tuned& t = g_tuned[p.va];
    t = {v, v, p.check_a, p.check_b, value_a, value_b, p.kind, p.owner};
    return &t;
}

void Parsed(uint32_t parser) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_pending.find(parser);
    if (it == g_pending.end()) return;
    for (const Pending& p : it->second) Track(p);
    g_pending.erase(it);
}

// Caller holds g_lock. Ride height only for a player's vehSim; everyone else keeps (or gets back) the original.
float Target(uint32_t va, const Tuned& t, const app::Settings::Tuning& s) {
    switch (t.kind) {
    case 0: return g_player_sims.count(t.owner) ? t.original + s.ride_front : t.original;
    case 1: return g_player_sims.count(t.owner) ? t.original + s.ride_rear : t.original;
    case 3:
    case 4:
    case 5:
    case 6: {
        if (!g_player_sims.count(t.owner)) return t.original;
        // the same wheel's SuspensionOffset, as this file sets it (garage step + F1 offset)
        const int axle = (t.kind - 3) & 1;
        const uint32_t field = t.kind <= 4 ? kWheelSuspensionLimit : kWheelTravelExtent;
        auto offset = g_tuned.find(va - field + kWheelSuspensionOffset);
        if (offset == g_tuned.end() || offset->second.owner != t.owner || offset->second.kind != axle)
            return t.original;
        const float excess = -Target(offset->first, offset->second, s) - kFreeLowering;
        return excess > 0 ? t.original + excess : t.original;
    }
    default: return t.original * s.wheel_limit;
    }
}

// Caller holds g_lock. False when the field must be forgotten (freed, reused or reloaded behind our back).
bool Apply(uint32_t va, Tuned& t, const app::Settings::Tuning& s) {
    if (!Readable(va, t.check_a, t.check_b) || R32(t.check_a) != t.value_a || R32(t.check_b) != t.value_b ||
        F32(va) != t.written)
        return false;
    float want = Target(va, t, s);
    if (std::isfinite(want) && want != t.written) {
        WF32(va, want);
        t.written = want;
    }
    return true;
}

// the two wheel blocks' SuspensionOffset of a vehSim, as the parse path tracks them
bool SimFields(uint32_t sim, Pending out[2]) {
    if (!Plausible(sim) || !Committed(sim + kSimWheelFront, 8)) return false;
    for (int i = 0; i < 2; i++) {
        uint32_t w = R32(sim + (i ? kSimWheelBack : kSimWheelFront));
        out[i] = {w + kWheelSuspensionOffset, w + kWheelTravelLimit, w + kWheelSuspensionExtent, i, sim};
    }
    return true;
}

// the same wheels' SuspensionLimit (out[0..1]) and TravelExtent (out[2..3]), same neighbours as the reuse check
void TravelFields(const Pending offsets[2], Pending out[4]) {
    for (int i = 0; i < 2; i++) {
        out[i] = out[2 + i] = offsets[i];
        out[i].va = offsets[i].va - kWheelSuspensionOffset + kWheelSuspensionLimit;
        out[i].kind = 3 + i;
        out[2 + i].va = offsets[i].va - kWheelSuspensionOffset + kWheelTravelExtent;
        out[2 + i].kind = 5 + i;
    }
}
}  // namespace

// Diagnose the real contact solver at extended-low settings. This calls the original solver unchanged; sampling
// its requested and retained compression distinguishes a travel clamp from body/bound geometry in a playtest.
// At most six samples (two seconds apart) per wheel/setting are emitted, never a per-frame log flood.
X86_EXTERN_FUNC(__imp__sub_000A7250);
X86_FUNC(sub_000A7250) {
    const uint32_t wheel = ctx.ecx;
    const bool readable = Plausible(wheel) && Committed(wheel, 0x168) && Committed(ctx.esp + 4, 12);
    const uint32_t tune = readable ? R32(wheel + 4) : 0;
    const float requested = readable ? F32(ctx.esp + 4) : 0;
    const unsigned contact = readable ? kernel::G<uint8_t>(ctx.esp + 8) : 0;
    __imp__sub_000A7250(ctx, base);
    if (!readable || !Plausible(tune) || !Committed(tune + kWheelTravelExtent, 12)) return;
    uint32_t owner = 0;
    bool sample = false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto offset = g_tuned.find(tune + kWheelSuspensionOffset);
        if (offset == g_tuned.end() || !g_player_sims.count(offset->second.owner) ||
            F32(offset->first) >= -kFreeLowering) return;
        owner = offset->second.owner;
        auto& s = g_contact_samples[wheel];
        const uint32_t value = R32(tune + kWheelSuspensionOffset), stop = R32(tune + kWheelSuspensionLimit),
                       extent = R32(tune + kWheelTravelExtent);
        if (s.owner != owner || s.offset != value || s.stop != stop || s.extent != extent)
            s = {owner, value, stop, extent, 0};
        sample = s.calls < 6 * 120 && s.calls % 120 == 0;
        if (s.calls < 6 * 120) ++s.calls;
    }
    if (sample)
        LOGR("garage: wheel contact sim %08X wheel %08X contact %u requested %.3f actual %.3f offset %.3f stop %.3f "
             "extension %.3f", owner, wheel, contact, requested, F32(wheel + 0x144),
             F32(tune + kWheelSuspensionOffset), F32(tune + kWheelSuspensionLimit), F32(tune + kWheelTravelExtent));
}

// garage.cpp: a human player's car (mcPlayerFactory) owns this vehSim, or no longer does
void TuningSetPlayerSim(uint32_t sim, bool player) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (player)
        g_player_sims.insert(sim);
    else
        g_player_sims.erase(sim);
}

// garage.cpp: the car owning this vehSim is being destroyed; never touch its fields again
void TuningForgetSim(uint32_t sim) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_player_sims.erase(sim);
    std::erase_if(g_contact_samples, [sim](const auto& entry) { return entry.second.owner == sim; });
    for (auto it = g_tuned.begin(); it != g_tuned.end();)
        if (it->second.owner == sim && it->second.kind != 2)
            it = g_tuned.erase(it);
        else
            ++it;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        auto& fields = it->second;
        std::erase_if(fields, [sim](const Pending& p) { return p.owner == sim; });
        if (fields.empty()) it = g_pending.erase(it);
        else ++it;
    }
}

// garage.cpp, after vehSim::ApplyCarCfg 0x121970 wrote the garage ride height into SuspensionOffset: that value is the
// new original, and a player's offset is written now so the physics set up right after sees it
void TuningCarCfgApplied(uint32_t sim) {
    Pending fields[6];
    if (!SimFields(sim, fields)) return;
    TravelFields(fields, fields + 2);
    const app::Settings::Tuning& s = app::Get().tuning;
    std::lock_guard<std::mutex> lock(g_lock);
    // offsets first: the travel limits follow their wheel's offset
    for (const Pending& p : fields) {
        // ApplyCarCfg unconditionally wrote both offsets. Even if a new garage step happens to equal the value
        // last produced by F1, it is a fresh baseline. Track's normal unchanged-value shortcut is only valid on
        // the visitor/parse path; using it here silently loses the offset on some adjacent step transitions.
        if (p.kind < 2) g_tuned.erase(p.va);
        if (Tuned* t = Track(p))
            if (!Apply(p.va, *t, s)) g_tuned.erase(p.va);
    }
}

// vehSim's visitor registers the two wheel blocks under nested parsers. Completing the outer parser also completes
// both nested blocks, so it is the boundary at which their loaded values can be recorded.
X86_EXTERN_FUNC(__imp__sub_000A3CA0);
X86_FUNC(sub_000A3CA0) {
    uint32_t self = ctx.ecx;
    uint32_t parser = Committed(ctx.esp + 4, 4) ? R32(ctx.esp + 4) : 0;
    __imp__sub_000A3CA0(ctx, base);
    Pending fields[2];
    if (!SimFields(self, fields)) return;
    for (const Pending& p : fields)
        if (Plausible(p.va - kWheelSuspensionOffset)) Queue(parser, p);
}

// vehModel's visitor: the wheel and tire size limits
X86_EXTERN_FUNC(__imp__sub_000FB760);
X86_FUNC(sub_000FB760) {
    uint32_t self = ctx.ecx;
    uint32_t parser = Committed(ctx.esp + 4, 4) ? R32(ctx.esp + 4) : 0;
    __imp__sub_000FB760(ctx, base);
    if (!Plausible(self)) return;
    Queue(parser, {self + kModelTireDiameterFront, self + kModelTireExtentFront, self + 0x0C, 2, 0});
    Queue(parser, {self + kModelTireDiameterRear, self + kModelTireExtentFront, self + 0x0C, 2, 0});
}

// The parser's text reader writes the registered fields, including recursively reading nested blocks.
X86_EXTERN_FUNC(__imp__sub_0032F490);
X86_FUNC(sub_0032F490) {
    const uint32_t parser = ctx.ecx;
    __imp__sub_0032F490(ctx, base);
    Parsed(parser);
}

// Some visitors are used to write/save rather than read. Do not retain their unused pending fields after the
// parser's destructor, since its address may be recycled for an unrelated parser.
X86_EXTERN_FUNC(__imp__sub_0032F840);
X86_FUNC(sub_0032F840) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_pending.erase(ctx.ecx);
    }
    __imp__sub_0032F840(ctx, base);
}

// once a frame (the time manager hook): every tracked value set from the current settings
void ApplyVehicleTuning() {
    const app::Settings::Tuning& s = app::Get().tuning;
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto it = g_tuned.begin(); it != g_tuned.end();) {
        if (!Apply(it->first, it->second, s))
            it = g_tuned.erase(it);  // the block was freed or reloaded: the visitor tracks it again if it comes back
        else
            ++it;
    }
}
