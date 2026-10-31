# PBR system development

**Status 2026-10-31:** the dedicated renderer path draws baked city scenery in game (opt-in, PBR tab). A whole-city
pack (`tools/pbr/test_pack.py --cities`) covers every Detroit/San Diego/Atlanta/Tokyo texture with the generated
normal, AO/roughness and height maps (`city_map_sizes`: roads and ground 512/256/128, everything else its own size up
to 128) and samples the game's own texture as albedo. In baked mode the light has no direction, so the shader takes it
as falling from above the surface toward the viewer: the normal map shades relative to the surface's own normal
(relief, exactly 1 for a flat map) and a GGX lobe of the same light adds gloss scaled by 1 - roughness (none at 1). Cutouts (GREATER/GEQUAL alpha tests,
SRC_ALPHA/INV_SRC_ALPHA or ONE/ZERO blending) keep their coverage: same clip on the same packed alpha, the draw's own
blend state (`KeepsLegacyBlend`). Facades with lit night windows draw their stage-1 window texture as a layer on a
second UV set, added in display values after the PBR surface is encoded (`WindowLayerSurface`, `options[3]`). How draws reach the path, the gates and the
remaining legacy cases are in [pbr_reachability_20261029.md](pbr_reachability_20261029.md). The sections below are the
original design record.

The material system and shared shading math are implemented and CPU-tested.

## Implemented

`runtime/gpu/pbr_material.*` loads explicit `materials-v1.toml` libraries with an independently
supplied manifest SHA256. A material names an original texture and, either exact
or `"any"`, its vertex/pixel shader and UV set; `albedo = "game"` uses the game's accepted colour texture instead of a
pack image (no alpha proof needed, gamma decoded in the shader). Each assignment names an original texture, vertex and pixel shader,
texture stage, UV set, and world/vehicle surface. UI, movies, screen-space effects, unknown
geometry, blended/dynamic textures and render targets do not qualify for material replacement.

The importer validates each PNG's digest and format, bounds dimensions and total mip memory,
and keeps albedo/emission in sRGB while normal, ORM and 16-bit height data stay linear. ORM uses
R=ambient occlusion, G=perceptual roughness and B=metallic. Mips filter color in linear light,
renormalize normals and filter squared roughness. Original accepted level-zero alpha bytes and
dimensions must match a separate digest; reflectivity alpha is not silently used as metallic.
This is exact level-zero alpha preservation, not a claim of identical coverage at every mip.

The shared CPU/HLSL BRDF implements GGX distribution, separable Smith visibility, Schlick
Fresnel, dielectric/metallic energy sharing and Lambert diffuse. AO affects indirect diffuse,
not direct lights. Tangent-normal conventions are explicit. Degenerate bases have finite zero
fallbacks. The tests exercise linear mips, normal orientation, material/draw rejection,
malformed inputs, memory budgets, alpha preservation, deterministic white-furnace integration
and actual HLSL compilation without starting a game or GPU worker.

`Check-PbrMaterials` is a CPU preflight tool built explicitly from the game CMake project:

```text
cmake --build BUILD --target Check-PbrMaterials
Check-PbrMaterials --root PACK --manifest-sha256 APPROVED_SHA256
```

It outputs JSON and never activates a renderer or generates textures. Exit 0 means the pack
passed import validation; 2 means invalid arguments; 3 means the pack was rejected. Successful
import alone does not prove a native material assignment or visual quality.

## Renderer integration still required

The [geometry math module](pbr_geometry_math_20261028.md) now implements row-vector
model/view composition, inverse-transpose normals, explicit face orientation and guarded
cotangent frames, with CPU/HLSL helpers and reflected shader packing tests. It remains
separate from actual draw capture and shader/material activation. Legacy shader sources,
root signatures and rendering defaults are unchanged.

[The source-backed geometry bridge investigation](pbr_geometry_bridge_20261028.md) identifies
the first bounded fixed-function scope, actual morph/skinning snapshot points and missing
post-extension capture evidence. These are implementation inputs, not renderer acceptance.

The existing translated vertex output carries clip/screen position, colors, UVs and fog. It
does not yet carry independently established world position, geometric normal and tangent
basis for every supported surface. The existing root signature also has only the original
four texture/sampler stages and legacy constant buffers. A final-color filter cannot recover
those material and lighting inputs reliably.

The next path must therefore implement and validate:

1. Shader-specific geometry bridges, retaining the original rasterized position and using
   proven model/view transforms for normals and positions. Skinning and static world meshes
   need separate mappings. Unknown draws keep their original shader path.
2. A dedicated material root signature/PSO variant, uploaded normal/ORM/optional maps and
   explicit per-draw bindings. Original texture stages and effects must retain their meanings.
3. Proven camera and original scene-light inputs, linear HDR accumulation, environment
   lighting/reflection prefiltering and appropriate tone mapping. Car paint/clearcoat and
   surface-specific treatments follow real material evidence.
4. Bounded native A/B captures for vehicles, roads, buildings and cutouts, plus fallback,
   transparency, day/night, animation and memory/performance validation.

Real asset conversion follows the completed strict media pass and an all-file verified full
project backup. The accepted texture pack, original sources, saves and frozen release cohorts
remain the recovery baseline. New PBR materials require their own source/model provenance
and visual review; generated detail is not treated as recovered original detail.
