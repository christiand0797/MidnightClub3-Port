#pragma once
// PC preferences only. No file I/O, material approval, renderer activation or original game-save writes.
#include "gpu/pbr_material.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace gpu::pbr::controls {

// Metadata/edit rows only. This does NOT increase the material loader's decoded/resident map budgets.
constexpr size_t kMaxMaterials = 8192;
constexpr size_t kMaxPackPathBytes = 1024;
enum class Quality : uint32_t { Low, Medium, High, Ultra };
enum class HeightMode : uint32_t { None, Parallax, Displacement };

struct Parameters {
    std::array<float,3> base_color{1,1,1}, emission_color{1,1,1}; // RGB tints 0..4; accepted alpha is untouched.
    float roughness_factor = 1, metallic_factor = 1; // 0..4; applied to the reviewed ORM channels.
    bool roughness_override = false, metallic_override = false;
    float roughness = 0.5f, metallic = 0; // 0..1; override only when the corresponding switch is set.
    float normal_strength = 1;   // 0..4
    float ao_strength = 1;       // 0..2; indirect AO = saturate(1 + (mapAO - 1) * strength).
    float emission_strength = 1; // 0..16
    HeightMode height_mode = HeightMode::None;
    // Established draw-coordinate distances; never inferred metres or UV-distance substitutes.
    float parallax_height = 0;      // 0..0.1; UV shift only, with proven position/UV/view derivatives.
    float displacement_height = 0; // 0..0.25; actual geometry movement requires separate renderer proof.
    float displacement_midpoint = 0.5f; // 0..1
    uint32_t tessellation = 1;      // 1..16, requires a genuine tessellation pipeline.
    bool operator==(const Parameters&) const = default;
};

struct ExactMaterialKey {
    std::string approved_manifest_sha256;
    Assignment assignment; // All texture/VS/PS/stage/UV/surface/cutout fields form one identity.
};
struct MaterialOverride {
    ExactMaterialKey key;
    bool enabled = true;
    bool use_global_parameters = true;
    Parameters parameters;
};
struct Settings {
    bool enabled = false; // The initial and missing-table behavior remains exactly the legacy renderer.
    std::string pack_path, approved_manifest_sha256;
    Quality quality = Quality::High;
    Parameters parameters;
    std::vector<MaterialOverride> materials;
};

inline char LowerHex(char c) { return c>='A'&&c<='F'?char(c-'A'+'a'):c; }
inline bool ValidDigest(std::string_view text) {
    if(text.size()!=64)return false;
    for(char c:text){c=LowerHex(c);if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
    return true;
}
inline bool DigestEqual(std::string_view a,std::string_view b) {
    if(!ValidDigest(a)||!ValidDigest(b))return false;
    for(size_t i=0;i<64;++i)if(LowerHex(a[i])!=LowerHex(b[i]))return false;return true;
}
inline bool ValidAssignment(const Assignment& a) {
    return a.original_texture&&a.vertex_shader&&a.pixel_shader&&a.stage<4&&(a.uv_set<4||a.uv_set==kAnyUv)&&
        (a.surface==SurfaceClass::World||a.surface==SurfaceClass::Vehicle);
}
inline bool AssignmentEqual(const Assignment& a,const Assignment& b) {
    return a.original_texture==b.original_texture&&a.vertex_shader==b.vertex_shader&&a.pixel_shader==b.pixel_shader&&
        a.stage==b.stage&&a.uv_set==b.uv_set&&a.surface==b.surface&&a.cutout==b.cutout;
}
inline bool AssignmentLess(const Assignment& a,const Assignment& b) {
    return std::tie(a.original_texture,a.vertex_shader,a.pixel_shader,a.stage,a.uv_set,a.surface,a.cutout)<
        std::tie(b.original_texture,b.vertex_shader,b.pixel_shader,b.stage,b.uv_set,b.surface,b.cutout);
}
struct ExactMaterialKeyLess {
    bool operator()(const ExactMaterialKey& a,const ExactMaterialKey& b) const {
        if(AssignmentLess(a.assignment,b.assignment))return true;
        if(AssignmentLess(b.assignment,a.assignment))return false;
        return a.approved_manifest_sha256<b.approved_manifest_sha256; // Keys are normalized before indexing.
    }
};
inline bool KeyEqual(const ExactMaterialKey& a,const ExactMaterialKey& b) {
    // The cheap assignment first: device lookups compare against every cached material on each draw.
    return AssignmentEqual(a.assignment,b.assignment)&&DigestEqual(a.approved_manifest_sha256,b.approved_manifest_sha256);
}
inline ExactMaterialKey ExactKey(const Assignment& assignment,std::string_view manifest) {
    ExactMaterialKey key{std::string(manifest),assignment};
    if(ValidDigest(key.approved_manifest_sha256))for(char& c:key.approved_manifest_sha256)c=LowerHex(c);
    return key;
}
inline float Bounded(float value,float fallback,float lo,float hi) {
    return std::isfinite(value)?std::clamp(value,lo,hi):fallback;
}
inline void Sanitize(Parameters& p) {
    for(float& c:p.base_color)c=Bounded(c,1,0,4);for(float& c:p.emission_color)c=Bounded(c,1,0,4);
    p.roughness_factor=Bounded(p.roughness_factor,1,0,4);p.metallic_factor=Bounded(p.metallic_factor,1,0,4);
    p.roughness=Bounded(p.roughness,0.5f,0,1);p.metallic=Bounded(p.metallic,0,0,1);
    p.normal_strength=Bounded(p.normal_strength,1,0,4);p.ao_strength=Bounded(p.ao_strength,1,0,2);
    p.emission_strength=Bounded(p.emission_strength,1,0,16);
    p.parallax_height=Bounded(p.parallax_height,0,0,0.1f);p.displacement_height=Bounded(p.displacement_height,0,0,0.25f);
    p.displacement_midpoint=Bounded(p.displacement_midpoint,0.5f,0,1);p.tessellation=std::clamp(p.tessellation,1u,16u);
    if(static_cast<uint32_t>(p.height_mode)>static_cast<uint32_t>(HeightMode::Displacement))p.height_mode=HeightMode::None;
}
inline bool ValidPackPath(std::string_view path) {
    if(path.empty()||path.size()>kMaxPackPathBytes)return false;
    for(unsigned char c:path)if(c<32||c==127)return false;return true;
}
inline void Sanitize(Settings& s) {
    Sanitize(s.parameters);
    if(static_cast<uint32_t>(s.quality)>static_cast<uint32_t>(Quality::Ultra))s.quality=Quality::High;
    if(!ValidPackPath(s.pack_path))s.pack_path.clear();
    if(ValidDigest(s.approved_manifest_sha256))for(char& c:s.approved_manifest_sha256)c=LowerHex(c);
    else s.approved_manifest_sha256.clear();
    // Bound both work and retained rows. Invalid identities are dropped; no filename/model retargeting.
    // Deterministic duplicate policy: the first valid exact key wins, even if a later row differs.
    size_t kept=0,n=std::min(s.materials.size(),kMaxMaterials);
    std::set<ExactMaterialKey,ExactMaterialKeyLess> seen;
    for(size_t i=0;i<n;++i) {
        MaterialOverride row=std::move(s.materials[i]);
        if(!ValidDigest(row.key.approved_manifest_sha256)||!ValidAssignment(row.key.assignment))continue;
        for(char& c:row.key.approved_manifest_sha256)c=LowerHex(c);
        if(!seen.insert(row.key).second)continue;Sanitize(row.parameters);s.materials[kept++]=std::move(row);
    }
    s.materials.resize(kept);
}
inline const MaterialOverride* FindOverride(const Settings& s,const ExactMaterialKey& key) {
    for(size_t i=0,n=std::min(s.materials.size(),kMaxMaterials);i<n;++i)
        if(AssignmentEqual(s.materials[i].key.assignment,key.assignment)&&DigestEqual(s.materials[i].key.approved_manifest_sha256,key.approved_manifest_sha256))return &s.materials[i];
    return nullptr;
}
inline const MaterialOverride* FindOverride(const Settings& s,const Assignment& assignment,std::string_view manifest) {
    for(size_t i=0,n=std::min(s.materials.size(),kMaxMaterials);i<n;++i)
        if(AssignmentEqual(s.materials[i].key.assignment,assignment)&&DigestEqual(s.materials[i].key.approved_manifest_sha256,manifest))return &s.materials[i];
    return nullptr;
}
inline MaterialOverride* FindOverride(Settings& s,const ExactMaterialKey& key) {
    return const_cast<MaterialOverride*>(FindOverride(static_cast<const Settings&>(s),key));
}

} // namespace gpu::pbr::controls
