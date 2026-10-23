// Camera field of view scale (docs/notes/lod_fov.md has every address and the evidence).
//
// WIRING (lead): this file owns
//     float g_fov_scale = 1.0f;     // multiplies the game camera's vertical FOV in degrees; 1 = console
// Declare `extern float g_fov_scale;` where settings are applied and assign it from the new setting (read live every
// frame, clamped here to [0.5, 2]; the result is kept within [5, 150] degrees).
//
// How the game projects: sub_002D4AE0 (viewport thiscall, stdcall args fov_degrees, aspect, near, far; ret 0x10)
// stores fov at viewport+0x178 and aspect at +0x17C (0 = the viewport's own aspect +0x128 = width/height *
// pixel aspect 0x4B4E88, which the video mode code sets from the display aspect), then t = tan(fov * pi/360):
// +0x13C = t, +0x138 = t * aspect, projection m11 (+0x18) = 1/t, m00 (+0x04) = 1/(t*aspect), and the four side
// planes (+0x140..+0x15C) that the visibility tests use. The FOV is VERTICAL and the horizontal FOV follows the
// aspect (Hor+): a 16:9 output already sees wider, unstretched; this scale is an extra preference on top.
//
// The cameras: the camera base visitor sub_00138470 reads CameraFOV (+0x84), CameraNear (+0x88), CameraFar (+0x8C)
// (tune/camera/*.campovcs; chase cameras *.camtrackcs drive +0x84 from SSFovMin..SSFovMax, 40..60, by speed).
// Only these calls are scaled, recognised by their return address:
//   0x0013853A  sub_00138510 camera projection (fov +0x84, aspect +0x90)
//   0x0013879F  sub_00138750 camera projection, global clip (fov = +0x84 * zoom +0x38 * 1.075)
//   0x001387BE  sub_00138750 camera projection, camera clip
//   0x00138E6C  camera blend (sub_00138C60 / sub_00138CFB): fov lerped between two cameras
//   0x002C4201  sub_002C4040 camera manager setup
// Other callers re-apply the viewport's stored (already scaled) fov (sub_002C3F70, sub_002D4F10 viewport rect,
// the environment map pass sub_00331840 which copies the main viewport 0x558BB4) and are left alone, so nothing is
// scaled twice; the rear view mirror (sub_000DAF50), HUD arrows (sub_000D11A0) and env-map cameras keep their own.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "xdk/hle.h"

float g_fov_scale = 1.0f;

using kernel::R32;
using kernel::W32;

namespace {
constexpr uint32_t kCameraReturns[] = {0x0013853A, 0x0013879F, 0x001387BE, 0x00138E6C, 0x002C4201};
constexpr float kMinScale = 0.5f, kMaxScale = 2.0f, kMinFov = 5.0f, kMaxFov = 150.0f;

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

bool FromCamera(uint32_t ret) {
    for (uint32_t r : kCameraReturns)
        if (r == ret) return true;
    return false;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002D4AE0);
X86_FUNC(sub_002D4AE0) {
    float s = g_fov_scale;
    if (std::isfinite(s) && s != 1.0f && FromCamera(R32(ctx.esp))) {
        s = std::clamp(s, kMinScale, kMaxScale);
        const float fov = F32(ctx.esp + 4);
        if (std::isfinite(fov) && fov > 0.0f && fov < 180.0f) {
            const float want = std::clamp(fov * s, kMinFov, kMaxFov);
            WF32(ctx.esp + 4, want);  // the caller's pushed argument; the callee pops it (ret 0x10)
            LOG_FIRST(2, "fov: camera %.1f -> %.1f degrees (vertical, scale %.2f)", fov, want, s);
        }
    }
    __imp__sub_002D4AE0(ctx, base);
}
