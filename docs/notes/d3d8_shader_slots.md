# D3D8 HLE: vertex program slots and pretransformed immediate-mode vertices

2026-10-21. Source change in `runtime/xdk/d3d8.cpp` only (`runtime/gpu/state.h` unchanged). Not built or run by
this worker. A `clang++ -fsyntax-only` pass with the build's flags (`-Wall -Wextra`, MSVC headers) is clean. The
lead must build and the user must playtest.

## Evidence in this title

- `game.toml` maps `D3DDevice_LoadVertexShader = 0x4151C0` and `D3DDevice_SelectVertexShader = 0x415220`.
  `LoadVertexShaderProgram` is not in `game.toml` or `xdk/5849/signatures.toml`. It gets a placeholder symbol
  (`FFFFFE05`), the same way BeginPush/EndPush/SetPalette do, so it is live only for an image that maps it.
- The current `templates/game/out/native/logs/mc3.log` has no "not supported" lines from the old stubs and no
  pretransformed immediate-mode line. MC3 has not been seen calling these functions or pushing program methods
  inline. The new code is correct according to the references below, but this title's runs do not exercise it yet.
- XDK signature constants: SelectVertexShader pushes `0x00081E94` (EXECUTION_MODE and CXT_WRITE_EN, 2 dwords) and
  `0x00041EA0` (PROGRAM_START), and touches CDevice +0x794/+0x798/+0x79C. LoadVertexShader pushes `0x00041E9C`
  (PROGRAM_LOAD).

## NV2A methods (xemu `hw/xbox/nv2a/nv2a_regs.h`)

| Method | Value | Handling in Walk() / EndPush |
|---|---|---|
| NV097_SET_TRANSFORM_PROGRAM | 0x0B00..0x0B7C (32 dwords) | dword `(m-0x0B00)/4 & 3` of slot `g_prog_load`; after dword 3, `g_prog_load++` (as xemu does) |
| NV097_SET_TRANSFORM_CONSTANT | 0x0B80..0x0BFC | unchanged: `vsc[g_const_load][part]`, register advances every 4 dwords |
| NV097_SET_TRANSFORM_EXECUTION_MODE | 0x1E94 | consumed. MODE bits 0-1 (2 = program, 0 = fixed). Fixed mode is only logged |
| NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN | 0x1E98 | consumed, no effect |
| NV097_SET_TRANSFORM_PROGRAM_LOAD | 0x1E9C | `g_prog_load = v` |
| NV097_SET_TRANSFORM_PROGRAM_START | 0x1EA0 | `SelectProgram(0, v)`: start at slot v with the declaration in use |
| NV097_SET_TRANSFORM_CONSTANT_LOAD | 0x1EA4 | unchanged: `g_const_load = v`, part = 0 |

All of these are now in one function, `ApplyTransformMethod()`. Both `Walk()` and `EndPush` call it in command
order. Before this change, EndPush stored them in `State::simple[]`, where nothing read them.

## Design

- **Program memory**: `g_slots[136][4]` plus a `g_slot_written` flag per slot. `g_slot_gen` changes on every write.
- **Selection** (`g_sel`): {on, start slot, declaration handle, declaration object (null for an FVF), gen}.
- **BindSelected()** takes slots from `start` up to the first instruction with the final flag (bit 0 of dword 3,
  the same bit `VertexProgramHlsl` stops on) or the first unwritten slot. From these it builds a
  `gpu::VertexShader` with:
  - the program;
  - the declaration's `decl`, `inputs` and `stream_stride`, or `FvfInputs()` for an FVF;
  - `program_type` 0x2078;
  - `handle` = the declaration handle, or FVF | 1.

  It then binds this object as `g_state.vs` / `vs_handle`, the same shape the renderer already consumes. Objects
  are cached by (declaration object or FVF, program words) and keep their declaration object alive. This matters
  because the render thread tells shaders apart by pointer and caches the program hash inside the object. If the
  program equals the declaration shader's own program, that shader's object is reused. If the start slot is empty,
  the declaration's own shader stays bound and a line is logged.
- **SetVertexShader(handle with a program)** behaves like the XDK's, which calls LoadVertexShader(h, 0) and
  SelectVertexShader(h, 0) internally (confirmed by Cxbx-Reloaded `XbVertexShader.cpp`, whose SetVertexShader
  checks that the start slot is 0). It copies the program to slot 0 and turns selection on at 0, but it keeps the
  shader's own object bound. A declaration-only shader or an FVF turns selection off (fixed function).
- **LoadVertexShader(h, addr)** / **LoadVertexShaderProgram(func, addr)** only write slots. Programs past slot 135
  are cut off, and so are loads that start there; both cases are logged.
- **SelectVertexShader(h, addr)**: a non-zero h sets the declaration and loads its D3DVSD_CONST constants, like
  SetVertexShader. h = 0 keeps the current declaration: the one the previous selection used, or the bound shader
  or FVF.
- **SyncProgram()** runs before every draw: DrawVertices, DrawIndexedVertices, DrawVerticesUP,
  DrawIndexedVerticesUP, the EndPush primitive and EndPrimitive. If slots changed since the selection, it binds
  again. This matches the hardware, which reads program memory at draw time.
- **Ordering**:
  - SetVertexShader, Load*, and Select call `Walk()` first, so commands the title pushed earlier apply first.
  - The four SetVertexShaderConstant* HLEs call `FlushPush()`, which runs `Walk()` only when the push pointer moved
    since the last walk. An older raw `SET_TRANSFORM_CONSTANT` therefore can no longer overwrite a newer API
    constant (runtime_audit.md concern 2).
  - In the common case the cost is one compare. The ped-skinning c9 path through Walk is unchanged.
- **CreateDevice** clears the slots and the selection.

## Pretransformed (XYZRHW) immediate mode

Before this change, EndPrimitive drew XYZRHW vertices through the transformed fixed function path, because it set
`vs_handle | 1`. Now `DrawPretransformed()`:

1. Starts from the bound FVF and adds DIFFUSE, SPECULAR, PSIZE and texture count for slots the primitive wrote that
   the FVF lacks.
2. Packs every vertex in that FVF's layout (`gpu::FvfInputs`):
   - FLOATn copies n components;
   - D3DCOLOR packs bytes B G R A from the register's r g b a, clamped, NaN → 0.
3. Draws through DrawUp with `vs_handle` = that FVF.

The renderer's own XYZRHW path then applies (`Pretransformed()`, `FixedVertexHlsl(pretransformed)`, cache salt).
This required no State change. Texture coordinate sizes come from the FVF's TEXCOORDSIZE bits, as for any FVF
draw.

EndPrimitive now also copies `g_im_attr` to `State::vattr` before each immediate draw. Registers the vertices lack
then read the inline value current at that END, not the value at the end of the walk (runtime_audit.md concern 1).

## Uncertain / not done

- XDK SetVertexShader loading to slot 0 comes from Cxbx-Reloaded's notes and checks, not from disassembly of this
  XDK build's SetVertexShader. If a title depends on programs it loaded at slot 0 surviving a later
  SetVertexShader, this model overwrites them, as the hardware would.
- EXECUTION_MODE = fixed pushed inline (an inlined SetVertexShader(FVF)) is logged and ignored. The bound shader
  does not change.
- SelectVertexShaderDirect / LoadVertexShader on shaders made by inlined library code (objects not in `g_vs`),
  state and read/write programs (RunVertexStateShader, types 0x7378/0x7778), and the CDevice vertex shader fields
  (+0x794..+0x79C) are not modelled.
- An FVF used as the declaration of a selected program gets `vs_handle` = FVF | 1 (bit 0 is D3DFVF_RESERVED0, never
  set by titles). The renderer then takes the layout from the made object.
- Immediate-mode slots that an XYZRHW FVF cannot carry (normal, fog, back colours, blend weights) are not given
  per-vertex. The shader reads their inline value from `vattr` instead.

## Lead must verify

1. A full build. The syntax check passed, but nothing was linked. `sub_FFFFFE05` (the LoadVertexShaderProgram
   placeholder) must not collide with anything.
2. Menus, HUD and races render as before. The only always-on changes are: SetVertexShader copying programs into
   slots, Walk() at SetVertexShader, FlushPush in the constant setters, and EndPush constants now going to `vsc`.
3. Ped skinning is still correct (Walk constants now go through ApplyTransformMethod; same arithmetic).
4. Grep the log for `LoadVertexShader`, `SelectVertexShader`, `vertex program from slot`, `pushed transform
   execution mode` and `selected at slot`. Any hit is the first evidence of these paths in MC3.
5. Update the PORT_DOCUMENTATION.md "Known gaps" entry (this worker may not edit it).
