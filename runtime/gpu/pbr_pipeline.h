#pragma once
// Dedicated, opt-in material pipeline contracts. No device, renderer registration or asset generation.
#include "gpu/pbr_geometry.h"
#include "gpu/pbr_settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace gpu::pbr::pipeline {

constexpr uint32_t kLightCount = 8;
constexpr uint32_t kMapCount = 5;
constexpr uint32_t kMaxMeshVertices = 1u << 20;
constexpr uint32_t kMaxMeshIndices = 3u << 20;
constexpr uint64_t kMaxTessellatedTriangles = 1ull << 20;
enum class MapSlot : uint32_t { Albedo, Normal, Orm, Height, Emission };
enum class OutputPolicy : uint32_t { LinearHdr, ToneMappedSrgb };
enum class TessellationPolicy : uint32_t { UniformSharedEdges, SymmetricViewEdges };
enum class LightType : uint32_t { Directional, Point, Spot };
enum class Status {
    Ready, Disabled, IdentityMismatch, UnsupportedDraw, MissingGeometry, MissingLighting,
    MissingOutput, MissingBindings, MissingHeight, MissingDisplacementProof, InvalidConstants,
    InvalidMesh, InvalidPipeline
};
const char* StatusText(Status value);

// Backend packs actual decoded fixed-function inputs into this layout. No guest register inference.
// color is the actual decoded register-3 diffuse (r,g,b,a); streams without one keep white. Only the
// baked scene mode consumes it.
// uv1: the window layer's own coordinates (GeometryConstants::options[3]), zero otherwise.
struct Vertex { Vec3 model_position, model_normal; geometry::Vec2 uv; std::array<float,4> color{1,1,1,1}; geometry::Vec2 uv1{}; };
static_assert(sizeof(Vertex)==56 && offsetof(Vertex,model_normal)==12 && offsetof(Vertex,uv)==24 && offsetof(Vertex,color)==32 &&
              offsetof(Vertex,uv1)==48);
// Authored: register-2 normals. Face: the stream has none; the pixel shader shades with the geometric
// normal of the rasterized triangle and packed vertex normals only orient/interpolate.
enum class NormalSource : uint32_t { Authored, Face };

struct alignas(16) GeometryConstants {
    geometry::RowMatrix4 model_to_view, normal_to_view, model_to_clip, view_to_clip;
    // Actual target pixels; adaptive shared-edge target in pixels. Unused fourth component is zero.
    std::array<float,4> raster{0,0,16,0};
    std::array<float,4> surface{}; // Actual guest surface width/height; remaining words zero.
    // TessellationPolicy, NormalSource, albedo gamma encoded (1: the game's own UNORM texture, decoded in the shader
    // exactly as the legacy path filters it), window layer (1: the game's stage-1 texture on uv1, times the material's
    // emission colour and the albedo alpha, added after encoding: the facades' lit night windows). Camera is
    // view-space origin.
    std::array<uint32_t,4> options{};
    // Reversed depth as the legacy draw stores it (ReversedDepthHlsl's zparams): a w buffer's scale (0 for a z
    // buffer), its far w, the polygon offset; the fourth word 1 when depth is reversed, else 0 and the rest unused.
    std::array<float,4> depth{};
};
struct alignas(16) MaterialConstants {
    std::array<float,4> base_color{1,1,1,1}; // Alpha is always one: never change accepted coverage.
    std::array<float,4> emission_color{1,1,1,0};
    std::array<float,4> surface{1,1,1,1}; // roughness factor, metallic factor, normal strength, AO strength
    std::array<float,4> overrides{0.5f,0,0.5f,1}; // roughness, metallic, original alpha cutoff, emission strength
    std::array<float,4> height{0,0,0.5f,1}; // parallax distance, displacement distance, midpoint, max tessellation
    // DirectX normal convention, roughness override, metallic override, controls::HeightMode.
    std::array<uint32_t,4> options{};
    // Has normal, has ORM, has height, has emission. Required maps still receive valid neutral bindings.
    std::array<uint32_t,4> maps{1,1,0,0};
    // Cutout boolean. Remaining words zero. No reflectivity-alpha-to-metallic inference.
    std::array<uint32_t,4> coverage{};
};
struct alignas(16) LightConstants {
    std::array<float,4> position_range{};
    std::array<float,4> direction_type{};
    std::array<float,4> radiance{}; // Already linear, nonnegative incident light color/intensity.
    std::array<float,4> attenuation_falloff{1,0,0,1};
    std::array<float,4> cone{1,-1,0,0}; // cos(inner/2), cos(outer/2)
};
struct alignas(16) SceneConstants {
    std::array<float,4> irradiance_exposure{0,0,0,1}; // Linear diffuse irradiance, exposure.
    std::array<uint32_t,4> info{}; // light count, OutputPolicy, baked vertex-diffuse mode, zero.
    std::array<LightConstants,kLightCount> lights{};
    // The original fixed-function fog: start, end, density, D3DRS_FOGTABLEMODE (0 = none), and the
    // D3DRS_FOGCOLOR blended over the encoded output exactly as the legacy pixel shader does.
    std::array<float,4> fog{};
    std::array<float,4> fog_color{};
};
static_assert(sizeof(GeometryConstants)==320 && sizeof(MaterialConstants)==128 && sizeof(LightConstants)==80);
static_assert(sizeof(SceneConstants)==704 && offsetof(SceneConstants,lights)==32 && offsetof(SceneConstants,fog)==672);

struct Light {
    LightType type = LightType::Directional;
    Vec3 position{}, direction{0,0,-1}, linear_radiance{};
    float range=1, attenuation0=1, attenuation1=0, attenuation2=0, falloff=1;
    float cos_inner=1, cos_outer=-1;
};
// All builders fail with empty output. Matrices use the geometry module's explicit row-vector convention.
Status BuildGeometry(const geometry::AffineBridge& affine, const geometry::RowMatrix4& exact_model_to_clip,
                     const geometry::RowMatrix4& actual_view_to_clip,
                     uint32_t target_width,uint32_t target_height,uint32_t guest_width,uint32_t guest_height,TessellationPolicy policy,
                     float target_edge_pixels,GeometryConstants& result);
Status BuildMaterial(const controls::Parameters& parameters,NormalConvention convention,
                     bool height_map,bool emission_map,bool cutout,float original_alpha_cutoff,
                     MaterialConstants& result);
Status BuildScene(std::span<const Light> actual_linear_lights,Vec3 linear_irradiance,float exposure,
                  OutputPolicy policy,SceneConstants& result);
// LIGHTING-off mode: no lights and no uniform irradiance. The pixel shader multiplies albedo by the actual
// interpolated vertex diffuse, decoded with the same sRGB curve as the albedo so neutral maps reproduce the
// original texture*diffuse product, and saturates before encoding like the original combiner.
Status BuildBakedScene(float exposure,OutputPolicy policy,SceneConstants& result);
// Packs the actual fog state into an already built scene. Mode 0 is no fog. Fails without touching result.
Status SetFog(uint32_t table_mode,float start,float end,float density,std::array<float,3> color,SceneConstants& result);
Status ValidateConstants(const GeometryConstants&,const MaterialConstants&,const SceneConstants&);
// A bounded CPU check of packed triangle lists and authored normals. Triangles without a tangent frame (degenerate
// UVs or slivers) are allowed when at least one triangle has one; their normal is simply left unperturbed.
// Passing does not establish an original model, UV set or shader identity; those remain separate proof.
// Face normals only need unit packed normals; the cotangent check then uses each triangle's own plane.
// why, when given, names the first failing rule and counts the triangles that fail.
Status ValidateMesh(std::span<const Vertex> vertices,std::span<const uint32_t> indices={},
                    NormalSource normals=NormalSource::Authored,std::string* why=nullptr);
Status ValidateTessellationBudget(uint32_t patch_count,uint32_t max_tessellation);

struct Evidence {
    bool enabled=false, approved_assignment=false;
    uint64_t texture=0,vertex_shader=0,pixel_shader=0;
    uint64_t expected_texture=0,expected_vertex_shader=0,expected_pixel_shader=0;
    bool fixed_transform=false, triangle_list=false, opaque=false;
    bool pretransformed=false, vertex_blended=false, programmable=false, screen_space=false;
    bool packed_mesh_validated=false, original_normal_uv_proven=false, affine_bridge_proven=false;
    bool view_projection_proven=false, linear_lights_proven=false, output_policy_proven=false;
    bool all_five_bindings=false, height_map=false;
    bool tessellation_supported=false, shared_edges_and_uv_continuity_proven=false;
    bool displacement_bounds_and_culling_proven=false, height_units_proven=false;
    bool displacement_depth_and_multipass_proven=false, displacement_budget_proven=false;
};
Status Eligible(const Evidence&,controls::HeightMode);

// These helpers describe real D3D12 resources/PSOs, but never create or bind a device object.
// b0 geometry; b1 material; b2 scene; t0..4 maps; s0 sampler. DS requires the height SRV/sampler.
struct RootLayout {
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
    std::array<D3D12_ROOT_PARAMETER,5> parameters{};
    D3D12_ROOT_SIGNATURE_DESC Describe(); // Returned pointers remain valid while this object is unmoved.
};
std::span<const D3D12_INPUT_ELEMENT_DESC> InputLayout();
D3D_PRIMITIVE_TOPOLOGY DrawTopology(controls::HeightMode);
struct ShaderSet { D3D12_SHADER_BYTECODE vs{},ps{},hs{},ds{}; };
// Compositing the dedicated path keeps from the original draw: none, ONE/ZERO (opaque with blending on) or SRC_ALPHA/
// INV_SRC_ALPHA (cutouts drawn over what is behind). The output alpha is the original's, so either composites alike.
bool KeepsLegacyBlend(const D3D12_BLEND_DESC&);
Status DescribePipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& original_opaque_state,
                       ID3D12RootSignature* dedicated_root,const ShaderSet&,controls::HeightMode,
                       D3D12_GRAPHICS_PIPELINE_STATE_DESC& result);

// Complete separate shader stages; unchanged legacy VOut/shader/root paths do not consume them.
// PS returns scene-linear radiance or explicitly exposure/Reinhard/sRGB encoded output.
std::string VertexHlsl(bool tessellated=false);
std::string PixelHlsl();
std::string HullHlsl();
std::string DomainHlsl();
// CPU reference for output and actual point/spot/directional light evaluation, used by contract tests.
Vec3 EncodeOutput(Vec3 linear_radiance,float exposure,OutputPolicy);
Vec3 LightRadiance(const Light&,Vec3 view_position,Vec3& direction_to_light);
Status DisplaceViewPosition(Vec3 view_position,Vec3 view_normal,float height_sample,float distance,
                            float midpoint,Vec3& result);

} // namespace gpu::pbr::pipeline
