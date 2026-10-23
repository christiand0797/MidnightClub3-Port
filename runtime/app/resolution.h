#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace app::resolution {

// D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION. Keep this helper independent of Windows/D3D headers.
constexpr uint32_t kMaxDimension = 16384;
struct Extent {
    uint32_t width = 0, height = 0;
    bool operator==(const Extent&) const = default;
};

inline bool ValidScene(Extent e) {
    return e.width >= 320 && e.height >= 240 && e.width <= kMaxDimension && e.height <= kMaxDimension;
}

// Clamp both axes by one factor, so a supersampling request never changes the image's aspect ratio.
inline Extent Scale(Extent e, double factor) {
    if (!e.width || !e.height || !std::isfinite(factor) || factor <= 0) return {1, 1};
    double w = double(e.width) * factor, h = double(e.height) * factor;
    double cap = std::min(1.0, double(kMaxDimension) / std::max(w, h));
    return {std::clamp(uint32_t(std::lround(w * cap)), 1u, kMaxDimension),
            std::clamp(uint32_t(std::lround(h * cap)), 1u, kMaxDimension)};
}

inline Extent Scene(Extent explicit_size, Extent output, uint32_t percent) {
    return Scale(ValidScene(explicit_size) ? explicit_size : output, std::clamp(percent, 25u, 400u) / 100.0);
}

// The Xbox's 640x480 frame is anamorphic in widescreen. Its main and proportional effect/depth buffers must
// independently scale X and Y to obtain an actual 1920x1080 (etc.) scene, rather than a 1440x1080 intermediate.
// Square reflections/shadow maps retain uniform scaling. Extension overrides still take precedence at the caller.
inline Extent Target(Extent target, Extent guest_frame, Extent scene, double uniform_scale) {
    if (!target.width || !target.height || !guest_frame.width || !guest_frame.height) return {1, 1};
    if (uint64_t(target.width) * guest_frame.height == uint64_t(target.height) * guest_frame.width) {
        return Scale(scene, double(target.width) / guest_frame.width);
    }
    return Scale(target, uniform_scale);
}

struct Rect { int32_t left, top, right, bottom; };
inline Rect MapRect(Extent guest, Extent host, Rect r) {
    auto axis = [](int32_t v, uint32_t from, uint32_t to) {
        if (!from) return int32_t(0);
        return int32_t(std::lround(std::clamp(double(v) * to / from, 0.0, double(to))));
    };
    return {axis(r.left, guest.width, host.width), axis(r.top, guest.height, host.height),
            axis(r.right, guest.width, host.width), axis(r.bottom, guest.height, host.height)};
}

inline bool SameScale(Extent a_guest, Extent a_host, Extent b_guest, Extent b_host) {
    return uint64_t(a_host.width) * b_guest.width == uint64_t(b_host.width) * a_guest.width &&
           uint64_t(a_host.height) * b_guest.height == uint64_t(b_host.height) * a_guest.height;
}

inline Extent FitWindow(Extent requested, Extent available) {
    if (!requested.width || !requested.height || !available.width || !available.height) return {1, 1};
    double f = std::min({1.0, double(available.width) / requested.width, double(available.height) / requested.height});
    return {std::max(1u, uint32_t(requested.width * f)), std::max(1u, uint32_t(requested.height * f))};
}

}  // namespace app::resolution
