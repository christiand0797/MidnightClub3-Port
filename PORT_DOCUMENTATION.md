> **2026-10-21 continuation:** `docs/REANALYSIS.md` records repaired build/timing/tuning/media integration. Historical entries below include removed prototypes and superseded feature claims; use the current handoff and audit for present behavior.

> **Start with `docs/HANDOFF.md`** (current state, work in flight, addresses, formats, next steps). Agent research notes: `docs/notes/`.

# Midnight Club 3 DUB Edition Remix — Native PC Port: Working Notes

Hand-over document for the next person or agent. Keep it current: update **Status**, **Open issues** and **Log of
findings** whenever something is learned or fixed. Last updated 2026-10-21.

## What this is

A static recompilation of the original Xbox `default.xbe` (USA, XDK 5849) into a native Windows x64 executable, with
the console's kernel/XDK replaced by host code (D3D8 → D3D12, DirectSound → XAudio2, XInput → SDL3).
Framework: XboxXBERecomp. Game data is read straight from the user's disc image.

## Build and run

```
.\Build-MC3.ps1 -Jobs 8          # RelWithDebInfo, output templates\game\out\native\mc3.exe (+ .pdb)
.\Launch-MC3.ps1                  # uses the disc path saved in .local\source-disc.txt
```

- The link fails with "permission denied" while `mc3.exe` is running. Close the game first.
- Changing `templates/game/config/game.toml` regenerates all recompiled code (`generated/game/x86_recomp.*.cpp`,
  128 partitions): a full rebuild, several minutes.
- Release builds log only Report/Error lines unless `XBE_VERBOSE=1`.
- The user playtests themselves. Don't drive the game with automated input unless asked; automated navigation into a
  race has proven unreliable.

## Diagnostics (environment variables)

| Variable | Effect |
|---|---|
| `XBE_VERBOSE=1` | info-level logging in release builds |
| `XBE_PERF=1` | per-second fps/draw/upload statistics |
| `XBE_CAPTURE_DIR=captures` | enables frame capture (F3, `XBE_CAPTURE_FRAME`, or `CAPTURE` in the live file) |
| `XBE_CAPTURE_FRAME=600,2700` | capture these present numbers. Frame ~600 is the intro legal text |
| `XBE_CAPTURE_SNAP=819-823` | in a captured frame, also save the render target after these draws |
| `XBE_PAD_LIVE=<abs path>` | lines appended (`START 150`, `A+DOWN 200`, `CAPTURE`) are pressed on port 0 *in addition* to the real controller |
| `XBE_PAD_SCRIPT=<abs path>` | fixed timeline `ms buttons duration` (takes port 0 exclusively) |
| `XBE_WATCH=4B2D70:f,534774:x` | logs guest memory values once a second (f float, x dword, b byte) |

- The log is `templates/game/out/native/logs/mc3.log`, flushed every 250 ms (live).
- Crashes: the log gets a symbolized host stack (recompiled functions are `__imp__sub_XXXXXXXX`). A minidump goes to
  `out/native/CrashDumps`. Read it with `lldb -b -o "target create mc3.exe --core <dmp>" -o "thread backtrace"`.
- A frame capture writes `captures/<time>/frame.txt`: every draw with state, blend op, stencil, changed vertex
  constants and the first vertex. It also saves textures and targets as BMP and translated shaders as HLSL.
- The disc and XBE can be inspected without the game: see `tools/` and the session scripts described in **Log of findings**.

## Status

Boots, plays intro, menus, garage, free roam and races at 60 fps on an RTX 3050.

Fixed:
- Dark/unreadable UI text (all fonts, name-entry keyboard, intro credits). Cause: `D3DDevice_SetPalette` (0x412580)
  was missing from game.toml, so P8 font textures drew raw palette indices.
- Crash loading a car/race: the `IoQueryVolumeInformation` stub had its arguments swapped and zeroed low guest memory.
  It's now implemented properly (`runtime/kernel/files.cpp`). It backs XAPI `XGetFilePhysicalSortKey`, which reads
  `FILE_OBJECT->FsContext`.
- Game speed ×2 (cars, traffic, animation, race clock). Races run the engine clock in a fixed 30 Hz step mode; the
  hook in `templates/game/src/game/hooks.cpp` asks for 60 Hz instead (setting `graphics.sixty_fps`). Verified
  in-game: the `timing:` log line reads x1.000.
- White headlight ellipse: NV2A signed blend ops (0xF005/0xF006) emulated as two passes. Confirmed fixed in-game.
- Graphics settings overlay (F1 or Back+Y) with a post chain (grade, FXAA, CAS), resolution and render scale
  (supersampling). It's an overlay; the user wants it inside the game's own options menu (see Open).

In progress / open:
- **Native graphics page in the game's options menu** (user requirement: same font, as if shipped). Menus are built
  in C++ from string-table IDs (`CM_AudioOptions`, `CM_Hud_Options`, `Video Options`...). The lookup is `0x0EA750`
  with this=`0x507E08`, entry text at `+8`. Page building is around `0x177DC0` (calls `0x139520`, `0x177590`). The
  Flash template is `MenuOptionScreen` in `flash/pause_x.xbck` and `toplevel_x.xbck` (Rockstar-compiled, magic
  `82BF5980`). Implemented in `templates/game/src/game/graphics_page.cpp`. The Graphics entry is added (8th, reached by scrolling
  past Credits) and opens the Audio screen in "graphics mode". Findings:
  - Menu text is UTF-16. Strings are now widened (the first version was blank for this reason; not yet re-tested).
  - The Audio screen draws slider rows, not text rows. If text still doesn't show, rebuild on a text page (HUD
    Options, screen 0x08). Dump live cells with the `menudump.ps1` approach: menu = [[[0x50F148]+0xC]+0xFC], text
    cell = [menu+0x24C] + ((list*16+row)*4+col)*8. Lists are triple-buffered (+0x258 build, +0x25C shown).
  - Visible rows = ([menu+0x5C]+6)/[menu+0x60] (`0x348FE0`). The list scrolls.
  - Requested next: a ride-height slider on the Tuning page.
- Minimap black square: alpha-only clears (flags 0x80) were clearing all channels. Fixed with masked clears
  (`MaskedClear`); not yet confirmed in-game.
- **Black square around the minimap.** HUD draws 819–823 in a race frame use a dest-alpha mask (alpha-only clear
  `flags 0x80`, mask write `cw 8`, `DESTALPHA` blends). Inputs are correct; next step is
  `XBE_CAPTURE_SNAP` on those draws to see which one leaves the wrong alpha.
- **Opponent notification popup drawn off-screen to the left.** Not investigated.
- **NPC walk animations look wrong.** Re-check after the timing fix: it may just be the 2× speed.
- White rear window on cars; dark slab below the rear bumper (seen in race screenshots).
- `mc3_enhancements.toml` and most fields of `app::Settings` added by an earlier session are parsed but unused (FOV,
  bloom, SSAO, UI scale). `app::EnhancedRenderScale` duplicates `RenderScale`.

Requested by the user, not started:
- In-game graphics settings menu (overlay opened from the pause menu), motion blur toggle/strength, LOD/draw-distance
  increase, supersampling, SMAA/FXAA, CAS sharpening, colour grading, 60 fps vs 30 fps (original) toggle.
- DLSS: needs motion vectors and camera jitter the 2005 renderer doesn't produce. Not feasible directly. RTX 3050
  supports DLSS SR but not frame generation.

## Session 2026-10-21 (after another agent's pass)

- **NPC animation (stiff pedestrians).** Fixed-function vertex blending (`D3DRS_VERTEXBLEND`, NV2A skin modes 1-6,
  weights in v1, matrices WORLD..WORLD3 = transforms 256-259) was ignored. Now implemented in `FixedVertexHlsl`
  (`FixedKey::blend`, `DrawConstants::bwvp/bwv/bwvn`). The game sets state 0x89 through its render-state wrapper
  at `0x32E40`. Needs an in-game check.
- **HD texture packs.** `runtime/gpu/texture_pack.*`. The `graphics.texture_dump` setting writes every texture to
  `out/native/textures/dump/<hash>.png`; `textures/replace/<hash>.png` at any size replaces it (mips generated).
  The hash covers the guest bytes, format, size and palette. Textures uploaded more than 4 times (video, dynamic)
  are skipped. This supersedes the other agent's `.xpr` repack route: the game reads the disc image, so repacked
  files in `templates/game/assets` are never loaded.
- **Removed from the other agent's pass:**
  - `MC3Extension` was installed and harmful: `Settings()` returned true (every target remade every frame),
    `ext[4]` overflowed, and the empty `Shadow()` hook broke shadow shaders. It's now uninstalled (`game_info.cpp`).
  - Hooks on 0x425FDE/0x4257A0/0x425A74 were labelled "vehicle physics" but are in the WMADEC section (audio
    decoder). Removed, along with the `game.toml` symbol entries.
  - `sub_000EAE40` was labelled "motion blur speed check, 76 mph". It is a 2D draw routine (72 callers); the
    0x428C0000/0x43AA0000 arguments are screen coordinates 70/340. Removed.
  - Menu entries for settings nothing reads were removed: FOV, bloom, SSAO, colour grading, UI/HUD scale,
    deadzones, the fix_* toggles, texture memory, shader cache. Motion blur, glow, headlights and LOD are shown
    greyed out as "next".
  - The in-menu Graphics page was disabled by the other agent ("vfunc 0xDC crashes"). Their dumps predate the
    current binary and can't be symbolized. Needs a fresh repro before it's re-enabled.
- `PORTABLE_README.md` (other agent) overstates features: its enhancement-settings section and its xpr-copy
  instructions don't work as written.

## Log of findings (reverse engineering)

Addresses are guest virtual addresses in default.xbe.

- `0x20B6F0` is the engine time manager update. It reads the TSC (`0x20E150`, low dword), converts with
  1/733,333,333 (`0x46D6B8`) and writes the frame time to `0x4B2D70` (raw also to `0x4B2D74`).
  - Flags (bytes) `0x534774/75/76` are all set to 1 by `0x20880` for play.
  - `0x534776` rounds the frame time UP to 1/60, 1/30, 1/20 or 1/15 s.
  - The time is clamped to `[ *0x4B2D60, *0x4B2D5C ]` (0.0001, 0.1). It is *not* raised in races (an earlier
    guess).
  - **Fixed step mode**: `0x20BB00(float rate, int substeps)` (called from `0x20880` at play start with rate 30)
    sets `0x534770`=1, step `0x4B2D58` = 1/(rate*substeps), rate `0x4B2D54`. In that mode the update ignores
    measured time and every frame advances the step (`0x20B942`). The console ran races at 30 fps, so this was
    the ×2. The hook passes 60. `0x20BAB0(x)` returns to measured mode.
  - In measured mode (menus, free roam) the hook snaps time to whole 60 Hz fields with carry.
  - Live state can be read without pausing the game: guest memory is at `0x100000000` in mc3.exe
    (`ReadProcessMemory`). See the `peek.ps1` approach in the session notes.
  - The previous tick is at `0x534778`. Time scale is `0x4B2D6C` (1.0).
  - `0x534AC4`/`0x534A9C` select a replay/network time source (`0x20E2D0`, `0x20E340`).
- `0x412580` is `D3DDevice_SetPalette`. The signature table `xdk/5849/signatures.toml` has an empty entry, so
  xbe_symbols writes 0. Other `= 0` symbols in game.toml may also really exist: check before trusting a 0.
- `0x17A60` is XAPI `XGetFilePhysicalSortKey`. It uses `ObReferenceObjectByHandle` + `IoQueryVolumeInformation`
  (class 5, FS name `FATX`/`GDFX`).
- `0x32633B`/`0x32634C` are XAPI QueryPerformanceCounter/Frequency (TSC, 733,333,333 Hz).
- The renderer's fixed-function path and inline vertex attributes: NV2A uses the last `SET_VERTEX_DATA` value for
  attributes a vertex lacks. `State::vattr` now carries them (from the push-buffer walker in `runtime/xdk/d3d8.cpp`).
  This was **not** the headlight cause, but it is hardware-accurate.
- Race-frame structure (720×480 guest):
  - ~550 draws of world.
  - Depth copy to a 360×240 depth target.
  - 32 headlight light-volume draws (8 nested cones × front/back × 2 meshes) into alpha of `028C7E80`, using signed
    blend counters, then composited additively.
  - Glow/blur ping-pong on 360×240 targets `02921F00`/`028C7E80`.
  - HUD.
- The XDK swap (`0x418500`–`0x418720`) programs the flip interval from `D3DRS_PRESENTATIONINTERVAL` (render state 127
  at `0x422CDC`). The host `D3DDevice_Swap` paces by `fps_cap` instead.

## Key files

- `runtime/gpu/d3d12.cpp`: D3D12 backend (draw, capture, present).
- `runtime/gpu/shaders.cpp`: fixed-function and NV2A vertex program → HLSL, register combiners → HLSL.
- `runtime/gpu/texture.cpp`: texture formats and decode (P8 needs a palette).
- `runtime/xdk/d3d8.cpp`: D3D8 HLE, push-buffer walker, vblank thread, Swap.
- `runtime/kernel/*`: kernel exports (files, objects, memory, threads).
- `runtime/core/input.cpp`: SDL3 controllers, keyboard, pad script and live input.
- `templates/game/src/game/hooks.cpp`: game-specific replacements of recompiled functions.
- `templates/game/config/game.toml`: recompiler symbols (XDK function addresses).

### Invisible menus (2026-10-21): the shader constant buffer must match DrawConstants

The title and menu UI disappeared because `DrawConstants::ext` (C++, `runtime/gpu/shaders.h`) and the HLSL cbuffer
disagreed: `ext` was widened to `float[5]` and the default `ShaderHooks::Constants()` was emptied, so every constant
after `wvp` (the UI composite's `spread` taps, lights, texture transforms, `vattr`...) was read 16-20 bytes off.
Rules: `ext` stays `float ext[4]`, declared in HLSL as exactly one `float4 ext;` (the default hook and
`MC3ShaderHooks::Constants` both return that), and an extension writes `ext[0..3]` only. Any field added to
`DrawConstants` needs the same field, in the same place and 16-byte layout, in the HLSL cbuffer in `shaders.cpp`.
The MC3 extension is installed again (`game_info.cpp`, `.load`), now without the forced 1x targets (which rendered at
640x480), the broken `Shadow()` override, or the `Output()` override that dropped the saturate.

### HD texture pack workflow and texture detail (2026-10-21)

1. In game, F1 > Quality > "Dump textures" on, then play: every texture shown is saved once to
   `textures/dump/<hash>.png` (hash of the console's bytes, shape and palette; BGRA and DXT formats only).
2. `py -3.12 tools/texture_pack/upscale.py` (needs Pillow, numpy) upscales new dumps with Real-ESRGAN x4plus
   (`upscale_tools/realesrgan-ncnn-vulkan-v0.2.0-windows`) into `textures/replace/`: 4x, 2x when 4x passes 2048;
   edges wrap-padded so tiling textures stay seamless; alpha resized with Lanczos; flat or tiny textures skipped.
   Incremental: rerun after more dumping, `--force` redoes everything.
3. "HD texture pack" on (the default) shows them; the runtime builds the mips. Textures rewritten 4+ times (video,
   drawn to) are never replaced.

F1 > Quality > "Texture detail" (`[graphics] texture_detail`): 0 as the console; 1 Full (default) drops the title's
D3DTSS_MAXMIPLEVEL limit and positive LOD bias, adds a -0.5 bias and forces 16x anisotropic where mips are filtered;
2 Max samples only mip 0 (sharpest, may shimmer at a distance). Shadow map samplers are never changed
(`StageSampler` in d3d12.cpp).

### Game files: archives, textures, vehicle tuning (2026-10-21)

- `templates/game/assets/assets_x.dat` (+ `_1`.. `_5`, `banks.dat`) are "Dave" archives: header {"Dave", count,
  dir size, name size}, directory at 0x800 of {name off, data off, size, packed size} (packed: raw deflate), names
  6-bit packed with prefix sharing. `tools/texture_pack/dave.py list|extract` reads them. Parts: `_2` cars, `_1/_3/_4/_5`
  one city each (props, city sections per time of day), the base archive tune files, cutscenes, anims, UI.
- `.xbck` are baked memory images (base address at 0, image from 0x80) holding D3D texture objects
  {Common 0x0004xxxx, Data physical, Lock, Format, Size}; pixels at Data - (base & 0x0FFFFFFF) + 0x80.
  `tools/texture_pack/extract_all.py` writes every one to `textures/source/<hash>.png` with the runtime's ContentHash
  (`xbtex.py` is a Python port of `Hash` in shaders.cpp; verified bit exact against runtime dumps): 5965 bundles,
  76k textures, 4776 unique; P8 textures (palette paired at draw time) are not extracted yet. `upscale.py` reads
  `source/` and `dump/`. `.tex` (4416, cutscene/ped, 14 byte header + 256 colour palette + 8 bit pixels) and the
  `resources/vehicle/*.xbpf` page pools (+ `.xbpmap` CSV index) are other texture stores not handled yet.
- Vehicle tuning files: `.vehsim` (physics; WheelFront/WheelBack blocks: SuspensionOffset +0x48 etc., visitor
  sub_000A6DA0, vehSim visitor sub_000A3CA0 with wheel block pointers +0x54/+0x58), `.vehmodel` (garage fit limits,
  visitor sub_000FB760: TireDiameterLimitFront/Rear +0x14/+0x18), `.carcfg` (save: RideHeight0/1 +0x16/+0x17,
  TireWidth, RimSize, TireProfile bytes, parser around 0x270F00). `templates/game/src/game/tuning.cpp` hooks the
  visitors and applies `[tuning]` ride_height_front/rear and wheel_size_limit live (F1 > Vehicle), fingerprinting
  each block so freed memory is never written.
- Cutscenes: `video/**/*.bik` (106, 1.7 GB), decoded by the recompiled Bink library (`runtime/xdk/bink.cpp` hooks).

### Ped skinning, HD movies, audit list (2026-10-21)

- **Ped animation (stiff NPCs)**: peds are skinned by an NV2A vertex program (e.g. vs_48C366892849F012 in race
  captures): bone indices in v12 scaled by c105.w (D3D c9), then c[a0+0..2] per bone from the palette at c-96...
  c9 was never set in the renderer's state: the title writes it straight into the push buffer
  (NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4 + SET_TRANSFORM_CONSTANT 0x0B80..0x0BFC, the XDK's inlined constant
  set), which `Walk()` in runtime/xdk/d3d8.cpp ignored, so every vertex used bone 0 and bodies moved rigidly.
  Walk() now applies those methods to `State::vsc`.
- **HD movies**: runtime/gpu/video_hd.{h,cpp} + templates/game/src/game/movies.cpp. The title's player (open
  sub_003639C0, update sub_003638A0, close sub_00363B90, BinkOpen 0x3AB590, BinkCopyToBuffer 0x3AD280) still decodes
  the Bink; `video_hd/<path below video/>.mp4` (lower case, e.g. video_hd/story/sgameint.mp4) is decoded by Media
  Foundation in step with Bink's FrameNum (+0xC) and bound in place of the LIN_X8R8G8B8 movie texture
  (D3D12Backend::BindMovie). `[graphics] hd_movies` toggles it. Producing the mp4s: ffmpeg (winget Gyan.FFmpeg,
  not on PATH) decodes Bink; Real-ESRGAN x4plus is ~2-7 s a 640x480 frame on an RTX 3050, too slow for the ~63
  minutes (~113k frames) of video: a video model (realesr-animevideov3) is needed.
- **Known gaps still open** (keep this list current):
  - D3D8: LoadVertexShader / SelectVertexShader are log-only stubs (MC3 has not been seen calling them);
    pretransformed immediate-mode vertices unsupported.
  - DirectSound: buffer envelope (SetEG, hit by MC3) and buffer/stream LFO modulation not implemented.
  - game.toml `= 0` entries to verify against the XBE (SetPalette was wrongly 0 once): the IDirectSound /
    IDirectSoundBuffer 3D wrappers (SetPosition, SetVelocity, Min/MaxDistance, listener orientation...),
    D3DDevice_DrawVertices, fences, SwitchTexture, SetTile, etc.
  - MC3Extension: FOV is a comment only; ext[] values and the UI scale Sampled hook have no shader that reads them.
  - Settings with no effect yet: app.h force_widescreen_menus/fix_* compat flags, bloom, SSAO, color grading,
    fov_*, ui_scale_*, hud_*, deadzone_*, msaa_samples, texture_filtering, debug show_*; F1 "Level of detail" pending.
  - Textures: P8 (palette paired at draw time), cutscene .tex, vehicle .xbpf page pools not in the offline pack.
