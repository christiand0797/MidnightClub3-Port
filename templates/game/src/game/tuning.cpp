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
#include <array>
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
#include "garage.h"

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

app::Settings::Tuning SettingsSnapshot() {
    // ImGui edits the settings on the render thread. Take one consistent copy before the tuning lock; never
    // hold g_lock while waiting for UiLock, and never let a slider race a physics read of the same float.
    std::lock_guard ui(app::UiLock());
    return app::Get().tuning;
}

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
    int kind;  // 0/1 ride, 2 fit limit, 3/4 bump travel, 5/6 droop travel, 7 horsepower, 8..17 per-axle multipliers
    uint32_t owner;             // the vehSim of a ride height field, 0 for model data
};
std::mutex g_lock;
std::unordered_map<uint32_t, Tuned> g_tuned;
std::unordered_set<uint32_t> g_player_sims;  // vehSims of human players' cars (garage.cpp)
struct CarTuning {
    uint32_t sim = 0, cfg = 0, render = 0, model = 0;
    std::array<float, 2> offset_original{}, offset_written{};
    std::array<float, 15> settings{};
    bool initialized = false;
};
std::unordered_map<uint32_t, CarTuning> g_player_cars;
struct BoundPayload {
    uint32_t bytes, bound, vertices, polygons, vertex_count, polygon_count;
    bool operator==(const BoundPayload&) const = default;
};
std::unordered_map<uint32_t, BoundPayload> g_bound_payloads; // 105A40 relocation generation -> live pointer identity
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
    case 7: return g_player_sims.count(t.owner) ? t.original * s.engine_power : t.original;
    case 8: return g_player_sims.count(t.owner) ? t.original * s.spring_front : t.original;
    case 9: return g_player_sims.count(t.owner) ? t.original * s.spring_rear : t.original;
    case 10: return g_player_sims.count(t.owner) ? t.original * s.damper_front : t.original;
    case 11: return g_player_sims.count(t.owner) ? t.original * s.damper_rear : t.original;
    case 12: return g_player_sims.count(t.owner) ? t.original * s.brake_front : t.original;
    case 13: return g_player_sims.count(t.owner) ? t.original * s.brake_rear : t.original;
    case 14: case 16: return g_player_sims.count(t.owner) ? t.original * s.grip_front : t.original;
    case 15: case 17: return g_player_sims.count(t.owner) ? t.original * s.grip_rear : t.original;
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

// These are the original vehWheel/vehEngine fields, not UI-only values. The same per-instance ownership and
// lifetime checks used for ride height apply. Garage upgrades rebuild them; the setup hook below re-bases them.
std::vector<Pending> ParameterFields(uint32_t sim) {
    Pending offsets[2];
    if (!SimFields(sim, offsets)) return {};
    std::vector<Pending> fields;
    for (int axle = 0; axle < 2; ++axle) {
        const uint32_t wheel = offsets[axle].va - kWheelSuspensionOffset;
        for (const auto [off, kind] : {std::pair{0x58u, 8}, {0x5Cu, 10}, {0x34u, 12}, {0x68u, 14}, {0x6Cu, 16}})
            fields.push_back({wheel + off, offsets[axle].check_a, offsets[axle].check_b, kind + axle, sim});
    }
    if (Committed(sim + 0x5C, 4)) {
        const uint32_t engine = R32(sim + 0x5C);
        if (Plausible(engine)) fields.push_back({engine + 4, engine + 8, engine + 0x0C, 7, sim});
    }
    return fields;
}

std::array<float, 15> SettingsKey(const app::Settings::Tuning& s) {
    return {s.ride_front, s.ride_rear, s.camber_front, s.camber_rear, s.offset_front, s.offset_rear,
            s.spring_front, s.spring_rear, s.damper_front, s.damper_rear, s.brake_front, s.brake_rear,
            s.grip_front, s.grip_rear, s.engine_power};
}

// Caller holds g_lock. A positive offset extends the wheel: the retail renderer subtracts its hub offset along
// the wheel's outward basis at 0xF76E5..0xF76F9, so our additional outward distance has the opposite sign here.
void ApplyRender(uint32_t car, CarTuning& c, const app::Settings::Tuning& s, bool freshly_applied = false) {
    if (!Committed(car + 0x50, 0x1C) || R32(car + 0x50) != c.sim || R32(car + 0x64) != c.cfg ||
        !Committed(c.cfg + 0x0C, 8)) return;
    const uint32_t render = R32(car + 0x68);
    if (!Plausible(render) || !Committed(render + 0x12C8, 0x28) || !Committed(render + 0x1800, 4)) return;
    const uint32_t model = R32(render + 0x1800);
    if (!Plausible(model)) return;
    const bool reset = freshly_applied || c.render != render || c.model != model;
    c.render = render; c.model = model;
    for (int axle = 0; axle < 2; ++axle) {
        const uint32_t off = render + 0x12D8 + 4 * axle;
        const float current = F32(off), camber = F32(c.cfg + 0x0C + 4 * axle);
        if (!std::isfinite(current) || !std::isfinite(camber)) continue;
        if (reset || current != c.offset_written[axle]) c.offset_original[axle] = current;
        const float extra = axle ? s.offset_rear : s.offset_front;
        const float angle = camber + (axle ? s.camber_rear : s.camber_front);
        const float want = c.offset_original[axle] - extra;
        if (std::isfinite(want)) WF32(off, want), c.offset_written[axle] = want;
        if (std::isfinite(angle)) WF32(render + 0x12E8 + 4 * axle, angle);
    }
}

bool ReadBoundPayload(uint32_t root, uint32_t bytes, BoundPayload& out) {
    const uint64_t end = uint64_t(root) + bytes;
    auto contains = [&](uint32_t address, uint64_t size) {
        return size && address >= root && uint64_t(address) + size <= end && size <= UINT32_MAX &&
               Committed(address, uint32_t(size));
    };
    if (!Plausible(root) || end > (uint64_t(1) << 32) || !contains(root, 0x10)) return false;
    const uint32_t bound = R32(root + 0xC);
    if (!Plausible(bound) || !contains(bound, 0x70) || kernel::G<uint8_t>(bound + 4) != 3) return false;
    const uint32_t vertices = R32(bound + 0x68), polygons = R32(bound + 0x6C);
    const uint32_t count = R32(bound + 0x4C), faces = R32(bound + 0x50);
    // Count loads are u32, not masked words. Original polygon indices are u16; reject impossible/corrupt
    // geometry before multiplication or a long committed-range scan.
    if (!count || count > 0x10000 || !faces || !Plausible(vertices) || !Plausible(polygons) ||
        !contains(vertices, uint64_t(count) * 12) || !contains(polygons, uint64_t(faces) * 0x20)) return false;
    auto overlaps = [](uint32_t a, uint64_t as, uint32_t b, uint64_t bs) {
        return uint64_t(a) < uint64_t(b) + bs && uint64_t(b) < uint64_t(a) + as;
    };
    if (overlaps(bound, 0x70, vertices, uint64_t(count) * 12) ||
        overlaps(bound, 0x70, polygons, uint64_t(faces) * 0x20) ||
        overlaps(vertices, uint64_t(count) * 12, polygons, uint64_t(faces) * 0x20)) return false;
    for (uint32_t i = 0; i < faces; ++i)
        for (unsigned corner = 0; corner < 4; ++corner)
            if (kernel::G<uint16_t>(polygons + i * 0x20 + 0x10 + 2 * corner) >= count) return false;
    out = {bytes, bound, vertices, polygons, count, faces};
    return true;
}

// Caller holds g_lock. Human driving construction requests a fresh resource-cache slot (105B80 key 0), while
// garage previews may request shared resources. The cache, not bound+48's pinned 16-bit reference count,
// establishes whether changing this shape can affect another car. Reject any uncertainty and keep retail.
bool PrivateFloorArgument(uint32_t body, float argument, float& corrected) {
    if (!Plausible(body) || !Committed(body, 0x1AC) || R32(body + 0x18) == 2 ||
        !std::isfinite(argument) || argument < 0) return false;
    uint32_t car = 0, sim = 0;
    for (const auto& [candidate, c] : g_player_cars) {
        if (!g_player_sims.count(c.sim) || !Plausible(candidate) || !Committed(candidate + 8, 0x60) ||
            !Plausible(c.cfg) || !Committed(c.cfg, 0x24) ||
            R32(candidate + 8) != body || R32(candidate + 0x50) != c.sim ||
            R32(candidate + 0x64) != c.cfg || R32(body + 0xB4) != c.sim ||
            kernel::G<uint8_t>(candidate + 0x5E) != 0) continue;
        if (car) return false;
        car = candidate; sim = c.sim;
    }
    if (!car || !Plausible(sim) || !Committed(sim, 0xE0)) return false;
    const uint32_t root = R32(car + 0x4C);
    if (!Plausible(root) || !Committed(root, 0x10)) return false;
    const auto captured = g_bound_payloads.find(root);
    BoundPayload live{};
    if (captured == g_bound_payloads.end() || !ReadBoundPayload(root, captured->second.bytes, live) ||
        live != captured->second) return false;
    const uint32_t bound = live.bound;
    // Match the actual A2450 pointer chain, rather than assuming the resource supplied at construction is
    // still installed on this body after a rebind/reset.
    uint32_t link = body;
    for (const uint32_t offset : {4u, 0x1Cu, 4u, 0xCu}) {
        if (!Plausible(link) || !Committed(link + offset, 4)) return false;
        link = R32(link + offset);
    }
    if (link != bound) return false;
    const uint32_t vertices = live.vertices, count = live.vertex_count;
    bool has_floor = false;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t index = R32(body + 0x190 + 4 * i);
        if (index == UINT32_MAX) continue;
        if (index >= count) return false;
        for (unsigned j = 0; j < i; ++j)
            if (R32(body + 0x190 + 4 * j) == index) return false;
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(F32(vertices + 12 * index + 4 * axis))) return false;
        has_floor = true;
    }
    const float minimum = F32(body + 0x1A0), cap = F32(body + 0x1A4);
    if (!has_floor || !std::isfinite(minimum) || !std::isfinite(cap) || cap < minimum ||
        !std::isfinite(F32(body + 0x1A8)) || !std::isfinite(F32(sim + 0xD8)) ||
        !std::isfinite(F32(sim + 0xDC))) return false;
    if (!Committed(0x50A08C, 4)) return false;
    const uint32_t manager = R32(0x50A08C);
    if (!Plausible(manager) || !Committed(manager, 16 * 0x10)) return false;
    bool private_slot = false;
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t entry = manager + 0x10 * i;
        if (R32(entry) == UINT32_MAX) continue;
        const uint8_t refs = kernel::G<uint8_t>(entry + 9), pending = kernel::G<uint8_t>(entry + 0xA);
        if (!refs && !pending) continue;
        const uint32_t other_root = R32(entry + 4);
        if (other_root == root) {
            if (private_slot || kernel::G<uint8_t>(entry + 8) != 0 || refs != 1 || pending) return false;
            private_slot = true;
            continue;
        }
        if (!Plausible(other_root) || !Committed(other_root + 0xC, 4)) return false;
        const uint32_t other_bound = R32(other_root + 0xC);
        if (!Plausible(other_bound) || !Committed(other_bound, 0x70) || other_bound == bound ||
            R32(other_bound + 0x68) == vertices) return false;
        const uint32_t other_vertices = R32(other_bound + 0x68), other_count = R32(other_bound + 0x4C);
        if (other_count && uint64_t(vertices) < uint64_t(other_vertices) + uint64_t(other_count) * 12 &&
            uint64_t(other_vertices) < uint64_t(vertices) + uint64_t(count) * 12) return false;
    }
    if (!private_slot) return false;
    const uint32_t front = R32(body + 0xD8), rear = R32(body + 0xDC);
    if (!Plausible(front) || !Plausible(rear) || !Committed(front + 0x10C, 4) ||
        !Committed(rear + 0x10C, 4)) return false;
    const float mean = (F32(front + 0x10C) + F32(rear + 0x10C)) * .5f;
    if (!std::isfinite(mean) || mean >= 0) return false;
    // 105450 supplied max(0, stockRadius-currentRadius-mean). Adding the negative mean back, then
    // clamping again, removes only the lowering term; no model getter or duplicate radius read is needed.
    corrected = std::max(0.0f, argument + mean);
    return std::isfinite(corrected);
}
}  // namespace

// Common synchronous/asynchronous relocation boundary. The original loader supplies the checked payload
// length; root-80 and root+7C are not reliable length sources. A fresh relocation replaces stale identity
// at a reused address. A cache entry must still independently prove current private ownership below.
X86_EXTERN_FUNC(__imp__sub_00105A40);
X86_FUNC(sub_00105A40) {
    const bool readable = Committed(ctx.esp + 4, 12);
    const uint32_t root = readable ? R32(ctx.esp + 4) : 0;
    const uint32_t bytes = readable ? R32(ctx.esp + 0xC) : 0;
    __imp__sub_00105A40(ctx, base);
    if (!readable) return;
    BoundPayload payload{};
    const bool valid = ReadBoundPayload(root, bytes, payload);
    std::lock_guard<std::mutex> lock(g_lock);
    if (valid) g_bound_payloads[root] = payload;
    else g_bound_payloads.erase(root);
}

// The original refresh raises its four lowest chassis-bound vertices by negative mean ride height. Preserve
// their normal tire-size compensation and native 5 cm skin, so lowering can bring the chassis onto the road.
// Shared previews and anything without a proven private human cache slot retain the complete original path.
X86_EXTERN_FUNC(__imp__sub_00121440);
X86_FUNC(sub_00121440) {
    if (Committed(ctx.esp, 8) && R32(ctx.esp) == 0x10570C) {
        const float original = F32(ctx.esp + 4);
        float corrected = original;
        bool changed = false, report = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            changed = PrivateFloorArgument(ctx.ecx, original, corrected) && corrected != original;
            static unsigned reports = 0;
            report = changed && reports++ < 8;
        }
        if (changed) WF32(ctx.esp + 4, corrected);
        if (report) LOGR("garage: private chassis %08X retains its contact floor (clearance %.3f -> %.3f)",
                         ctx.ecx, original, corrected);
    }
    __imp__sub_00121440(ctx, base);
}

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

// The car renderer enforces a second, visual wheel/arch envelope at 0xF74D5/0xF7525. At extreme lowering it
// pushes a correctly grounded tire DOWN to fit the original wheel arch (0xF7543..0xF7572), through the road.
// These three callers use the displacement query only to calculate that cosmetic correction. Suppress the
// correction for deliberately tucked player wheels; all physical and other visual displacement queries remain
// original. The user's extended tuck permits fender intersection, never a tire translated through the ground.
X86_EXTERN_FUNC(__imp__sub_000A65D0);
X86_FUNC(sub_000A65D0) {
    const uint32_t wheel = ctx.ecx;
    const uint32_t caller = Committed(ctx.esp, 4) ? R32(ctx.esp) : 0;
    __imp__sub_000A65D0(ctx, base);
    const bool normal = caller == 0xF7525 || caller == 0xF7B7D;
    const uint32_t limit_va = ctx.esp + (caller == 0xF7B7D ? 0x18u : 0x58u);
    if ((!normal && caller != 0xF74D5) || !Plausible(wheel) || !Committed(wheel, 0x148) ||
        !Committed(limit_va, 4)) return;
    const uint32_t tune = R32(wheel + 4);
    const float limit = F32(limit_va), radius = F32(wheel + 0x100);
    // The ordinary-tire envelope is a signed model-space height, not a positive radius. Negative values are
    // valid on low arches and still participate in limit-query-radius. Only the symmetric special-tire branch
    // needs a nonnegative half-extent.
    if (!std::isfinite(limit) || !std::isfinite(radius) || radius <= 0 ||
        (caller == 0xF74D5 && limit < 0)) return;
    bool report = false;
    const bool allow_tuck = SettingsSnapshot().wheel_tuck;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto offset = g_tuned.find(tune + kWheelSuspensionOffset);
        if (!allow_tuck || offset == g_tuned.end() || !g_player_sims.count(offset->second.owner) ||
            F32(offset->first) >= -kFreeLowering) return;
        static unsigned reported = 0;
        report = reported++ < 8;
    }
    const double original = ctx.st[ctx.top];
    // Normal tire: correction = min(0, limit - query - radius). A tiny margin avoids a negative rounding ulp.
    // Special tire: correction is zero for a query inside [-limit,+limit]. Neither caller otherwise uses it.
    ctx.st[ctx.top] = normal ? double(limit) - radius - 0.00001 : 0.0;
    if (report) LOGR("garage: tucked wheel %08X keeps contact position (render fit %.3f -> %.3f, radius %.3f, limit %.3f)",
                     wheel, original, ctx.st[ctx.top], radius, limit);
}

// garage.cpp: a human player's car (mcPlayerFactory) owns this vehSim, or no longer does
void TuningSetPlayerSim(uint32_t sim, bool player) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (player)
        g_player_sims.insert(sim);
    else {
        const app::Settings defaults;
        for (auto& [car, c] : g_player_cars)
            if (c.sim == sim) ApplyRender(car, c, defaults.tuning);
        g_player_sims.erase(sim);
    }
}

void TuningBindCar(uint32_t car, uint32_t sim, uint32_t cfg, bool player) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!player) { g_player_cars.erase(car); return; }
    auto& c = g_player_cars[car];
    if (c.sim != sim || c.cfg != cfg) c = CarTuning{};
    c.sim = sim; c.cfg = cfg;
}

// garage.cpp: the car owning this vehSim is being destroyed; never touch its fields again
void TuningForgetSim(uint32_t sim) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_player_sims.erase(sim);
    std::erase_if(g_player_cars, [sim](const auto& entry) { return entry.second.sim == sim; });
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
    const app::Settings::Tuning s = SettingsSnapshot();
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
    for (const Pending& p : ParameterFields(sim))
        if (Tuned* t = Track(p))
            if (!Apply(p.va, *t, s)) g_tuned.erase(p.va);
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
    for (const Pending& p : ParameterFields(self)) Queue(parser, p);
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
    const app::Settings::Tuning s = SettingsSnapshot();
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto it = g_tuned.begin(); it != g_tuned.end();) {
        if (!Apply(it->first, it->second, s))
            it = g_tuned.erase(it);  // the block was freed or reloaded: the visitor tracks it again if it comes back
        else
            ++it;
    }
    for (auto& [car, c] : g_player_cars)
        if (g_player_sims.count(c.sim)) ApplyRender(car, c, s);
}

// The original component's per-frame update already has the complete "CarCfg dirty" refresh path. Request that
// path once when a live PC tuning control changes, so cached wheel offsets, engine and suspension coefficients
// update together. Do not rebuild the car every frame or write its purchased parts.
void GarageRefreshPlayerCar(uint32_t car);  // garage.cpp: recover concrete human ownership after a car rebind
X86_EXTERN_FUNC(__imp__sub_000FFD90);
X86_FUNC(sub_000FFD90) {
    const uint32_t car = ctx.ecx;
    GarageRefreshPlayerCar(car);
    const auto settings = SettingsSnapshot();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_player_cars.find(car);
        if (it != g_player_cars.end() && g_player_sims.count(it->second.sim)) {
            auto& c = it->second;
            const auto key = SettingsKey(settings);
            if ((!c.initialized || key != c.settings) && Committed(car + 0x64, 4) && R32(car + 0x64) == c.cfg &&
                Committed(c.cfg + 8, 4)) {
                W32(c.cfg + 8, R32(c.cfg + 8) | 0x81u);
                c.settings = key; c.initialized = true;
            }
        }
    }
    __imp__sub_000FFD90(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000F32A0);
X86_FUNC(sub_000F32A0) {
    const uint32_t render = ctx.ecx;
    const uint32_t cfg = Committed(ctx.esp + 4, 8) ? R32(ctx.esp + 4) : 0;
    const bool rebuilt_wheels = Committed(ctx.esp + 8, 4) &&
                                ((R32(ctx.esp + 8) & 0xFFu) || (Committed(cfg + 8, 4) && (R32(cfg + 8) & 5u)));
    __imp__sub_000F32A0(ctx, base);
    GarageNormalizeProfileMeshes(render, cfg);
    const auto settings = SettingsSnapshot();
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [car, c] : g_player_cars)
        if (g_player_sims.count(c.sim) && Committed(car + 0x68, 4) && R32(car + 0x68) == render)
            ApplyRender(car, c, settings, rebuilt_wheels);
}

// Purchased performance upgrades interpolate base/mods physics in this routine. Re-record only the fields that
// interpolation just replaced, then apply the PC multipliers once; ride-height baselines remain garage-owned.
X86_EXTERN_FUNC(__imp__sub_00103C50);
X86_FUNC(sub_00103C50) {
    const uint32_t car = Committed(ctx.esp + 4, 4) ? R32(ctx.esp + 4) : 0;
    __imp__sub_00103C50(ctx, base);
    if (!Committed(car + 0x50, 4)) return;
    const uint32_t sim = R32(car + 0x50);
    const auto settings = SettingsSnapshot();
    std::lock_guard<std::mutex> lock(g_lock);
    for (const Pending& p : ParameterFields(sim)) {
        g_tuned.erase(p.va);
        if (Tuned* t = Track(p))
            if (!Apply(p.va, *t, settings)) g_tuned.erase(p.va);
    }
}

// AirRide::MoveWheel(float*, state, low, high, neutral). The original 0x1288C0 supplies each axle's baseline as
// neutral, while its original low/high values are absolute and ignore a deep garage selection. Use the actual
// per-axle baseline for both endpoints: fully low H, fully high H+2*abs(H). Thus lowering mirrors above stock,
// and a raised setup keeps its selected height as the low endpoint with the same 2*abs(H) travel. Keep the rates,
// four-wheel sequencing, original state machine and installed-equipment checks unchanged.
X86_EXTERN_FUNC(__imp__sub_001284F0);
X86_FUNC(sub_001284F0) {
    const uint32_t control = ctx.ecx, stack = ctx.esp;
    if (SettingsSnapshot().airbag_match_ride && Committed(control + 8, 4) && Committed(stack + 4, 20)) {
        const uint32_t car = R32(control + 8), target = R32(stack + 4);
        bool owned = false;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto it = g_player_cars.find(car);
            owned = it != g_player_cars.end() && g_player_sims.count(it->second.sim) &&
                    target >= control + 0x20 && target <= control + 0x2C && ((target - control) & 3) == 0;
        }
        const float baseline = F32(stack + 0x14);
        if (owned && std::isfinite(baseline) && baseline != 0) {
            const float high = baseline + 2 * std::abs(baseline);
            // State +1 moves down toward arg4; state -1 moves up toward arg3 in the retail body.
            WF32(stack + 0x0C, high);
            WF32(stack + 0x10, baseline);
            // A different garage step may leave an in-flight airbag value outside the new range. Do not let the
            // retail mover's directional early-return strand it below its new lowest position.
            if (Committed(target, 4) && std::isfinite(F32(target)))
                WF32(target, std::clamp(F32(target), baseline, high));
        }
    }
    __imp__sub_001284F0(ctx, base);
}
