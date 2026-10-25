// Fit extended-profile tire openings to the selected rim without changing shared console resources.
// Original106F90 receives its render config at entry ESP+0x10, then reuses ESI as a draw-pass counter.
// Retain that config during the call: the third 1B5F20 argument is NOT a stable render pointer.
// Tire callers are1080C5/109A9C; retained axle index is ESP+0x34 (six arguments plus return address).
#include "garage.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace {
using kernel::G;
using kernel::R32;
struct DrawScope { uint32_t render = 0, resources = 0; };
thread_local DrawScope active_draw;

bool Committed(uint32_t va, uint64_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!bytes || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}

bool TireVertexData(uint32_t handle, float source_ratio, std::array<uint32_t, 4>& data_ids) {
    if (!Committed(handle, 8)) return false;
    const uint32_t record = R32(handle + 4);
    if (!Committed(record, 0x70)) return false;
    const uint32_t bytes = R32(record + 0xC);
    if (bytes < 0x70 || bytes > 1024 * 1024 || !Committed(record, bytes)) return false;
    const uint64_t end = uint64_t(record) + bytes;
    auto contains = [&](uint32_t p, uint64_t n) {
        return p >= uint64_t(record) + 0x20 && n && uint64_t(p) + n <= end;
    };
    unsigned output = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t shape = R32(record + 0x30 + 4 * slot);
        if (!shape) continue;
        if (!contains(shape, 12)) return false;
        const unsigned count = R32(shape + 4) >> 16;
        const uint32_t table = R32(shape + 8);
        if (!count || count > 4 || !contains(table, count * 4)) return false;
        for (unsigned i = 0; i < count; ++i) {
            if (output >= data_ids.size()) return false;
            const uint32_t geometry = R32(table + 4 * i);
            if (!contains(geometry, 0x20) || (R32(geometry + 4) >> 24) != 28) return false;
            const uint32_t vb = R32(geometry + 0x18), desc = R32(geometry + 0x10);
            if (!contains(vb, 12) || !contains(desc, 4) || (R32(vb) & 0x70000u) != 0) return false;
            const uint32_t raw_data = R32(vb + 4), vertices = mem::VirtualFromPhysical(raw_data);
            const uint32_t count_word = R32(desc);
            if (count_word < 4 || !contains(count_word - 4, 12)) return false;
            const uint32_t ib = count_word - 4, encoded = R32(count_word);
            const uint32_t index_count = encoded >> 16, indices = R32(geometry + 0x1C);
            if ((encoded & 0xFFFFu) || !index_count || !contains(indices, uint64_t(index_count) * 2) ||
                ib <= vertices || (ib - vertices) % 28 || !contains(vertices, ib - vertices)) return false;
            const unsigned vertex_count = (ib - vertices) / 28;
            if (!vertex_count || vertex_count > 1024) return false;
            for (unsigned k = 0; k < index_count; ++k)
                if (G<uint16_t>(indices + 2 * k) >= vertex_count) return false;
            float minimum = 1, maximum = 0;
            for (unsigned k = 0; k < vertex_count; ++k) {
                const float x = G<float>(vertices + 28 * k), y = G<float>(vertices + 28 * k + 4),
                            z = G<float>(vertices + 28 * k + 8);
                const float radius = std::hypot(y, z);
                // The rounded stock shoulder extends beyond nominal +/-0.25 (as far as -0.263825).
                // Permit that measured bulge rather than silently rejecting every high-detail stock7 alias.
                if (!std::isfinite(x) || !std::isfinite(radius) || std::fabs(x) > 0.27f) return false;
                minimum = std::min(minimum, radius); maximum = std::max(maximum, radius);
            }
            // Fail closed for special/bike/unrecognised meshes: this normalization is measured on the
            // original 88 ordinary models, independently in both draw slots and every tire brand.
            if (std::fabs(minimum - source_ratio * 0.5f) > 0.0001f || maximum < 0.499f || maximum > 0.501f)
                return false;
            data_ids[output++] = raw_data;
        }
    }
    return output > 0;
}
} // namespace

X86_EXTERN_FUNC(__imp__sub_00106F90);
X86_FUNC(sub_00106F90) {
    struct RestoreDraw {
        DrawScope previous;
        ~RestoreDraw() { active_draw = previous; }
    } restore{active_draw};
    active_draw = {Committed(ctx.esp + 0x10, 4) ? R32(ctx.esp + 0x10) : 0, ctx.ecx};
    __imp__sub_00106F90(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001B5F20);
X86_FUNC(sub_001B5F20) {
    gpu::State::TireFit fit{};
    const uint32_t caller = Committed(ctx.esp, 0x38) ? R32(ctx.esp) : 0;
    if ((caller == 0x1080C5 || caller == 0x109A9C) && ctx.ebx == active_draw.resources) {
        const uint32_t axle = R32(ctx.esp + 0x34);
        if (GarageTireFitForDraw(active_draw.render, ctx.ebx, ctx.ecx, axle, fit.source_ratio, fit.target_ratio) &&
            !TireVertexData(ctx.ecx, fit.source_ratio, fit.vertex_data)) fit = {};
    }
    // State is snapshotted by the threaded backend at the actual draw. Never temporarily edit guest VBs:
    // the render worker deliberately reads their vertex data later. RAII also prevents an exceptional call
    // or a following non-tire resource draw from inheriting the previous car's requested ratio.
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    auto& state = d3d::State();
    struct Restore {
        gpu::State::TireFit& destination;
        gpu::State::TireFit previous;
        ~Restore() { destination = previous; }
    } restore{state.tire_fit, state.tire_fit};
    state.tire_fit = fit;
    if (fit.vertex_data[0]) LOG_FIRST(8, "garage: tire bead fit %.6f -> %.6f, axle %u, side %u",
                                    fit.source_ratio, fit.target_ratio, R32(ctx.esp + 0x34), caller == 0x109A9C);
    __imp__sub_001B5F20(ctx, base);
}
