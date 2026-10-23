# PBR material conversion — analysis and plan

Written 2026-10-21. Status: **offline phase P0 done** (tooling + a 54-texture starter set); the runtime is untouched and
the game renders exactly as before. Everything runtime-side below is a plan, gated behind a setting that defaults to off.

Contents: 0 guarantees · 1 how MC3 renders surfaces today · 2 what "PBR" can mean here · 3 material maps and storage ·
4 classifying materials · 5 authoring the maps (tools evaluated) · 6 runtime design · 7 performance budget ·
8 phases · 9 risks · 10 test plan · 11 what was done in P0 (commands, results).

---

## 0. Guarantees (the user's condition: "I want it to work how it does for now")

1. Default rendering is byte-for-byte the current path. A new `[graphics] pbr = false` setting gates every runtime
   change; with it off the renderer does not look for material maps, does not build any new pipeline, root signature or
   shader variant, and does not change any existing shader text, `PixelKey`, `PsoKey`, `DrawConstants` or descriptor
   use. Test 10.1 proves it (identical frame-capture shader hashes and identical pixels with the setting off).
2. Material maps live in their own folder `textures/pbr/`, never in `textures/replace/`. This matters: `texture_pack.cpp
   Index()` walks `replace/` **recursively** and accepts any file whose stem *starts* with 16 hex digits, so a
   `<hash>_n.png` placed there would be loaded as the colour replacement of `<hash>`.
3. `DrawConstants` is not extended. PBR data goes in a separate cbuffer (`b2`) used only by PBR pipelines (6.4), so the
   C++/HLSL layout rule that broke the title once (HANDOFF §3, §9) is never at risk.

---

## 1. How MC3 renders surfaces today

Evidence: race frame captures `templates/game/out/native/captures/211022` (1,235 draws), `204913`, `014417`; their
translated HLSL (`vs_*.hlsl`, `ps_*.hlsl`); `source_index.json`; tune files in `assets_x.dat`.

### 1.1 Draw census of a race frame (211022)

| Share | Vertex shader | Pixel side | What it is |
|---|---|---|---|
| ~45% | `B0102421` / `B0102441` (FF, lighting off) | fixed-function stages | city geometry, props: prelit |
| ~15% | `00000142` / `00000144` / `000001C4` | FF, additive / alpha | sprites, particles, glow cards, post quads |
| ~12% | `B0102451` / `B0102461` | FF, alpha blend | alpha-blended world (foliage, decals, sky) |
| ~5% | `B0101FC1`..`B0101FF1` (vertex programs) | combiners `B0102220`/`240`/`250`/`260` or FF | the player car and traffic |
| rest | `B01019A1`, skinning program, quads | mixed | car shadow/ground pass, peds, HUD, post chain |

(Handles are addresses and may move between runs; the runtime keys shaders by program hash, so a PBR classifier must
key on `DrawShaders::vkey`/`pkey`, not on handles.)

### 1.2 City / world: lighting is baked into vertex colours

- Vertex layout (draw 0060 etc.): stream 0 stride 28 = position float3 + uv0 float2 + uv1 float2; **stream 1 stride 4 =
  a D3DCOLOR per vertex**. The translated vertex shader (`vs_C029509767C46FD8.hlsl`) is `oD0 = v3; oT0 = v9; oFog =
  vattr[5]`: no lighting, **no normals in the vertex data**. All world shading is the prelit colour stream.
- Each city ships one bundle per time of day × weather (`<city>_{dawn,dusk,midnight}_{clear,cloudy,rainy}.xbck`,
  ~24 MB each). Their textures are almost all shared: of the city textures in `source_index.json` 2,697 are first seen
  in `dawn_clear`, only 30-130 in each other variant (sky, wet and night variants). So **textures are close to
  un-lit albedo already**; light, time of day and weather live in the vertex colours, `tfactor` and fog. De-lighting
  the albedo (a big problem in most PBR conversions) is a small one here.
- Single-stage world draws: `cur = v0 * t0` (diffuse colour times prelit colour), fog, alpha test.
- **Two-stage reflective world draws** (`ps_8A2D4F28CF001C14.hlsl`, ~100 draws a frame):
  `out = t0.rgb * v0.rgb + t1.rgb * tfactor.rgb * t0.a` — t0 is the DXT5 diffuse, t1 a small DXT1 reflection texture
  (64², 128×64, 256²) on the second UV set, `tfactor` the time-of-day reflection strength. **The DXT5 alpha of world
  textures is the artists' reflectivity mask**: window panes are white, asphalt carries a sparse speckle (glints), most
  walls have none. This is the single most useful authored material signal in the game (roughness/F0 hint, 4.2).
  Other DXT5 world textures use alpha as coverage (foliage, fences) and some have dense or uniform alpha whose role
  the runtime census must decide (4.3).
- Night: lit windows are separate dark textures with bright panes (e.g. `6FFF2CC720A1FA3D`): emissive candidates.
- Street lights are baked into the night vertex colours plus glow sprites; there are no runtime point lights.

### 1.3 Cars: per-vertex lighting, painted colour, a live reflection map

- Vertex program `vs_1CB4A27574EC95F3.hlsl` (handle `B0101FC1`): packed normal `v2` (NORMPACKED3), **three
  directional lights** (directions `c[108..110]`, colours `c[96..98]` in the HLSL's register numbering, ambient
  `c[99]`), a damage/morph offset (`v3`, `c[116]`), fresnel and highlight terms written to `oD0.a`/texcoords.
- Pixel combiner (`ps_BD42D2F3FB43F290.hlsl`, handle `B0102220`): `r0 = t0 * c0` (c0 = paint colour), `r0 *= v0 * 2`
  (lit), `r1.a = t1.a * v0.a` (fresnel from the vertex shader), `out = lerp(r0, t1, r1.a)` — **t1 is a 256² A8R8G8B8
  render target (`83004E80`) re-rendered every frame** (draws 0659-0667 render the scene into it): a dynamic
  environment map. Variants `B0102240/250/260` handle other car materials (glass, chrome, alpha).
- Paint parameters come from `tune/vehicle/customdata/<car>/*.mccarcustom`, e.g. vp_charger_69 default:
  `m_paintColor1 0.5 0.25 0.066`, `m_paintType 4`, `m_metallicPaintHilightScale 1.25`,
  `m_carpaintFresnelParams 0.8 0.275 0.725 1.0`, `m_highlightSize 0.5`, `m_highlightAlpha 1`, `m_additivePaint 1`,
  `m_numGradients`/`m_color0..7` (pearl/two-tone gradient), `m_windowTintColor`, `m_caliperColor`. The specular
  highlight is a texture (`vehicle/shared_texture/fx_specular_highlight.tex`), not a BRDF.
- Car textures are **atlases** (one 256² DXT5 per car, `resources/vehicle/vp_*/vp_*.xbck`, plus shared parts under
  `vehicle/shared_texture`: grills, exhausts, radiators, tail-light lenses, carbon fibre, damaged glass), wheels in
  paged pools (`rim.xbpf` 672, `tire.xbpf` 128, `brake.xbpf` 38). The body itself is mostly a white/grey base that
  the paint colour multiplies. Vinyls (`decal*.xbpf`, 6,071) are palette masks the game tints: not surfaces.

### 1.4 Peds, effects, post

Peds: skinning program `vs_48C366892849F012` (bones in constants). Effects: additive sprites. Post (race): headlight
pools drawn into a half-size buffer and added, glow (downsample + add), speed blur (alpha over), all recognised by
`D3D12Backend::ScaleScreenEffect`. **Headlights are screen-space pools, not lights**: no analytic light exists for them.

### 1.5 Light sources available to a PBR path

| Light | Source | Notes |
|---|---|---|
| key/fill lights | car program constants `c[96..99]`, `c[108..110]` | 3 directional + ambient, in the car's space; transform with its world matrix (or capture the setter) |
| sun / moon direction for the world | none at run time | author per city × time of day (`textures/pbr/lights.json`), checked against the car rig and the sky |
| irradiance | the prelit vertex colour | keep it: it *is* the game's lighting (GI, street lights, time of day) |
| specular environment | world: the t1 reflection textures; cars: the live 256² target | prefilter into a mip chain for roughness |
| headlights | the pools pass | optional later: analytic spotlights from the player car's matrix |

---

## 2. What "PBR" can realistically mean here

A full physically based relight of MC3 is not possible without re-lighting the city (no normals, no lights at run time,
GI baked per vertex). What is possible, in tiers, each a strict superset of the previous and each opt-in:

- **T0 — offline maps (done for 54 textures):** normal, height, roughness, metalness, AO per texture hash.
- **T1 — material-aware console lighting (world):** keep `v0` (baked light) as diffuse irradiance and the game's
  combiners; add (a) normal-mapped modulation of the baked light by an authored key-light direction:
  `diffuse = v0 * clamp(lerp(1, NdotL_tex / max(NdotL_geo, 0.2), k), 0.6, 1.4)`, (b) AO on the irradiance,
  (c) roughness-aware reflections: the reflection term `t1 * tfactor * mask` becomes
  `prefiltered(t1, roughness) * F_schlick(F0, NdotV) * specular_occlusion`, with the mask still scaling it, and
  (d) a GGX key-light highlight scaled by the time-of-day `tfactor`. Geometric normal: from screen-space derivatives
  of the reconstructed view position (6.5) — city geometry is planar, so face normals are right.
- **T2 — car materials:** paint as a two-layer clearcoat (base = paint colour × atlas, metallic flakes from
  `metallicPaintHilightScale`/`paintType`, clearcoat roughness ~0.04), chrome/rims with metalness maps, glass, tyre
  rubber; lights from the car rig constants; reflections from the live 256² target prefiltered per frame. The game's
  `carpaintFresnelParams` keep the look in the same family.
- **T3 — later, optional:** de-lit albedo (only where textures carry baked shading: car atlases, signs), parallax
  occlusion from `_h`, emissive night windows driven by time of day, analytic headlight spots.

---

## 3. Material maps and storage

Per texture hash (the runtime's `ContentHash`, the same name as `source/` and `replace/`), in
`templates/game/out/native/textures/pbr/`:

| File | Content | Format |
|---|---|---|
| `<hash>_n.png` | tangent-space normal, **OpenGL / glTF convention** (+X right, +Y up the image, +Z out) | RGB8 |
| `<hash>_h.png` | height, 0.5 = mean surface, high-passed (~24 original texels) | 16-bit grey |
| `<hash>_r.png` | perceptual roughness (0 mirror .. 1 rough) | 8-bit grey |
| `<hash>_m.png` | metalness, only for classes that can be bare metal | 8-bit grey |
| `<hash>_ao.png` | ambient occlusion from the height field (1 = open) | 8-bit grey |
| `manifest.json` | per hash: class, input (replace/source), size, original size/format/location, maps, method, settings, stats | |
| `classes.json` | first-pass class of every source texture (classify.py) | |
| `_preview/` | contact sheets, method comparisons | |

**Input = the console's own texture** (`textures/source/<hash>.png`), Lanczos-enlarged 4x (the HD pack's size) and
capped at 1024 on the long side. Not the HD pack: `textures/replace` is regenerated from time to time (it was just
redone with UltraSharp, which adds grain), and an upscaler's invented grain would turn into normal-map noise. Maps share
UVs with whatever albedo the pack shows, so their resolution need not match it. (`--input replace` remains available.)
UV convention: D3D texture v runs down the image, so the shader builds `B = dP/dv` pointing down the image and uses
`n = T*x - B*y + N*z` for these OpenGL-convention maps (DeepBump is OpenGL-convention; PBRify is DirectX and is flipped
on output; both were checked against the classical estimate's sign).

**Runtime format (phase P2):** convert the PNGs to two DDS files per hash: `<hash>_n.dds` BC5 (RG, z rebuilt) and
`<hash>_orm.dds` BC7 (R AO, G roughness, B metalness, A height), full mip chains with normals renormalised per level
and roughness widened by normal variance (Toksvig-style) so distant normal detail turns into roughness instead of
sparkle. The PNG set stays the editable master.

---

## 4. Classifying materials

### 4.1 Offline, by origin (exact)

`tools/pbr/classify.py` → `pbr/classes.json`. Rules from the `source_index.json` location: `rim/tire/brake/rider.xbpf`
→ rim / tire / brake / rider; `vehicle/shared_texture/*` by name (carbon, grill|radiator, exh_, tl1elgte|signal|fog
lights, window, chrm, …); `resources/vehicle/vp_*` → car_atlas; `vehicle/va_*` → traffic_atlas; flash/XPR/fonts → ui;
`decal*.xbpf` vinyls and fx → skip; cutscene/peds → rider (cloth/skin).

### 4.2 Offline, by image statistics (city bundles carry no texture names)

Alpha use (coverage vs sparse mask vs dense), saturation, brightness, green fraction, dark-with-bright-panes (night
windows), tileability (edge continuity) and size give a low-confidence class (road_asphalt, concrete, facade,
signage, foliage, terrain, windows_night). Marked `"confidence": "low"`. The class only selects a prior; the per-pixel
maps come from the image either way, so a wrong class costs accuracy of roughness/metalness level, not artefacts.

The DXT5 alpha is used as a **gloss mask** (roughness pulled towards the class's `gloss_rough`) only when it looks like
one: structured (std > 0.06), covering under 70% and reaching below 0.5. Dense/uniform alpha is ignored and recorded as
such in the manifest (example: `6F9FD0667A447E65` brick, alpha a flat ~0.62).

### 4.3 At run time: a material census (planned, phase P1b, opt-in debug setting)

The decisive facts about a texture are in its draws, which only the runtime sees. A census mode (setting
`graphics.pbr_census`, off by default, or a frame capture flag) appends per texture hash, once per distinct use:
vertex shader key and kind (FF prelit / FF lit / program hash), pixel key (FF ops or combiner hash), the stage the
texture is in, whether stage 0 alpha feeds a later stage (the reflection combiner) or the alpha test/blend, blend
mode, cull, and the average screen-space geometric normal "up-ness" (road vs wall) from a 1-in-N sampled draw. Output
`textures/pbr/census.jsonl`, merged offline into `classes.json` with `"confidence": "census"`. A few minutes of the
user's normal play in each city covers most of the textures that matter.

### 4.4 Draw classification for shading (run time)

Per `(vkey, pkey, stage setup)` cached once: world-prelit (FF lighting off + vertex colour), world-reflective (the
two-stage `t0.a` combiner), car-paint (car program + `B0102220`-family combiner sampling the 256² target), car-part,
ped, sprite/particle/post/UI (never PBR: screen quads, additive blends, `kTexTarget`/`kTexWide` reads, pretransformed
vertices). Unknown → untouched.

### 4.5 Class priors (tools/pbr/pbrlib.py `MATERIALS`)

| Class | rough | metal | notes |
|---|---|---|---|
| road_asphalt / marking / concrete | 0.85-0.88 | 0 | gloss mask → 0.35-0.40 (wet sparkle) |
| pavement, curb | 0.80-0.82 | 0 | strong normals (joints), AO 0.8 |
| terrain / foliage | 0.92 / 0.75 | 0 | foliage alpha = coverage |
| brick, concrete, facade | 0.80-0.88 | 0 | windows in the mask → 0.05-0.15 |
| storefront, windows | 0.60-0.65 | 0 | mask → glass 0.05-0.08 |
| metal_painted / metal_bare | 0.55 / 0.40 | 0 / 1 | |
| signage | 0.55 | 0 | weak normals |
| car_atlas, traffic_atlas | 0.45 | per pixel (chrome detection) | lenses, chrome, rubber mixed |
| car_glass, car_light | 0.06 / 0.12 | 0 | |
| rim, chrome, exhaust, grill, brake | 0.12-0.45 | per pixel | alpha = coverage on rims/brakes |
| tire, carbon, rider | 0.85 / 0.30 / 0.70 | 0 | |

Per-pixel roughness = prior + variation from local detail (grain), cavities and the network's own roughness
*variation* (PBRify's absolute roughness is unreliable on these textures: a black tyre reads 0.2), pulled down where
metal, and by the gloss mask. Metalness per pixel for chrome-capable classes = bright × unsaturated × contrasty.

---

## 5. Authoring the maps: tools evaluated (RTX 3050 8 GB, shared with a running Real-ESRGAN job)

| Tool | What | Runs here? | Verdict |
|---|---|---|---|
| **Classical** (this repo) | band-pass height from luminance (6 DoG bands, DXT pre-blur, wrap), Sobel normals, local-variance roughness, Frankot-Chellappa height, multi-radius cavity AO | CPU, 0.1-2 s / 1024² | baseline and fine-detail source; always available |
| **DeepBump** (HugoTini, `deepbump256.onnx`, 27 MB, GPL-3 model) | colour → normals U-Net, 256² tiles | onnxruntime CPU ~1 s / 1024² (DirectML ~8 ms/tile) | used, weight 0.3: good broad shapes, but alone it reads dirt and stains as large bumps (high-passed at 16 texels to remove that) |
| **PBRify_Remix 1.7.2** (Kim2091, CC0, SPAN 1x, trained on ambientCG/PolyHaven for RTX Remix) | albedo → normal (DirectX), roughness, height | torch CPU + spandrel, ~1.2 s / 512² per net (~10 s / 1024² for normal + roughness) | used, weight 0.7: cleanest, architecture-aware normals (bricks, pavers, grates); roughness only as relative variation |
| Marigold-Normals / StableNormal (SD2-based) | scene-geometry normals from photos | ~3-5 GB VRAM fp16, seconds per image | not used: estimates the *scene* shape; a flat texture comes out as a tilted plane with little relief. Not worth GPU time next to the upscaler |
| Marigold-IID appearance (SD2-based) | albedo / roughness / metallicity from photos | ~4-5 GB fp16 at 768² | candidate for T3 de-lighting of car atlases and signs only; evaluate when the GPU is free |
| RGB↔X (Adobe research, SD-based) | intrinsic decomposition | heavy, research-only licence | not used |
| Diffusion SVBRDF estimators (e.g. Ubisoft CHORD 2025, MatFusion) | full SVBRDF from a texture | 8 GB+, research licences | watch list; would replace the rule-based roughness if one becomes practical |
| Materialize, Substance Sampler, PBRFusion (SDXL) | interactive / commercial / heavy | n/a | manual tools for hero textures (player cars) later |

Default method `ensemble`: `normalize(slopes(0.3 · DeepBump_hp + 0.7 · PBRify_hp))` blended with the classical
fine bands by the class's `deep` weight; learned slopes are rescaled to the classical median slope so the class
`normal` strength rules every method. Comparison sheets: `textures/pbr/_preview/compare_starter*.png`.

---

## 6. Runtime design (phases R1-R5; nothing implemented yet)

### 6.1 Settings

`[graphics]`: `pbr = false` (master), `pbr_normals = 1.0` (0..2), `pbr_specular = 1.0` (0..2), `pbr_ao = 1.0`,
`pbr_cars = true`, `pbr_world = true`, `pbr_debug = 0` (1 normals, 2 roughness, 3 metal, 4 AO, 5 draw class),
`pbr_census = false`. F1 Quality tab: "PBR materials (experimental)" plus strengths. Toggling `pbr` invalidates the
cached textures the same way `texture_pack` does (`Tex::pack_enabled` pattern).

### 6.2 Loading (texture_pack)

- `pack::FindMaterial(hash)` beside `pack::Find`, indexing `textures/pbr/` once (stems `<16 hex>_<n|r|m|ao|h>` or the
  P2 `.dds` pair). Only called when `pbr` is on.
- `BindGuestTexture` computes `name` today only when replacing or dumping; with `pbr` on it is computed too (same hash).
- **Decode off the render thread**: today `pack::Find` decodes PNGs synchronously in the draw. Material maps (2-4 per
  texture) must load on a worker; until ready the draw uses the non-PBR path (no hitch, maps pop in). P2 DDS files
  remove decode cost entirely (memcpy into the upload buffer).
- `Tex` gains `ComPtr<ID3D12Resource> nrm, orm;` (BC5/BC7 or BGRA8 from PNG) and a `material` byte (class id from the
  manifest). Freed with the texture.

### 6.3 Descriptors and root signature

Current: one root signature, `t0-t3` (descriptor table, 4 SRVs) + `s0-s3`, CBVs `b0` (vertex constants), `b1`
(`DrawConstants`). PBR adds a **second root signature used only by PBR pipelines**: the same four parameters in the
same order plus `[4]` a descriptor table of 8 SRVs `t4-t11` (normal + ORM for each stage, null views when absent),
`[5]` CBV `b2` (`PbrConstants`), `[6]` a table for the prefiltered environment (`t12`) and a static linear-mip sampler
`s4`. The SRV heap (`kSrvCount = 98,304`, a third per frame) has room: +8 per PBR draw.

### 6.4 Shaders

- `PixelKey` gets `uint8_t pbr[4]` (material mode per stage) and `uint8_t pbr_class`. These are zero unless the
  setting is on and the draw qualifies; `operator==` and the key hash must include them, and with all zeros the
  generated HLSL and the key's hash must be **identical to today's** (add the fields at the end, hash them only when
  non-zero — test 10.1).
- `PixelHlsl`: when `k.pbr_class != 0`, declare `Texture2D nrm{i} : register(t{4+2i}); Texture2D orm{i} :
  register(t{5+2i});` and `cbuffer PC : register(b2) { ... }`, sample them with the stage's own UV and sampler, and
  replace the final colour expression per class (T1 world, T2 car) — the FF/combiner code above it stays as generated,
  so the PBR result starts from exactly the console colour.
- `PbrConstants` (b2), C++ struct and HLSL declared from one source (a macro list or a generated string) with
  `static_assert(sizeof(PbrConstants) % 16 == 0)` and `offsetof` checks: inverse projection, view→world rotation, key
  light dir/colour, ambient, time-of-day factor, strengths, per-stage flags, env mip count. `DrawConstants` is not
  touched.
- Vertex shaders need no change: the pixel shader reconstructs view position from `SV_Position` and the inverse
  projection, and builds the geometric normal and tangent frame from derivatives (cotangent frame, Schüler 2013).
  Projection: recorded in the camera projection setter `sub_002D4AE0` (already hooked for FOV, `fov.cpp`) and handed
  to the renderer; for fixed-function draws `D3DTS_PROJECTION` is in the state anyway. Car draws can use their real
  vertex normal: add `oN` only to the PBR variant of the car program's HLSL (`VertexProgramHlsl` writes it into an
  extra `TEXCOORD6`, PBR variant only).
- PSO creation: PBR pipelines are separate `PsoKey`s (the `ps` hash differs), compiled on first use like today.

### 6.5 Shading (T1 world)

```
N_geo  = normalize(cross(ddx(Pv), ddy(Pv)))                 // view space, flat per triangle
TBN    = cotangent_frame(N_geo, Pv, uv)                     // from ddx/ddy of Pv and uv
N      = normalize(TBN * (nrm.xy * pbr_normals, z))         // OpenGL map: n = T*x - B*y + N*z (D3D v down)
irr    = v0.rgb * bentfactor(N, N_geo, keyL) * lerp(1, ao, pbr_ao)
base   = console colour before reflections (t0 * v0 path, unchanged combiner maths)
refl   = sample(env_prefiltered, reflect(V,N), lod = rough * maxmip) * F_schlick(0.04, NdotV) * mask_from_alpha_or_orm
spec   = GGX(N, V, keyL, rough) * keyColour * tfactor_luma
out    = base * (irr / v0) + (refl + spec) * pbr_specular  then fog exactly as before
```

### 6.6 Shading (T2 cars)

Base = `t0 * c0` (paint colour from the combiner's constant) × atlas albedo; clearcoat layer: F0 0.04, roughness 0.04,
reflections from the live 256² target prefiltered into 5 mips each frame (one `Context::Pass` chain at 256² — cost
negligible); base layer: metallic flakes `metal = paintType ∈ metallic types ? 0.6 : 0`, roughness 0.35, sparkle from
a noise texture scaled by `metallicPaintHilightScale`; lights = car rig `c[96..99]/c[108..110]`; keep the game's
fresnel `carpaintFresnelParams` as the clearcoat's reflection weight floor so cars keep their character. Chrome/rims:
metalness map, roughness map, same env.

### 6.7 What is never touched

Screen quads (`quad`), post passes, `kTexTarget`/`kTexWide` reads, pretransformed vertices, UI/flash, movies, the
headlight/glow/blur composites, shadow passes, the car reflection render itself (its draws stay console-exact; only
the reading of the result is upgraded).

---

## 7. Performance budget (RTX 3050 laptop-class, render scale as today)

- GPU: target ≤ +1.5 ms per frame at 1080p for T1+T2 (two extra samples per textured stage for ~600 world draws,
  ~1 env sample + GGX per pixel; prefilter chain 256² ×5 mips ≈ 0.05 ms). The 60 fps budget is 16.6 ms; the
  current cost should be measured with the perf lines in `mc3.log` before R2.
- VRAM: per 1024² texture BC5 + BC7 with mips ≈ 2.7 MB; a city keeps ~900 textures, about half at ≥512² in the
  pack → roughly 0.8-1.2 GB extra if everything had maps. Mitigation: maps at half the albedo resolution for ground
  and walls seen at distance (roughness/AO/metal are low-frequency), evict with the texture.
- Disk: PNG master set measured in 11 (≈2.7 MB per 1024² texture as PNG); the whole pack ~12k textures but ~6k are
  vinyls/UI that get no maps → est. 10-15 GB PNG, 4-6 GB as DDS.
- Load: no synchronous decode in the draw (6.2). DDS from P2.

---

## 8. Phases and milestones

| Phase | Work | Done when |
|---|---|---|
| **P0** (done) | tools/pbr: generator (classical + DeepBump + PBRify ensemble), classifier, contact sheets, method comparison; 54-texture starter set | maps + sheets reviewed |
| P1 | run classify.py on all sources; tune priors on more sets (per city); batch-generate the world + vehicle classes (~6.5k textures; ~10 s each on CPU → ~18 h, or ~3 h with PBRify exported to ONNX/DirectML when the GPU is free) | manifest covers every non-skip class |
| P1b | runtime census (4.3), opt-in; merge into classes.json | ≥90% of textures drawn in a session classified by census |
| P2 | DDS packer: BC5 normal + BC7 ORM with Toksvig roughness, half-res option | pack loads with no decode |
| R1 | runtime plumbing behind `pbr`: index, async load, second root signature, `pbr_debug` views; no shading change yet | debug view shows maps on the right surfaces; off = identical frame |
| R2 | T1 world shading | user screenshots: roads/walls read as relief, wet glints, windows reflect; no seams |
| R3 | T2 cars | paint/chrome/glass/tyres look right in garage and race |
| R4 | environment prefilter, key light table per city × time, emissive night windows | time of day consistent |
| R5 | strengths in F1, presets, performance pass | ≤1.5 ms, no hitches |
| P3 | optional: de-lit atlases, parallax from height, headlight spots | |

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Changing the console path by accident (keys, cbuffer, root signature) | separate root signature + cbuffer; PBR fields hashed only when non-zero; test 10.1 on every R-phase |
| Baked lighting in vertex colours fights a new key light (double shading, wrong sun side) | T1 only *modulates* `v0` within 0.6-1.4×; key light per city × time authored and checked against shadows in the baked colours |
| No vertex normals for the world | flat derivative normals (planar city); for curved props, accept faceting or skip their class |
| Wrong classes (city textures have no names) | class only sets priors; census at run time; curated overrides in sets/*.json |
| DXT5 alpha meaning varies per draw | gloss mask only when the reflection combiner uses it (census) |
| Seams on atlases (car textures) | atlas classes (`pbrlib.ATLAS_CLASSES`) filter with mirrored edges instead of wrapping (done in P0); filters still cross UV-island borders inside the atlas (≤ ~2 texels of bleed at 4x); P1 option: dilate islands using the albedo's alpha/background colour |
| Normal-map aliasing/sparkle at distance | mip renormalisation + Toksvig roughness (P2), anisotropic filtering already 16x |
| PNG decode stalls | async load (6.2), DDS (P2) |
| VRAM on 8 GB with the 4x pack | half-res ORM, maps only for drawn textures, evict with textures |
| Model licences | DeepBump GPL-3 and PBRify CC0 are used only as local tools; weights are not committed (`tools/pbr/.gitignore`); outputs are our data |
| GPU shared with the video upscaler | generator defaults to CPU (`--device cpu`); DirectML opt-in |

---

## 10. Test plan

1. **Off = identical** (every runtime phase): same save, same camera, F4 frame capture with `pbr=false` before and after
   the change: identical `ps_*/vs_*` file names (keys) and HLSL text, identical draw list; screenshots bit-identical
   (`game.mjs shot` diff, user-run). Plus a unit check that `PixelHlsl(k)` for a sample of captured keys is unchanged.
2. **Layout**: `static_assert`s for `PbrConstants` size/offsets; a startup self-test that reflects the compiled PBR
   pixel shader's cbuffer (`ID3D12ShaderReflection`) and compares offsets with the C++ struct, logging a hard error.
3. **Maps**: offline checks in tools (normal unit length, no NaNs, tiling seam error ≤ the albedo's own, convention
   sign check against the classical estimate, roughness in range); contact sheets reviewed per batch.
4. **Debug views** (`pbr_debug` 1-5) in each city/time of day: maps on the right surfaces and in the right orientation
   (a raised paint line must catch light from the light's side).
5. **Performance**: perf lines in `mc3.log` with pbr on/off in the same race start; budget 7.
6. **Toggling live**: on/off/on in a race and the garage; no leaks (VRAM stable), no hitches.
7. **User playtest** per phase (the user tests; agents do not drive the game).

---

## 11. P0: what was done, commands, results

New files (nothing else in the repo touched): `tools/pbr/pbrlib.py` (all algorithms), `generate.py` (batch, manifest),
`classify.py`, `contact_sheet.py`, `compare_methods.py`, `sets/starter50.json` (54 hand-picked hashes: 16 road/ground,
3 terrain, 1 foliage, 18 building, 16 vehicle), `.gitignore` (models/). Output in
`templates/game/out/native/textures/pbr/` (inside `out/`, already git-ignored).

Dependencies installed: `onnxruntime-directml 1.24.4` (CPU + DirectML providers), `spandrel` (SPAN architecture
loader; torch 2.10 CPU was already present). Models in `tools/pbr/models/` (`--download-models` fetches and checks
SHA-256): `deepbump256.onnx`, `pbrify/1x-PBRify_{NormalV3,RoughnessV2,Height}.pth` (+ licence/readme).

```
py -3.12 tools/pbr/generate.py --download-models                     # once
py -3.12 tools/pbr/generate.py --set tools/pbr/sets/starter50.json   # maps for the starter set (incremental; --force)
py -3.12 tools/pbr/contact_sheet.py --set tools/pbr/sets/starter50.json
py -3.12 tools/pbr/compare_methods.py --set tools/pbr/sets/starter50.json --pick 1 12 20 48
py -3.12 tools/pbr/classify.py                                       # classes.json for all sources
py -3.12 tools/pbr/generate.py --classes --only brick concrete --limit 100
```

RESULTS_PLACEHOLDER
