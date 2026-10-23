// Level of detail and draw distance scale (docs/notes/lod_fov.md has every address and the evidence).
//
// WIRING (lead): this file owns
//     float g_lod_scale = 1.0f;     // 1 = console; 2 = LOD switches and prop draw distance twice as far; 0.5 = half
// Declare `extern float g_lod_scale;` where settings are applied and assign it from the new setting (any time, it
// is read live, clamped here to [0.25, 4]). Nothing else needs calling: every effect below is applied from hooks.
//
// What the scale changes:
//  1. Model LOD switches (vehicles incl. traffic, peds, entities, fx: every "type" with a lodgroup {high/med/low/
//     vlow <meshes> <distance>}). The type's LOD selector sub_000E62B0 (vtable slot +0x24 of the type classes
//     0x4540D0 and 0x467540) asks the viewport's sphere test sub_002D45C0 for the view depth of the bounding sphere
//     and picks the first level whose switch distance (type+0x20..+0x2C) is >= depth - radius; a level whose mesh
//     is "none" (traffic: "med none 9999") hides the model, so the last real level's distance is its draw distance.
//     The hook on sub_002D45C0 rewrites that one caller's depth to radius + (depth - radius) / scale, which is
//     exactly the same as multiplying every switch distance by the scale, without writing the type.
//     The lodgroup's mesh prefetch sub_000E9260 (distance argument 2) gets the same distance / scale.
//  2. City props (tune/prop/*.propdata cull_dist, 100-400 m): prop type +0x9C (loaded by the prop type's virtual
//     loader sub_0019D980), and the prop manager's search range (+0x303C = the largest cull_dist loaded, prop
//     manager pointer at 0x4CB6E4). Both are read by the prop manager's draw sub_0019BF50 (40 m grid cells). Prop
//     types are tracked like tuning.cpp (loaded value + neighbour fingerprint + committed pages, dropped when the
//     prop manager's destructor sub_0019A630 runs) and set to loaded * scale once per prop draw; the manager range
//     is scaled only for the duration of its own draw call and restored afterwards.
//  3. The camera far clip (2000 m single player, 800/500 m split screen, set by sub_002C3F70 from the camera
//     manager setup sub_002C4040) is raised by min(scale, 2) when the scale is above 1. It is set when a camera
//     manager is created (race / free roam load), so this part takes effect on the next load.
// Not covered: the city's own baked geometry (resources/city/<city>_<tod>.xbck, PVS driven) has no distance LOD
// found; the traffic .vehicle "DrawDistance" (-25..-120, +0x1CC of the ambient vehicle type, visitor sub_0007C350)
// has no reader found, traffic visibility is decided by its .type lodgroup (item 1).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

float g_lod_scale = 1.0f;

using kernel::R32;
using kernel::W32;

namespace {
constexpr uint32_t kTypeLodReturn = 0x000E6342;      // return address of sub_000E62B0's call to sub_002D45C0
constexpr uint32_t kPropCull = 0x9C, kPropRadius = 0xA0, kPropFlags = 0x94;  // prop type fields
constexpr uint32_t kPropManagerRange = 0x303C;       // prop manager: largest cull_dist, grid search radius
constexpr float kMinScale = 0.25f, kMaxScale = 4.0f, kMaxFarScale = 2.0f;

float Scale() {
    float s = g_lod_scale;
    if (!std::isfinite(s)) return 1.0f;
    return std::clamp(s, kMinScale, kMaxScale);
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

bool Committed(uint32_t va, uint32_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!bytes || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
bool Plausible(uint32_t va) { return va >= 0x10000 && va < 0x84000000 && (va & 3) == 0; }

// a prop type's cull_dist as loaded, what this file last wrote, and two neighbours that must still hold what they
// held (a freed and reused block is let go instead of written)
struct Prop {
    float original, written;
    uint32_t radius, flags;  // values of +0xA0 and +0x94 at load
};
std::mutex g_lock;
std::unordered_map<uint32_t, Prop> g_props;  // prop type address -> state

bool PropReadable(uint32_t type) { return Plausible(type) && Committed(type + kPropFlags, 0x10); }

void ApplyProps(float s) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto it = g_props.begin(); it != g_props.end();) {
        const uint32_t type = it->first;
        Prop& p = it->second;
        if (!PropReadable(type) || R32(type + kPropRadius) != p.radius || R32(type + kPropFlags) != p.flags ||
            F32(type + kPropCull) != p.written) {
            it = g_props.erase(it);  // freed, reused or reloaded: the loader hook tracks it again if it comes back
            continue;
        }
        float want = p.original * s;
        if (std::isfinite(want) && want != p.written) {
            WF32(type + kPropCull, want);
            p.written = want;
        }
        ++it;
    }
}
}  // namespace

// Viewport sphere test (thiscall, stdcall args x, y, z, radius, float* depth_out; returns 0 outside, 1 inside,
// 2 crossing a side plane, 3 near, 4 far). Only the type LOD selector's call is changed.
X86_EXTERN_FUNC(__imp__sub_002D45C0);
X86_FUNC(sub_002D45C0) {
    const uint32_t ret = R32(ctx.esp);
    if (ret != kTypeLodReturn) {
        __imp__sub_002D45C0(ctx, base);
        return;
    }
    const float radius = F32(ctx.esp + 0x10);
    const uint32_t out = R32(ctx.esp + 0x14);
    __imp__sub_002D45C0(ctx, base);
    const float s = Scale();
    if (s == 1.0f || !out || !Plausible(out) || !Committed(out, 4) || !std::isfinite(radius)) return;
    const float depth = F32(out);
    if (!std::isfinite(depth)) return;
    WF32(out, radius + (depth - radius) / s);  // same as every lodgroup switch distance * s
}

// Lodgroup mesh prefetch (thiscall this = lodgroup, args: x, distance; ret 8): distance / s, matching the selector.
X86_EXTERN_FUNC(__imp__sub_000E9260);
X86_FUNC(sub_000E9260) {
    const float s = Scale();
    if (s != 1.0f) {
        const float d = F32(ctx.esp + 8);
        if (std::isfinite(d)) WF32(ctx.esp + 8, d / s);
    }
    __imp__sub_000E9260(ctx, base);
}

// Prop type loader (virtual, thiscall this = prop type, ret 8): cull_dist (+0x9C) is final when it returns.
X86_EXTERN_FUNC(__imp__sub_0019D980);
X86_FUNC(sub_0019D980) {
    const uint32_t type = ctx.ecx;
    __imp__sub_0019D980(ctx, base);
    if (!PropReadable(type)) return;
    const float v = F32(type + kPropCull);
    if (!std::isfinite(v) || v <= 0.0f) return;
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_props.find(type);
    if (it != g_props.end() && it->second.written == v) return;  // visited again with the value this file wrote
    g_props[type] = {v, v, R32(type + kPropRadius), R32(type + kPropFlags)};
}

// Prop manager draw (virtual, thiscall this = prop manager; once per view and frame): prop types are set from the
// current scale, and the manager's search range is scaled for this call only.
X86_EXTERN_FUNC(__imp__sub_0019BF50);
X86_FUNC(sub_0019BF50) {
    const float s = Scale();
    ApplyProps(s);
    const uint32_t range = ctx.ecx + kPropManagerRange;
    float original = 0.0f, written = 0.0f;
    bool scaled = false;
    if (s != 1.0f && Plausible(ctx.ecx) && Committed(range, 4)) {
        original = F32(range);
        written = original * s;
        if (std::isfinite(original) && original > 0.0f && std::isfinite(written)) {
            WF32(range, written);
            scaled = true;
        }
    }
    __imp__sub_0019BF50(ctx, base);
    // a prop type loaded meanwhile may have raised the range: keep the game's value then
    if (scaled && Committed(range, 4) && F32(range) == written) WF32(range, original);
}

// Prop manager destructor (thiscall, ret 4): its prop types go with it.
X86_EXTERN_FUNC(__imp__sub_0019A630);
X86_FUNC(sub_0019A630) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_props.clear();
    }
    __imp__sub_0019A630(ctx, base);
}

// Viewport far clip setter used by the camera manager (thiscall this = viewport, arg far, ret 4).
X86_EXTERN_FUNC(__imp__sub_002C3F70);
X86_FUNC(sub_002C3F70) {
    const float s = std::min(Scale(), kMaxFarScale);
    if (s > 1.0f) {
        const float far_clip = F32(ctx.esp + 4);
        if (std::isfinite(far_clip) && far_clip > 0.0f) {
            WF32(ctx.esp + 4, far_clip * s);
            LOG_FIRST(4, "lod: camera far clip %.0f -> %.0f m (lod scale %.2f)", far_clip, far_clip * s, Scale());
        }
    }
    __imp__sub_002C3F70(ctx, base);
}
