# Original pedestrian input audit — 2026-10-25

No remaining input/weight fault was found in this bounded CPU audit. No runtime change, game launch, GPU work or
input operation was performed. This complements the fresh evolving-palette evidence in `npc_skinning.md`; it does
not replace a close-up native visual check.

## Original source ranges

The four original city pedestrian bundles were read from their Dave archives. Baked addresses were normalized
against the XBCK payload base at file offset 0x80. Geometry selection required stride 36, a valid VB header/data
pointer, a bounded index descriptor, a whole 36-byte vertex span and in-range referenced indices. Position-like
float scanning was not used to select these ranges.

| Original bundle | Validated vertex ranges | Vertices | Stored weights |
|---|---:|---:|---|
|tokyo_peds.xbck | 12 |2,656 |2,650 at (1,0); 6 at approximately (.685838,.314162) |
|sd_peds.xbck | 12 |2,702 |All at (1,0) |
|atlanta_peds.xbck | 12 |2,604 |2,598 at (1,0); 6 at approximately (.685838,.314162) |
|detroit_peds.xbck | 12 |2,703 |All at (1,0) |

Across these 48 ranges / 10,665 vertices, every decoded position and weight is finite, weights are nonnegative
and sum to at most 1 within stored float precision, and all RGB bone-row bytes are valid multiples of 3 from 0
through 66. No source vertex uses the reserved viewport rows 58/59. Most source art deliberately uses one rigid
joint per vertex; replacing this with guessed smoothed weights would alter the original models.

Scope: these are the structurally validated stride 36 ranges in the four city bundles, not a claim about every
cutscene character, rider, special layout or LOD selection in the game.

## Actual captured inputs and shader math

For both `013050` and `013051`, all 12 skin draws' first 4 captured vertices were matched against the original San
Diego source ranges: 96 position/weight/packed-normal/BGRA cases total. Printed position precision is accounted
for with a 1e-5 tolerance; packed normal and bone-index dwords match exactly. Some clothing draw prefixes occur
in more than one original range, so those matches are recorded as ambiguous source ranges rather than invented
unique model identities.

The fresh declaration is position FLOAT3 at 0, weights FLOAT2 at 12, D3DCOLOR indices at 20, packed normal at 24 and
UV FLOAT2 at 28. Missing position W and weight W are 1. RGB bone indices come from the D3DCOLOR bytes in R,G,B
order, which means memory byte offsets 2,1,0. For example, `FF120000` selects row 18 for the first influence.
The historical capture's float4 HLSL variables alone do **not** establish that it used an incorrect declaration.

The original 40-instruction program at 0x4B0ED0 and the actual captured translation were inspected. Its position
sequence implements:

```text
rows = floor(256 * normalized_RGB_indices + .001)
weights = (v1.x, v1.y, 1-v1.x-v1.y)
world_position = weights.x * bone(rows.x) * (position,1)
               + weights.y * bone(rows.y) * (position,1)
               + weights.z * bone(rows.z) * (position,1)
```

The ARL instructions have intervening independent instructions before their relative matrix reads. Paired ARL /
texture-coordinate MOV and MUL / reciprocal instructions consume their old operands before writes. The captured
translation follows that ordering. The original program transforms normals using the first influence; this is
not evidence that the native implementation should invent blended normals. No verified remaining position or
index-order error was found by this source inspection. CPU prediction is not post-vertex-shader GPU execution.

Using original full VB vertices, captured palettes/projection and the original formula predicts only roughly
10.5–34.6 console pixels of pedestrian height in those menu-background frames. Some predicted bodies are partly
outside the640×480 target. That scale cannot certify elbows, knees and foot plants visually.

Private proof files are under `.local/npc-skinning-source-20261025/`:

- `original-vertex-audit.json`: source SHA-256, geometry/range offsets, full weight distributions and invalid counts.
- `captured-input-source-verification.json`: 24 draw matches and predicted full-VB screen bounds.
- `shader-source-identities.json`: supported XBE/program and actual HLSL hashes. Program-with-header SHA-256 is
  `11ba1e51e3336ad8cf05be0c7f2ce17cadfc7a9f5e538c08e1109984aacabd67`; both captured HLSL files have SHA-256
  `516dd913ecf601b37f12b95b3ce93b4e286736ca8863c84c36a8db4e7711208b`.

## Close-up native check

The later private native session captured a nearby pedestrian's avoidance leap outside the San Diego garage.
The observed pose has a bent elbow, separated arms and differently bent knees/legs; it is useful evidence
that this actor can articulate. The raw Sky capture is
`.local/npc-native-closeup-20261025/evade-observed.png`, SHA-256
`541d843f56e5868b75ada8e7819db4869a472753f0c36f2a3d6de6cc9520aaa9`, 1536×864, 174,036 bytes.
Its payload is JPEG despite the filename. This single leap does not certify a walking cycle, foot contact,
every pedestrian model or animation timing. A nearer idle capture cropped the actor's head/feet and was not
accepted as full-body gait evidence. The walking check below remains open.

1. After the scheduling owner confirms the cooperative GPU stop and no human session needs the GPU, use the
   isolated save/settings instance. Keep normal 60 fps, close F1/F2/pause overlays, and preserve physical input.
2. In free roam, use the confirmed W/S and A/D driving controls to park near a sidewalk without hitting the
   pedestrian. Place a walking pedestrian ahead and to one side of the car so the car does not cover their feet.
   Aim for a full body at least 80 console pixels tall; a stationary camera approximately5–8m away is suitable.
   Use an on-screen verified camera control if a view change is necessary; do not guess a button mapping.
3. Release driving keys. Record several seconds of an uninterrupted walking cycle and take three F3 captures at
   unequal short intervals, approximately 0, .23, .61 seconds. Keep the camera stationary and show the same person
   with head, hands, knees and feet visible. Avoid exact repeated gait periods.
4. Run the existing temporal analyzer with its ordinary 2 m matching bound. Require stable geometry, unambiguous
   nearby roots, correct scale and evolving relative nonroot poses, then inspect the actual arm swing, knee bend
   and foot plants in the video. A still image or different bone matrices within one frame is insufficient.
5. If the close-up still looks wrong despite evolving correct inputs, retain the exact shader/target/draw IDs
   before another source change. A post-VS mesh inspection would distinguish GPU vertex results from lighting,
   occlusion or artist-authored rigid skinning. Compare 30 fps only if the normal 60 fps check shows a real timing
   defect; no broad clock rewrite is supported by the evidence above.
