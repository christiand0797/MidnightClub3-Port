#pragma once
// Geometry math only. No renderer registration, scene-light inference or material activation.
#include "gpu/pbr_material.h"

#include <array>
#include <string_view>

namespace gpu::pbr::geometry {

struct Vec2 { float x = 0, y = 0; };

// Untransposed row-major storage: p_view = float4(p_model,1) * (model * view).
// This is NOT the transposed c.wv byte layout of the current legacy cbuffer.
struct RowMatrix4 {
    std::array<float, 16> values{};
    static constexpr RowMatrix4 Identity() {
        return {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    }
};
static_assert(sizeof(RowMatrix4)==16*sizeof(float));

enum class Status {
    Ready, NotReady, NonFinite, NonAffine, SingularTransform,
    DegeneratePosition, DegenerateNormal, DegenerateUv
};

struct AffineBridge {
    RowMatrix4 model_to_view;
    RowMatrix4 normal_to_view;  // Inverse transpose of the upper 3x3; other entries are 0, bottom-right 1.
    bool mirrored = false;     // det(linear(model * view)) < 0; does not automatically flip a normal.
    bool ready = false;
};

// Reject perspective, singular/near-collinear and nonfinite/unrepresentable transforms.
// Both input matrices must have exact affine last column (0,0,0,1).
Status BuildAffineBridge(const RowMatrix4& model, const RowMatrix4& view, AffineBridge& result);
Status ViewPosition(const AffineBridge& bridge, Vec3 model_position, Vec3& result);
Status ViewNormal(const AffineBridge& bridge, Vec3 model_normal, Vec3& result);

// A derivative cross product has a screen-coordinate orientation. The caller must establish
// flip_orientation from its actual raster/culling convention; no world-space orientation is inferred.
Status FaceNormal(Vec3 position_dx, Vec3 position_dy, bool flip_orientation, Vec3& result);

struct CotangentFrame {
    Vec3 tangent, bitangent, normal;
    // T and B use a COMMON normalization and need not be orthogonal or individually unit length.
    // They follow increasing U/V. Preserving this UV metric matters when consuming a normal map.
    float uv_orientation = 0;  // Sign of the derivative UV Jacobian, not an inferred asset property.
    float handedness = 0;     // Sign(dot(cross(T,B),N)); accounts for UV and geometric orientation.
    bool ready = false;
};

// Derivatives and geometric_normal must describe the same established coordinate space.
// No arbitrary axes or alternate UV set are substituted when geometry/UVs are degenerate.
Status BuildCotangentFrame(Vec3 position_dx, Vec3 position_dy, Vec2 uv_dx, Vec2 uv_dy,
                          Vec3 geometric_normal, CotangentFrame& result);

// Corresponding guarded HLSL helpers, CPU-compiled by tests. They are not inserted into any legacy shader.
std::string_view GeometryHlsl();

}  // namespace gpu::pbr::geometry
