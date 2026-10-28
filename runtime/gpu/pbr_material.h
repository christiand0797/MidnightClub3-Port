#pragma once
// Reviewed PBR assets and shading math. Nothing here registers a renderer extension or changes a draw.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gpu::pbr {

enum class ColorSpace { Linear, Srgb };
enum class MapRole { Albedo, Normal, Orm, Height, Emission };
enum class Storage { Rgba8, R16Unorm };
enum class NormalConvention { OpenGl, DirectX };
enum class SurfaceClass { World, Vehicle };

struct Mip {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA8, or little-endian R16_UNORM for height.
};
struct Map {
    MapRole role = MapRole::Albedo;
    ColorSpace color_space = ColorSpace::Linear;
    Storage storage = Storage::Rgba8;
    std::string file, sha256;
    std::vector<Mip> levels;
};
struct Assignment {
    uint64_t original_texture = 0, vertex_shader = 0, pixel_shader = 0;
    uint32_t stage = 0, uv_set = 0;
    SurfaceClass surface = SurfaceClass::World;
    bool cutout = false;
};
struct Material {
    Assignment assignment;
    NormalConvention normal_convention = NormalConvention::OpenGl;
    std::string review_sha256;
    // SHA256(width_u32le || height_u32le || every level-0 alpha byte), from the currently accepted colour.
    // A material cannot silently change coverage or reuse the game's reflectivity alpha as metalness.
    std::string baseline_alpha_sha256;
    Map albedo, normal, orm;
    bool has_height = false, has_emission = false;
    Map height, emission;
};

struct LoadPolicy {
    bool enabled = false;
    // An independently reviewed digest, supplied by the caller. The manifest cannot approve itself.
    std::string approved_manifest_sha256;
    uint32_t max_materials = 512, max_dimension = 8192;
    uint64_t max_encoded_file = 128ull << 20, max_decoded_bytes = 512ull << 20;
};
enum class LoadStatus { Disabled, Ready, Rejected };
struct Library {
    LoadStatus status = LoadStatus::Disabled;
    std::string manifest_sha256;
    std::vector<Material> materials;
    std::vector<std::string> errors;
};

// Explicit worker-thread preparation. Disabled returns before any filesystem/COM/hash operation.
// Reads ONLY materials-v1.toml and its explicitly listed, digest-pinned PNGs below root.
// Old generated manifest.json, filename suffixes and image-statistics classes are never auto-adopted.
Library LoadReviewedManifest(const std::filesystem::path& root, const LoadPolicy& policy);

// Supply facts from the actual draw/texture upload. Unknown or incompatible surfaces stay on the console path.
struct SurfaceEvidence {
    Assignment assignment;
    uint32_t color_width = 0, color_height = 0;
    std::string current_alpha_sha256;
    bool texture_2d = false, pretransformed = false, screen_space = false;
    bool dynamic_texture = false, render_target = false, depth_texture = false;
    bool additive = false, blended = false;
    bool geometric_position = false, geometric_normal = false, tangent_basis = false;
    bool linear_lighting = false;
};
enum class SelectionReason { Disabled, NoAssignment, UnsupportedDraw, MissingGeometry, MissingLinearLighting, AlphaMismatch, Ready };
struct Selection { const Material* material = nullptr; SelectionReason reason = SelectionReason::NoAssignment; };
Selection Select(const Library& library, const SurfaceEvidence& draw);

// CPU-only helpers used by the loader and by future upload/census integration.
std::string Sha256(const std::vector<uint8_t>& bytes);
std::string AlphaDigest(const Mip& rgba);

struct Vec3 { float x = 0, y = 0, z = 0; };
float SrgbToLinear(float encoded);
float LinearToSrgb(float linear);
Vec3 FresnelSchlick(Vec3 f0, float cos_theta);
float DistributionGgx(float n_dot_h, float perceptual_roughness);
float VisibilitySmith(float n_dot_v, float n_dot_l, float perceptual_roughness);
// Single-scattering GGX + separable Smith + Schlick Fresnel, Lambert diffuse. Linear inputs/output.
// Includes NdotL and radiance. AO is deliberately absent from direct lighting.
Vec3 EvaluateDirect(Vec3 base_color, float metallic, float roughness, Vec3 normal, Vec3 view, Vec3 light, Vec3 radiance);
Vec3 EvaluateIndirectDiffuse(Vec3 base_color, float metallic, float ao, Vec3 irradiance);
// bitangent follows increasing texture V. Green-up (OpenGL) maps therefore negate their Y component.
Vec3 TangentNormal(Vec3 sample, float strength, NormalConvention convention, Vec3 tangent, Vec3 bitangent, Vec3 normal);
std::string_view BrdfHlsl();

} // namespace gpu::pbr
