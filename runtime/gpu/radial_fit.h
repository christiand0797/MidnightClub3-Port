#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure per-vertex math for the ordinary tire mesh convention: X is the axle, outer YZ radius is 0.5,
// and the stock profile's bead radius is source_ratio/2. Callers own their vertex copy and decide which
// concrete car/draw it belongs to. Never run this against a shared guest vertex buffer in place.
namespace gpu::radial_fit {

inline int32_t Signed(uint32_t value, unsigned bits) noexcept {
    const uint32_t mask = (uint32_t(1) << bits) - 1;
    value &= mask;
    return value & (uint32_t(1) << (bits - 1)) ? int32_t(value) - int32_t(uint32_t(1) << bits) : int32_t(value);
}

inline bool Vertex(float position[3], uint32_t& packed_normal, float source_ratio, float target_ratio) noexcept {
    if (!position || !std::isfinite(source_ratio) || !std::isfinite(target_ratio) || source_ratio <= 0 ||
        source_ratio >= 1 || target_ratio <= 0 || target_ratio >= 1 || !std::isfinite(position[0]) ||
        !std::isfinite(position[1]) || !std::isfinite(position[2])) return false;
    const double y = position[1], z = position[2], radius = std::hypot(y, z);
    if (!std::isfinite(radius) || radius <= 0) return false;
    // Preserve every input bit for stock identity and the existing tread/outer bulge. No quantization drift.
    if (source_ratio == target_ratio || radius >= 0.5) return true;

    const double slope = (1.0 - target_ratio) / (1.0 - source_ratio);
    const double warped = target_ratio * 0.5 + (radius - source_ratio * 0.5) * slope;
    if (!std::isfinite(slope) || slope <= 0 || !std::isfinite(warped) || warped <= 0 || warped >= 0.5) return false;
    const double tangent_scale = warped / radius;
    const double ey = y / radius, ez = z / radius;
    const double nx = std::max(-1.0, Signed(packed_normal, 11) / 1023.0);
    const double ny = std::max(-1.0, Signed(packed_normal >> 11, 11) / 1023.0);
    const double nz = std::max(-1.0, Signed(packed_normal >> 22, 10) / 511.0);
    const double radial = ny * ey + nz * ez;
    // J is diagonal in (axial, radial, tangential) coordinates. Normals use inverse-transpose(J),
    // not the point's radial scale; a uniform scale or unchanged normal is wrong on rounded shoulders.
    const double out_y = radial * ey / slope + (ny - radial * ey) / tangent_scale;
    const double out_z = radial * ez / slope + (nz - radial * ez) / tangent_scale;
    const double length = std::hypot(nx, out_y, out_z);
    const float py = float(warped * ey), pz = float(warped * ez);
    if (!std::isfinite(length) || length <= 0 || !std::isfinite(py) || !std::isfinite(pz)) return false;
    auto component = [](double value, int scale, unsigned bits) {
        const auto q = std::clamp(std::lround(value * scale), long(-scale), long(scale));
        return uint32_t(q) & ((uint32_t(1) << bits) - 1);
    };
    const uint32_t normal = component(nx / length, 1023, 11) |
                            (component(out_y / length, 1023, 11) << 11) |
                            (component(out_z / length, 511, 10) << 22);
    // Only publish after all checks. X, UVs, colour, topology and resource ownership are caller-preserved.
    position[1] = py;
    position[2] = pz;
    packed_normal = normal;
    return true;
}

}  // namespace gpu::radial_fit
