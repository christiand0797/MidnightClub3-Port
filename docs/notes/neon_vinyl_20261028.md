# Neon colours, invisible vinyls, per-car tuning (2026-10-28)

User report: vinyls invisible, every neon colour white, per-vehicle tuning "not working". Static analysis only;
the game was not launched (the user playtests). Nothing below is verified in game yet.

## Neon shown white — fixed (likely cause)

- The neon glow is 0xFA5A0 mode 0x10: four FVF 0x142 quads (xyz, D3DCOLOR, uv) through the game's inline-array
  writer 0x2D5D90/0x2D5E40, texture `fx_car_neon_glow`, blend mode 2 = SRCALPHA/ONE (0x2D3660 jump table
  0x2D37B0). Vertex colour = neonRGB x flash x per-quad fade, alpha = intensity; packed ARGB by `_ftol2` 0x2DAE60.
- The HD replacements of the neon textures are white RGB with an alpha falloff: the colour is all vertex colour.
- The user's log had `neon = 4.0`. `ScaleTextureEffect` brightens SRCALPHA draws through `sblend.w`: alpha first,
  then the rest of the factor multiplied into the colour. At x4 any channel above 0.25 clipped to 1, so blue
  went cyan/white, orange yellow, lemon/light colours white.
- Fix (runtime/gpu/shaders.cpp): the colour gain is capped at 1 / brightest channel, so the hue is kept. The glow
  can no longer exceed a fully saturated colour at alpha 1; overlapping quads still add on the frame buffer.
  Test: `shader_stage_test`.
- Not covered: a separate `neon` tube mesh / `vp_neon` texture in the car bundle (not traced).

## Vinyls invisible — one real hazard fixed, cause not proven

- Vinyls are P8 gfxImage pages turned into pool textures by 0xE46F0: D3DPalette `{0x30001, Data, 0}` at tex+0x78,
  colours (0x400 bytes, game heap via `[0x4B51FC]` = 0x367290) at tex+0x7C, registered by D3DResource_Register
  0x4146F0 (`Data = (ptr + Data) & 0x0FFFFFFF`). The game heap is in contiguous memory, so the renderer's
  `0x80000000 | Data` palette read aliases the CPU's buffer.
- Vinyl colours are applied on the CPU: 0xF5830 (when render+0x3134 is set) rebuilds all 256 palette entries from
  the saved mask palette at render+0x13DC: R selects colour A (+0x13BC or the preset), G colour B (+0x13CC),
  B lightens, alpha = mixed alpha x mask alpha. Stage-1 palette bound by 0x1B9A60 (SetPalette(1, tex+0x78)).
  The renderer's P8 cache rehashes the palette every frame, so this path is handled.
- Hazard found: colour render targets are keyed by address only (`SizedTargetKey` ignores size for colour) and
  live until re-created at that address. Any uncompressed texture later built at a freed target's address was
  sampled as that stale target; its refresh (`UploadTarget`) decodes with **no palette** and only after a
  lock epoch the pool builder never raises. A P8 vinyl in recycled heap memory would show the old target or
  unpaletted indices, and never its colour changes. The NV2A cannot render to P8, so
  `CanBeTargetFormat(format, compressed)` (texture.h) now keeps DXT and P8 off the target path.
  Test: `texture_target_test`.
- If vinyls are still invisible after this: press F4 in the garage with a vinyl applied and read the stage-1 P8
  draw (palette alpha, texcoords, whether t1 is a target) in `captures/<time>/frame.txt`.

F4 now saves without XBE_CAPTURE_DIR (to `<exe dir>/captures`, `gpu::CaptureBase`) and logs "frame captured to"
at release level. Before this, an F4 press from the normal shortcut was silently ignored (2026-10-28 19:12 run).
F1 > Capture (settings_menu.cpp `Capture`) adds a delayed capture, an automatic mode (`[capture]` automatic /
interval_seconds / max_per_session; `gpu::capture::AutoSchedule` in capture_control.h, driven from
D3D12Backend::Present, paused while F1 is open) and a status line. Tests: capture_control, settings, graphics_menu.

### Vinyl capture 2026-10-28 19:29 (garage, `captures/f5522-*`)

- Draws 0629-0651: vs B01019E1, **ps 0**, t0 = paint target 830E2D80, t1 = P8 vinyl 833D7300, t2 = env target.
  Generated shader = `ps_8D5B2046CF7E9D3F` (stage 0 t0 x diffuse, stage 1 current x TFACTOR, stage 2 off).
- `tex_833D7300` decodes correctly through its palette: a tribal vinyl with a proper alpha mask. The texture and
  palette path are fine; the paint pass simply never uses t1.
- The paint target is filled only by the 64x64 ramp draws (0608-0621); nothing composites the vinyl into it.
- Conclusion: the carpaint effect's pixel shader (`lrp r0.rgb, t1.a, t1, t0 ...`) is missing, so the pass keeps
  the code's default stages (exactly garage_car_carpaint's). The pass applier 0x4F4F0 sets a pixel shader only
  when effect+0x6C is non-zero. The `.fx` text is assembled at load by the XGraphics assembler linked into the XBE
  (errors through 0x39D3E9, previously only visible as dropped DbgPrint/info logs). `src/game/shader_assembler.cpp`
  now logs those errors as `shader assembler: ...` to find why carpaint is rejected.

### Root cause found and fixed (2026-10-28 19:51 run)

- With compile/assembler logging, no effect failed to compile, yet 19 effects that define a pixel shader drew
  without one: carpaint, carpaint_novinyl, default_shiny(_rim), carbon_fiber, distort, rubber, tire, sprocket,
  rider_cloth/helmet_matte/helmet_shiny/vinyl_jacket, train, city_road, city_texanimation, ped, water, glow.
  The 29 that worked all declare a top-level `PixelShader PIXL = asm {...}` that the game fetches itself
  (GetPixelShader 'PIXL' -> effect+0x6C, set by 0x4F4F0). The failing ones put `PixelShader = asm {...}` inside
  the pass, which D3DX applies itself.
- D3DX applies pass states through an XDK state block: the first Pass (0x37F975) records them
  (BeginStateBlock 0x4169C2 sets device flags |= 0x20; the state applier 0x37F096 marks the pixel shader changed,
  `mov byte [0x42551C], 1`, then calls SetPixelShader; EndStateBlock 0x4173FD -> capture 0x4169D0), later passes
  replay it (ApplyStateBlock 0x416DFF: SetPixelShader([block]) when its PS flag is set).
- The capture copies `CDevice_PixelShader` (device+0x784) and adds a reference (0x416A63 `inc [eax]`). The HLE
  SetPixelShader never wrote that field, so every block recorded 0 and every replay set **no** pixel shader.
- Fix (runtime/xdk/d3d8.cpp): SetPixelShader stores the handle at CDevice_PixelShader; CreatePixelShader builds
  the XDK object (refcount 1, owned, definition pointer +8, copy +0xC, 0xFC bytes) and DeletePixelShader releases a
  reference, freeing at zero like the XDK (the capture's reference would otherwise outlive a freed handle).
  Test: d3d8 (object layout, device mirror, reference counting).
- Expected visible change: vinyls, carpaint env reflection, shiny body/rim, tire/rubber, roads, water, peds and
  glow drawing with their intended combiners. Not yet seen in game.

### How the paint shaders combine the vinyl (from assets_x.dat `shaderlibx/cars/*.fx`)

- `carpaint.fx` (street): t0 base paint, t1 decal/vinyl, t2 env map;
  `lrp r0.rgb, t1.a, t1, t0; mul_x2 r0.rgb, r0, v0; mov r1.rgb, t2 +mul_x2 r1.a, t2.a, v0.a; lrp r0.rgb, r1.a, r1, r0`.
  `carpaint_novinyl.fx` drops t1. The C++ `carpaint.shadert` (0x1B7190 -> base 0x4F320) is effect based: the pass
  applier 0x4F4F0 calls SetPixelShader only when the effect's handle at +0x6C is non-zero.
- `garage_car_carpaint.fx` has **no pixel shader**: fixed function, stage 0 texture x diffuse, stage 1
  current x TFACTOR (t1 unused), stage 2 disabled ("these states are the default in the code").
- Old capture `captures/204913` (2026-10-20, garage): draws 0613-0638, vs B01019A1, ps 0, t0 = paint target
  8316D800 (blue gradient painted by draws 0591-0604 with 64x64 ramp textures), t1 = P8 8345D780 = the game's
  `nodecal` (uniform, alpha 0), t2 = env target 83004E80. So that car had no vinyl, and in the garage the vinyl
  cannot come from t1 in the paint pass. Either the game composites it into the paint target (look for a draw into
  that target sampling the P8 vinyl) or another pass draws it. A capture with a vinyl applied is still needed,
  in the garage and on the street.

## Per-car tuning — no defect found

Reviewed runtime/app/tuning_profiles.cpp, templates/game/src/game/tuning_profiles.cpp and tuning.cpp mapping.
The port only writes CarCfg+8 (dirty, outside the identity signature). The installed sidecar holds identity
values for all three cars (Bel Air enabled, Golf/IS300 on global) and was not written during the 18:19 session.
Things that would look like "not working": sliders are disabled until a career car is loaded (state 2/5 with a
career object), outside career (Arcade/Tokyo) only global applies, and a sidecar save conflict makes it read-only
for the session (shown as a status line under the checkboxes). A concrete symptom is needed to go further.
