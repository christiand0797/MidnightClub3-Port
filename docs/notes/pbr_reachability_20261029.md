# Dedicated PBR path: which captured draws can reach it (2026-10-29)

Capture analysis by the Claude session (morning), then the renderer steps below (afternoon). No game launch, GPU
work or asset generation.
Tool: `tools/pbr/draw_census.py` (tests: `python -I -m unittest discover -s tools/pbr -p test_draw_census.py`).

## Result (morning, before the steps below)

The working-tree dedicated PBR path (`D3D12Backend::TryPbrDraw` → `pbr::fixed_mesh::Build` → `pbr::scene::Build` →
`pbr::device::RecordDraw`) builds and passes its tests, but **no captured draw can reach it**, with or without a
material pack. Over the 40 most recent captures (`f63857` … `f132401`, 2026-10-28 22:2x–22:52, 71,920 draws):

| gate (TryPbrDraw order) | draws still passing |
| --- | --- |
| fixed transform (`mul(ip, wvp)` translation, not program/pretransformed) | 60,295 |
| triangle list (prim 5) | 12 |
| authored normal (register 2 / FVF normal) | 0 |
| fixed-function lighting enabled | 0 |

Lifting the strip and lighting gates (`--skip triangle_list,ff_lighting`) still leaves only **856** draws with an
authored normal (one class, `B01056E1` / `vs_3AED0D1D06D8298E`, `v0,v2,v3,v9`), and all 856 are alpha-blended.

## Why: the original world data

| class (handle / translation) | draws | registers | notes |
| --- | --- | --- | --- |
| `00000142` / `vs_0B41DE119782320C` | 34,993 | v0, v3, v9 | FVF XYZ+DIFFUSE+TEX1 |
| `B0105721` / `vs_DC8ED78AF8EAA640` | 5,300 | v0, v3, v9 | declaration-only fixed function |
| `B0105741` / `vs_7BED675A05C8D468` | 4,484 | v0, v3, v9, v10 | two UV sets |
| `B0105701` / `vs_147B1CA80D4C51C4`, `vs_0193CDE3AD6B5094` | 7,059 | v0, v3, v9, v10 | two UV sets |
| `B01056E1` / `vs_3AED0D1D06D8298E` | 830 | v0, v2 (NORMPACKED3), v3, v9 | blended |

- City/world draws are **triangle strips or fans** (all captured draws: 57,323 prim 6, 14,077 prim 7, 429 lists),
  so `a.strip` and `Topology::Triangles` reject them first.
- World lighting is **baked into the vertex colour** (v3 → oD0) with fixed-function lighting off. The only lit
  draws (5 per frame) are FVF `0x142` with no normal (`lighting colorvertex`).
- The world streams carry **no normal at all**. `FixedVertexHlsl` still computes `vn` from the inline
  `vattr[2]`, which is not a surface normal.
- All of these translations apply fog (`fogfactor`). Fog-enabled draws are excluded too, but frame.txt does not
  record the fog render state, so the census does not count that gate.

## What the next step has to provide (PBR_PLAN T1)

To reach city surfaces, the dedicated path needs, in this order:

1. **Strip expansion — done 2026-10-29.** `fixed_mesh::Build` now expands strips with the rasterizer's parity
   rule and drops index-degenerate joins without shifting parity (fans already arrive rebuilt as lists by
   `UseIndexList`); `TryPbrDraw` no longer rejects `a.strip`. Test: `pbr_fixed_mesh_*` `Strips()`. This passes the
   60,283 fixed-transform strip/fan draws on to the next gate; it activates nothing by itself.
2. **Face normals — done 2026-10-29 (afternoon).** `FixedProof::face_normals` (exclusive with
   `authored_normals`, and refused when register 2 IS present) packs area-weighted vertex normals only to
   orient/interpolate; `GeometryConstants::options[1] = NormalSource::Face` makes the pixel shader shade with
   `PbrGeoFaceNormal(ddx, ddy)` turned toward the viewer. `ValidateMesh(..., NormalSource::Face)` checks each
   triangle's own plane for the cotangent frame instead of vertex-normal agreement. Exactly zero-area triangles
   (coincident/collinear corners, which cover no pixel in the original) are now dropped rather than rejecting the
   draw. The device revalidates with the same source.
3. **Baked vertex-diffuse mode — done.** Codex's `scene::BuildBakedVertexDiffuse` (8:06 AM, its proof/combiner
   guards unchanged) now has its pipeline half: `pipeline::BuildBakedScene` packs `info = {0, policy, 1, 0}` with no
   lights or irradiance; `pipeline::Vertex` grew `color` (48 bytes, `COLOR0` R32G32B32A32 at offset 32), filled from
   register 3 under `FixedProof::vertex_diffuse` (white otherwise). The pixel shader's baked branch is
   `saturate(albedo) * saturate(ao) * srgb_to_linear(vertex_rgb)`, then saturate + sRGB encode (no Reinhard), so
   neutral maps reproduce the original `texture * diffuse` product: the coefficient is display-referred like the
   texture. It has no direction, so metallic/roughness/normal maps do not change it; albedo, AO, emission and
   parallax do. `TryPbrDraw` takes this branch when `D3DRS_LIGHTING` is off: specular is proven zero only when
   register 4 is absent (FixedVertexHlsl: lighting off writes `oD1 = v4`, else 0).
4. **Fog — done.** `SceneConstants` gained `fog` (start, end, density, table mode) and `fog_color` (704 bytes); both
   scene builders accept `D3DRS_FOGENABLE` only with the new `fog_reproduced` proof and pack it like the
   fixed-function half of `FillFogConstants` (`InvalidFog` for a zero linear range, NaN density or unknown mode;
   table mode 0 packs nothing, as the legacy factor is 1). The vertex shader evaluates the legacy
   `fogfactor(abs(cp.z))` per vertex on the same composed clip position, and the pixel shader blends
   `lerp(fog_color, colour, saturate(fog))` after encoding, as the legacy combiner does. Root parameter b2 is now
   visible to all stages for this.
5. **Two-stage world reflection draws** (`t0 * v0 + t1 * tfactor * t0.a`, ps `B01020E0`) rather than only the
   single-albedo combiner, if reflective world surfaces are in scope.

With 1–4 in place, the same 40 captures with `--skip triangle_list,authored_normal,ff_lighting` leave **8,824
candidate draws** (about 220 per frame; mostly `B0105721`, `B0105701`, `B0105741`, `B0105731`). Of the rest,
31,280 are blended and 20,191 use more than one texture stage (most of the `00000142` FVF class). Capture text
cannot show the remaining runtime gates: the exact `texture * diffuse` combiner, constant albedo alpha, and an
approved `materials-v1.toml` assignment for that texture/shader pair.

None of this has run on a GPU yet: the shader changes compile and reflect in `pbr_pipeline_test`, but no frame has
been drawn through the dedicated path. The next check is native, with an approved pack for one of the candidate
classes, comparing a neutral-map material against the legacy draw.

## Test pack (2026-10-29, evening)

Exact assignment keys only exist at runtime: the 10-28 captures' `ps_D2015F79...` came from the since-quarantined
companion-map `PixelKey::pbr`, so today's pixel keys differ, and captures recorded no texture hashes. Frame
captures now write each draw's `keys vs <vkey> ps <pkey>` subline and each texture stage's `hash <content hash>
tci <coordinate index>` (appended, so older parsers still match). `tools/pbr/test_pack.py` turns one such capture
into a `materials-v1` pack (tests: `py -3.12 -I -m unittest discover -s tools/pbr -p test_test_pack.py`): albedo is
a byte copy of the accepted colour (HD replacement when the texture pack is on, else the console source), flat
normal, ORM 1/0.5/0, optionally tinted 40% magenta (`--style marked`); `--install` writes the `[pbr]` settings.
`Check-PbrMaterials` accepts a pack it generated from real HD textures (3 materials, ready) and rejects it under a
wrong digest.

### First native run (2026-10-29, ~16:45, title-screen city)

A pack from an F4 capture of the title screen (73 materials) now draws **69 materials through the dedicated
path**; `pbr-test-marked` shows them magenta, and `pbr-test-neutral` against PBR off at the same menu view differs
by a mean of 0.6–2/255 (99th percentile <= 8) on the replaced surfaces. Getting there needed four corrections,
each exact against the legacy shader, all with tests:

- **Stage 1 texture factor.** World draws have a texture-less second stage `CURRENT * TFACTOR` (a uniform tint;
  here `0x00FFFFFF`). `SimpleAlbedoSurface` and `BakedCombiner` accept exactly that stage; the backend multiplies
  the packed diffuse by the factor (`FixedProof::diffuse_factor`), which equals the original because every factor
  is in [0,1]. A lit draw with such a stage stays legacy. Stage 0 reading `CURRENT` is accepted as the diffuse.
- **Ended cascade.** `MakePixelKey` leaves the stage that ends the cascade at op 0; both checks required op 1.
- **Alpha test ALWAYS.** The world enables alpha test with `0x207` (ALWAYS). An enabled test the legacy `Cmp`
  maps to ALWAYS now passes the gates and scene builders; any other enabled test still blocks.
- **Output alpha.** That factor's alpha is 0, so the original writes alpha 0. The backend packs the cascade's
  alpha factors into the vertex alpha (`alpha_from_diffuse`, factor alpha; `BakedProof::combiner_alpha_packed`)
  and the baked shader writes `albedo.a * vertex alpha`.

`TryPbrDraw` now logs, once per material and reason, the exact check that stops it (`pbr: material N texture H:
...`, with combiner/blend/alpha/mesh detail).

**Mesh validation (same evening): 72 of 73 draw.** The 21 rejections were a few triangles per draw with collinear
UVs (texture smeared along a line, e.g. trims) or near-zero-area slivers, which the original rasterizes.
`ValidateMesh` now allows triangles without a tangent frame when at least one triangle has one (authored normals
must still agree); the shaders already skip normal mapping/parallax where no frame forms, and a face-normal sliver
now keeps the interpolated normal instead of shading black. `ValidateMesh(..., why)` names the first failing rule
and counts failing triangles; `fixed_mesh::Build(..., detail)` passes it to the log. What still stays legacy here is
genuinely blended or alpha-tested, and the 31 textures with varying alpha are not packable yet.

## Whole cities (2026-10-30)

Capture-built packs only cover what was photographed. The city pack needs no capture: materials name the texture
alone (`vertex_shader = "any"`, `pixel_shader = "any"`, `uv_set = "any"`; all draw-state gates still run on the actual
draw and the mesh uses the draw's own coordinate index) and take `albedo = "game"`, so the device binds the draw's own
texture as t0 (per-material SRV table appended when that resource changes, retained per frame) and the shader decodes
its gamma (`GeometryConstants::options[2]`). That also removes the varying-alpha limit: coverage is the original's.
`tools/pbr/test_pack.py --cities` writes one material per texture of the four cities in `source_index.json` that has
generated maps (ORM 256, height 128, a shared flat normal: baked lighting has no direction for a normal map). Varying
alpha is also accepted for copied albedos where the written alpha is provably zero (TFACTOR alpha 0). In Tokyo:
~260-300 draws through the path at ~60 fps; the rest are blended (~120), multi-texture or programmable-pixel (~85),
alpha-tested (~28) or vertex programs (~21), which stay legacy by design.

Final (all 3,259 city textures with maps, native, PBR on): San Diego 211 draws / 3.3 ms, Atlanta 464 / 7.6 ms,
Tokyo 346 / 6.5 ms, all at a steady 60 fps.
