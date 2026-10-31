#pragma once
// Conditional original-state lighting bridge. No device calls, guest reads, defaults, or fake illumination.
#include "gpu/pbr_pipeline.h"
#include "gpu/state.h"

namespace gpu::pbr::scene {

struct Proof {
    // Producer-side observations, not the legacy pre-original shadow's assumptions.
    bool light_writes_after_success = false;
    bool light_index_coverage = false;
    bool stateblock_valid = false;
    // Facts from the actual selected draw and matrices. Numeric validity is rechecked by Build.
    bool fixed_function_lighting_proven = false;
    bool rigid_view_proven = false;
    bool perspective_camera_proven = false;
    bool opaque_no_cutout_proven = false;
    // Explicit reviewed interpretation: retain API float RGB as normalized linear coefficients.
    // Packed ambient is UNORM-as-linear coefficient; this does not claim calibrated lumens/metres.
    bool api_colors_linear = false;
    // Actual target/encoding/composition policy, supplied by the backend; never inferred from State.
    bool output_policy_proven = false;
    // The dedicated shader reproduces the legacy fixed-function fog from the same draw's fog states.
    // Without it a fogged draw stays unsupported.
    bool fog_reproduced = false;
    float exposure = 1;
};
// A separate compatibility mode for an explicitly reviewed world draw whose unlit fixed vertex
// translation forwards the actual stream register 3 to saturated oD0. These facts belong to ONE
// immutable State/assembly/stream snapshot; a texture name, census class or missing light is no proof.
// The coefficients are retained normalized-linear inputs by declared policy, not recovered physical
// irradiance/radiance, and establish no direct light, environment reflection or normal-map relighting.
struct BakedProof {
    bool immutable_draw_proven = false;
    bool fixed_world_source_proven = false;
    bool vertex_diffuse_stream_proven = false;
    bool vertex_diffuse_forwarded_proven = false;
    // The original fixed pixel path adds v1.rgb after the cascade even with LIGHTING off.
    bool vertex_specular_zero_proven = false;
    bool albedo_diffuse_combiner_proven = false;
    bool vertex_diffuse_alpha_one_proven = false;
    // A stage-1 CURRENT*TFACTOR tint is accepted only when the backend multiplied the packed diffuse RGB by the
    // actual D3DRS_TEXTUREFACTOR, which reproduces the original product exactly (every factor is within [0,1]).
    bool texture_factor_applied = false;
    // The backend packed the cascade's alpha factors (vertex alpha where read, TFACTOR alpha where modulated) into
    // the vertex alpha, and the baked shader writes albedo alpha times it: the original alpha, whatever its value.
    bool combiner_alpha_packed = false;
    bool rigid_view_proven = false;
    bool perspective_camera_proven = false;
    bool opaque_no_cutout_proven = false;
    // The dedicated pipeline keeps the draw's coverage: a GREATER/GEQUAL alpha test as the same clip on the same
    // (packed) alpha, and its blend state as it is (pipeline::KeepsLegacyBlend). Cutouts then need no opaque state.
    bool coverage_kept = false;
    // The pipeline adds the cascade's stage 1 (texture*TFACTOR) as the window layer over the encoded surface, times the
    // base's alpha, and writes that alpha: the facades' MODULATEALPHA_ADDCOLOR cascade instead of a one-stage albedo.
    bool window_layer_kept = false;
    bool coefficient_colors_linear = false;
    bool output_policy_proven = false;
    // The dedicated shader reproduces the legacy fixed-function fog from the same draw's fog states.
    // Without it a fogged draw stays unsupported.
    bool fog_reproduced = false;
    float exposure = 1;
};
enum class Status {
    Ready, MissingProof, MissingColorPolicy, MissingOutputPolicy, UnsupportedDraw,
    InvalidView, InvalidProjection, UnsupportedMaterialSource, UnsupportedMaterial,
    InvalidLight, UnsupportedLightAmbient, UnsupportedLightSpecular, MissingLighting, InvalidConstants,
    UnsupportedBakedCombiner, InvalidFog
};
const char* StatusText(Status status);

// Clears result on every failure. Only consumes this draw's owned State and explicit proof.
// Opaque, lit fixed-function subset; no cutout/prelit/program/vertex-blend substitution, and fog only
// with fog_reproduced (then packed from the same State).
// Original API range/attenuation remain in original coordinate units under a proper rigid VIEW.
Status Build(const State& state,const Proof& proof,pipeline::OutputPolicy output,pipeline::SceneConstants& result);

// LIGHTING-off fixed-world subset: actual per-vertex RGB is carried by the dedicated mesh/shader.
// No material, global ambient or D3DLIGHT shadow is consumed and no light is synthesized. Only the
// exact one-stage texture*diffuse cascade is accepted. Unknown/programmed/blended cases, and fog without
// fog_reproduced, fail closed. The backend still proves the actual texture/UV/sampler, geometry, output and pack identity.
Status BuildBakedVertexDiffuse(const State& state,const BakedProof& proof,pipeline::OutputPolicy output,
                             pipeline::SceneConstants& result);

} // namespace gpu::pbr::scene
