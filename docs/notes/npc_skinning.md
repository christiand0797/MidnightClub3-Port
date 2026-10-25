# Pedestrian skinning investigation and constant parity — 2026-10-24

The user still reports broken/static NPC arms and legs. The changes below repair independently proven XDK
constant-handling gaps and improve capture evidence. They do **not** establish that the current NPC animation
failure is fixed. A fresh same-pedestrian temporal capture and visual check remain required. Older WORLD0–3
transfer and c9 handling changes alone are insufficient evidence.

## Exact title evidence

- The supported original XBE stores its indexed three-influence skin program at **0x4B0ED0**, with 40 instructions.
  The first instruction multiplies input v12 by hardware c105.w, the D3D register c9. Subsequent ARL operations
  select three-row bone matrices from hardware c0 upward (D3D c-96 upward), and blend positions using v1 weights.
  The older translated file is `vs_48C366892849F012.hlsl`; shader-cache names can change without changing this
  program's behavior.
- Original initialization **0x2D6FC0–0x2D6FC5** calls XDK **0x414EB0** for hardware register105 using the table at
  **0x4B5804**, whose four original float values are exactly 256. The expected skin index scale is therefore 256.
- The observed pedestrian layout has stride 36: position FLOAT4 at 0, weight FLOAT1 at 16, D3DCOLOR bone indices
  at 20, packed normal at 24 and FLOAT2 texture coordinates at 28. Original packed index bytes use BGRA memory
  order; the shader consumes RGB as three palette row indices.
- Reanalysis of old captures finds 4 skin draws in 211022, 2 in 013316, 4 in 014206 and 6 in 014417. All have c9.w=0.
  Their differing bone transforms prove that articulated palette data exists; they do **not** prove that any
  one pedestrian's pose advances over time. These old captures also do not prove the current executable's state.

Unsigned-byte normalization is **/255**, including the PBYTE layout, rather than /256. The existing conversion
and NV2A-style `floor(value+.001)` ARL preserve every valid palette row:
`floor((row/255)*256+.001)==row` for 0 through 191. Changing normalization would not be justified by this failure.
The primary xemu [vertex decoder](https://raw.githubusercontent.com/xemu-project/xemu/master/hw/xbox/nv2a/pgraph/vertex.c)
uses /255, its [OpenGL input binding](https://raw.githubusercontent.com/xemu-project/xemu/master/hw/xbox/nv2a/pgraph/gl/vertex.c)
uses normalized unsigned bytes, and its
[vertex-program implementation](https://raw.githubusercontent.com/xemu-project/xemu/master/hw/xbox/nv2a/pgraph/glsl/vsh-prog.c)
uses the same ARL bias. The original title's byte indices and scale are separate primary evidence.

## Proven constant-handling corrections

`runtime/xdk/d3d8.cpp` now uses a locked, bounded shared constant writer. Hardware register numbers already
include the title's +96 bias; the helper does not add it a second time.

| Original function | Verified original behavior | Native correction |
|---|---|---|
|414EB0, one constant | Writes GPU commands and guest shadow at 421CC0+16×register | Copy the same 16 bytes into both native constants and the optional configured guest shadow |
|414F60, four constants | Writes GPU commands and the same guest shadow | Mirror the same 64 bytes, bounded at hardware register 191 |
|414F10, one Fast | Writes GPU commands without copying guest shadow | Keep guest shadow unchanged |
|415010, bulk Fast | Count is dwords, including partial final vectors; no guest shadow copy | Preserve every requested in-range dword, including 1/3/5/17-dword uploads |
|4150F0, original non-Fast bulk wrapper | Copies guest shadow unless CDevice.Flags&0x10, then calls 415010 | Leave the recompiled wrapper intact; Fast HLE must not duplicate its shadow behavior |
|416F80, original state-block capture | Reads requested constants from 421CC0 | Its unmodified original body now observes the one/four HLE writes |

The optional image data symbol `D3D_VertexShaderConstants=0x421CC0` belongs to the supported disc configuration.
Other image configs lacking this symbol retain their previous shadow behavior. There is no guessed generic
CDevice offset or new `gpu::State` layout.

Direct setters now also leave the raw hardware constant-load cursor where their original pushes would leave
it. An empty bulk setter still performs LOAD; a valid partial upload preserves its register and component state.
Pending older raw writes execute first. Null/wrapping sources and out-of-range register requests are bounded.

Raw NV097 constant methods now select their component from the **method number**, rather than an assumed
sequential software component counter. Method 0xB84 writes Y even when no preceding X was written. Repeated
nonincrementing Y methods keep writing Y; a W method advances the constant register pointer. This follows the
primary [NV2A method handler](https://raw.githubusercontent.com/xemu-project/xemu/master/hw/xbox/nv2a/pgraph/pgraph.c),
whose SET_TRANSFORM_CONSTANT handler uses the method's component and advances the pointer only after W.
Ordinary whole-vector uploads retain their previous results.

The previous shadow mismatch could affect original state capture/restore. A missing cursor or method component
could affect mixed HLE/raw constant writes. Neither relationship has yet been observed as the current NPC cause.

## Capture diagnostics and temporal check

F3 captures now recognize this exact title skin-program prefix and record the original stream VB Data IDs,
declaration layout, all bone row indices present in the assembled vertex range, full constants and first 4 vertices.
This runs only while capturing; draw behavior, shader math, guest vertex bytes and physical input are unchanged.

Capture a pedestrian in view at least twice, preferably three times at different short intervals. Keep the same
camera and visible character. Do not interrupt a human game session or run alongside a GPU media worker.
Use the normal scheduling owner to arrange capture; this diagnostic does not launch or control anything.

```powershell
python tools/diagnostics/analyze_ped_skinning.py <first-capture-directory> <second-capture-directory> --output .local/npc-temporal.json
```

The read-only analyzer identifies skin draws and reports incorrect c9, unclassified index layouts and nonfinite
constants. It matches geometry and nearby root positions, rejects ambiguous neighboring matches, and compares
**inverse(root)×bone** matrices. Thus whole-character translation or turning cannot masquerade as moving arms
and legs. Matching remains an inference: shared geometry/root proximity is not a guest entity identifier.
No observed pose change alone does not prove a frozen animation; the sample may be idle or return to the same
animation phase. The report separates zero observed nonroot bones from a measured unchanged relative pose.

## Verification and remaining work

- `tests/vertex_constants_test.cpp` executes actual original one/one-Fast/four/bulk-Fast setters and the original
  state-block reader from `x86_recomp.122.cpp`. It compares all 192 native constants and guest-shadow bytes,
  including NaN payloads and negative zero, and verifies constant cursor state. It covers partial bulk writes,
  bounds, the original pure-device bulk wrapper, pending-raw ordering and raw continuation without another LOAD.
- The raw-method fixture covers repeated Y-only writes, W-only register advance and a later Z-only write.
- `tests/skinning_capture_test.cpp` verifies the original shader prefix, rejection of unrelated programs, raw
  BGRA/RGBA and converted-float bone row agreement, invalid layouts, truncated buffers and all valid row indices.
- Both focused CTests pass in `.local/vertex-constants-build`. The analyzer's 8 Python tests pass, including
  world-motion cancellation, real relative-pose change, shared-mesh nearest-root pairing and ambiguity rejection.
- A normal native rebuild and complete suite are the parent's responsibility. Current live paired NPC evidence
  and the visual arms/legs check are still pending; a human session in another game must remain undisturbed.
- If fresh captures show correct c9 and a changing relative palette while limbs remain wrong, investigate actual
  input layout/weights and translated skin execution next. If the same matched pedestrian's relative palette
  remains unchanged during non-idle motion, trace its original animation-controller update and time input.
