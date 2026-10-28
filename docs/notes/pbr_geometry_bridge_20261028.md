# PBR geometry bridge: source evidence and next scope

Recorded 2026-10-28 against repository HEAD
`12c4d938ad3db6796ab6cb788957c7a079eac2ff` and the then-current working tree.
Line anchors below describe that inspection; symbols remain the primary anchors when unrelated
diagnostic edits move lines. The workspace was
`C:\Users\purpl\Desktop\MidnightClub3-Port`. This pass was read-only except for this new note:
no renderer change, build, game launch, GPU inference, process/input action or material adoption.

The reviewed material importer and BRDF are foundations, not an active PBR renderer. The next
bounded implementation can establish geometry for a subset of fixed-function draws. It cannot
establish all car/skinned geometry or physically based scene lighting by itself.

## Current geometry and shader interfaces

| Source anchor | Established fact |
| --- | --- |
| `runtime/gpu/state.h:128`, `VertexShader` | Stores the original program dwords, declaration, parsed register-to-stream/offset/type mapping, per-stream strides and program hash. |
| `runtime/gpu/state.h:160`, `State` | Carries the draw's 192 `vsc` float4 constants in hardware order (D3D register +96), 16 inline vertex attributes, 512 transform matrices, material and eight light records. A draw's own state matters; the latest global camera is not a substitute. |
| `runtime/gpu/vertex.h:35`, `Assembled` | Carries the actual assembled buffers/elements, register mask, per-register decode nibbles, converted vertices, topology/strip, index source/type/count, base vertex and pretransformed flag. Tire fitting can supply draw-private repaired bytes. |
| `runtime/gpu/shaders.cpp:23`, `kCommonHead` / `VOut` | The ordinary output contains `SV_Position`, four colors, four texture-coordinate float4 values and fog. It contains no dedicated geometry position, normal or tangent output. |
| `runtime/gpu/shaders.h:84`, `DrawConstants` | The shared `b1` layout includes `wvp`, `wv`, `wvn`, texture matrices, material/lights and blend matrices. `b0` is the 192 original vertex constants. Changing this layout risks all existing shaders; use a separate PBR constant buffer when one is needed. |
| `runtime/gpu/d3d12.cpp:2306`, `ClassifyShaders` | `fixed_transform` means `!prog && !a.pretransformed`. A declaration-only shader follows fixed function. Small no-depth draws can be classified as `quad`; that flag is not proof of a world surface. |
| `runtime/gpu/d3d12.cpp:2319`, `EnsureProgramHash`; `:2328`, `VertexKey` | The program hash derives from program bytes. The vertex cache key additionally incorporates registers, decode, fixed-function/FVF state and quad/pretransformed salts. Guest shader handles are addresses, not persistent material identities. Record the program digest and exact `vkey`/`pkey` namespace explicitly. |
| `runtime/gpu/d3d12.cpp:2337`, `VertexHlsl` | Selects `VertexProgramHlsl` or `FixedVertexHlsl`; there is no PBR geometry variant at this point. |
| `runtime/gpu/d3d12.cpp:1113`, `CreateRootSignature` | The ordinary root has `b0`, `b1`, four SRVs and four samplers; it enables input assembly, not stream output. No dedicated material bindings or post-VS evidence buffer exists. |

## Fixed-function facts and safe first scope

`FillTransformConstants` (`runtime/gpu/d3d12.cpp:2562`) defines VIEW=2, PROJECTION=3,
TEXTURE0=16 and WORLD=256. It computes row-vector `World * View`, then projection and
viewport adjustment. The matrices are transposed for the current HLSL packing convention.
`c.wv` is the model-to-view transform and `c.wvn` contains the inverse transpose of its
upper 3x3. WORLD0 through WORLD3 have separate blended equivalents when vertex blending is on.

`FixedVertexHlsl` (`runtime/gpu/shaders.cpp:728`) already computes these intermediate values
for untransformed geometry:

```text
ip = float4(position.xyz, 1)
cp = mul(ip, wvp)                  // current raster position, including viewport adjustment
vp = mul(ip, wv).xyz               // view position, before screen/clip conversion
vn = mul(float4(normal.xyz,0),wvn) // only meaningful with an established source normal
```

Its vertex-blend branch instead combines `bwvp`, `bwv` and `bwvn` using the original weights.
That branch should be a separate supported case after the ordinary static branch is proven.
Pretransformed vertices, points/lines, quad/screen passes, blended/additive draws, dynamic or
render-target textures and unknown surface classes are outside the initial material scope.

The historical world shader
`templates/game/out/native/captures/211022/vs_C029509767C46FD8.hlsl` has registers
`v0`, `v3`, `v9`, `v10`: position, prelit color and two UV sets. It has no `v2` normal.
Its computed `vn` uses inline `vattr[2]`; that is not evidence of a real world normal.
The historical capture supports this layout for the observed draws, not every world mesh.

A first opt-in variant can export the existing `vp` and an explicitly valid geometric
normal, retaining the original raster-position expression. For the observed planar world
triangles, face normals can instead come from position derivatives and a cotangent frame
from position/UV derivatives. Reject nonfinite/near-zero geometry and UV determinants;
account for orientation, mirrored UVs and front/back facing rather than silently selecting
an arbitrary handedness. Missing/degenerate data must retain the original shader.

Model-to-world position and a world-space camera are derivable only when the draw's WORLD
and invertible VIEW transforms are established. View-space camera position is the origin
for a correctly established model-to-view bridge. A separate current camera hook is not
needed for that view-space case. Do not recover these by guessing an inverse projection
from final `SV_Position`: the title's viewport/half-pixel adjustments and program transforms
are part of the raster path.

## Programmable car bridge: exact intervention points

The historical car translation
`templates/game/out/native/captures/211022/vs_1CB4A27574EC95F3.hlsl` proves:

- Lines 98-121 compute a damage/morph selector from `v3.w * c[116].x`, an offset from
  `v3.xyz - c[116].y`, and the selected `c[a0+118].w`. The resulting position is packed
  into **`R0.xyw`**, with the homogeneous component in `R0.z`.
- Lines 160 and 188 onward project **`R0.xywz`** using `c[100..103]`. Exporting raw `v0`
  would discard the original morph; exporting a conventional `R0.xyz` would also be wrong.
- Lines 124-133 form a transformed normal in **`R11.xyz`** by dotting decoded `v2`
  against `c[134..136]`. Subsequent instructions overwrite R11 with lighting/highlight
  terms. A bridge must snapshot it at the established instruction point; copying R11
  in the final epilogue does not recover this normal.
- `c[96..99]` and `c[108..110]` participate in the observed directional/ambient lighting.
  The shader alone does not prove the independent model/world/view coordinate systems
  of every matrix/light set. `c[100..103]` is a composite position transform, not an
  independently established model-to-world matrix.

These are facts about that exact captured program. A future bridge must match full
program identity plus declaration/decode and capture the relevant values before temporary
reuse. A filename, guest handle, model name or short opcode prefix is insufficient to
authorize another program. Unknown morph/layout variants keep the original path.

## Indexed skin bridge: separate mapping

`runtime/gpu/skinning_capture.h:12`, `IsIndexedSkinProgram`, is a diagnostic recognizer
for the original indexed three-influence program at XBE `0x4B0ED0`. It recognizes a
prefix and relative constant reads; it is not a complete PBR-program allowlist.

The fresher translation
`.local/bead-npc6-trace-captures/013050/vs_48C366892849F012.hlsl` and
`docs/notes/npc_source_weights.md` establish, within their documented scope:

- Bone row indices come from normalized D3DCOLOR RGB components scaled by hardware
  `c[105].w` (original D3D c9.w=256). The three weights are
  `v1.x`, `v1.y`, `1-v1.x-v1.y`.
- The bone-blended position reaches **R4.xyz** at line 180; R4.w came from position W.
  Lines 187-199 project that position through `c[100..103]`. **R4 is later overwritten**
  by lighting accumulation (line 240), so an epilogue-only R4 export is incorrect.
- The original program transforms its normal with the first influence. The resulting
  **R11.xyz** is available after the third normal dot at line 212, before lighting uses
  it. Replacing the authored rule with guessed blended normals is a separate art/render
  change, not a correction implied by this bridge.
- Existing source/palette/CPU reconstructions do not measure actual post-VS GPU output.
  They cannot certify every cutscene/rider/LOD or foot contact.

The old historical `211022` skin capture is not a substitute for the later correct-c9
capture. Exact shader/input provenance and current executable identity remain required.

## Capture shortcomings and minimal evidence addition

The current capture provides useful identification but does not serialize complete
executed geometry:

1. `CaptureRawVertices` / `CaptureInputs`
   (`runtime/gpu/d3d12.cpp:2708`, `:2754`) record at most four source vertices and decimal
   constants. Larger draws may record only changed constants and limited first-buffer
   dwords (`EndDrawCapture`, `:2789`). Full index data/base-vertex effects and all uploaded
   VB bytes are not retained.
2. `UploadConstants` (`:2651`) copies `s.vsc` to upload memory, then calls
   `Extension::Constants` on **that copy**. `CaptureInputs` prints the original `s.vsc`.
   This is an observation gap, not proof that an extension changes a particular car or
   skinning matrix. Capture the actual post-extension bytes for definitive replay.
3. `UploadVertexBuffers` / `DrawGeometry` (`:2943`, `:2954`) upload each assembled buffer,
   upload indices separately and issue indexed draws with `a.base_vertex`. Evidence must
   use these actual assembled/repaired ranges, not earlier guest VB headers alone.
4. `CaptureShaders` (`:2852`) retains translated HLSL keyed by the shader cache keys, not
   a post-VS position/normal buffer. A screenshot cannot replace the missing geometry data.

The smallest evidence-only addition is a separately opt-in, bounded selected-draw record:
exact program/declaration and shader keys, register/decode/element layout, all selected
assembled buffer/index bytes, topology/count/base vertex, post-extension vertex constants,
final draw constants and relevant draw-local transforms/texture identity. Enforce byte/draw
budgets, copy while sources are valid, preserve existing files, and name exact omitted data
or truncation; never label an incomplete record as replayable. This can be CPU-side and
does not need extra GPU readbacks, resource waits, draw reordering or a material renderer.

A post-VS measurement would be a later, separately reviewed opt-in stream-output path
with proper root-signature/PSO support, bounds/overflow evidence and fence-owned readback.
`docs/notes/npc_capture_evidence_gap.md` documents why this is still a distinct requirement.

## Lighting and activation boundaries

`LightToView` / `FillLightConstants` (`runtime/gpu/d3d12.cpp:2616`, `:2639`) establish
fixed-function light conversion for enabled original light records. They do not establish
that a prelit world draw has analytic lights. Observed city illumination is largely in
vertex color; the live car reflection target and screen-space headlight pools do not
automatically supply physically based environment lighting or analytic headlights.

The FOV hook `templates/game/src/game/fov.cpp:62`, `sub_002D4AE0`, currently adjusts
approved camera projection calls and calls the original body. It does not publish a
draw-specific camera/projection evidence interface for all reflection, mirror or HUD passes.
Use the exact draw-local transform state for the initial fixed-function bridge.

`SurfaceEvidence` (`runtime/gpu/pbr_material.h:68`) already separates geometric position,
normal, tangent basis and linear lighting. `Select` (`runtime/gpu/pbr_material.cpp:283`)
rejects missing geometry and then missing linear lighting. A geometry bridge alone must
not set `linear_lighting=true` or claim material activation. Current albedo/reflection alpha
semantics and accepted alpha digests remain separate requirements.

## Focused validation before integration

- CPU transform fixtures: row/column packing, nonidentity WORLD/VIEW, projection/viewport
  separation, nonuniform/mirrored scale, inverse-transpose normals and singular/nonfinite
  rejection. Treat vertex blending as a separate case.
- Exact layout fixtures: FLOAT3 W=1, packed-normal decode, missing register2, repaired
  draw-private vertices, indexed/base-vertex/strip interpretation and bounded evidence
  round trips. Reject truncated/overflowed records rather than accepting partial replay.
- Program fixtures: capture the car morph position and normal at their exact points;
  capture skin position/normal before reuse; reject a one-dword program change, mismatched
  declaration/decode, wrong index scale or unsupported layout. These are CPU/source tests,
  not proof of GPU execution.
- Generated HLSL: default-off shader source/keys/layouts remain exactly unchanged. Compile
  the separate opt-in interface for both stages; test derivative degeneracy, mirrored UV
  orientation and finite fallback. Do not expand the shared `DrawConstants` layout.
- Later bounded native A/B: verify raster/depth parity (including EQUAL passes), world
  face-normal/UV orientation, car morph/damage, animation, cutouts/fallback and memory/
  performance. Bind acceptance to the actual executable/source/settings/material evidence.
  The scheduling owner must preserve physical input and obtain the required cooperative
  media pause; this note authorizes no launch or worker changes.

The next code scope should establish and record static fixed-function geometry first.
Dedicated material bindings, linear lighting/environment treatment, car/skinning bridges
and reviewed material adoption remain subsequent independently validated work.
