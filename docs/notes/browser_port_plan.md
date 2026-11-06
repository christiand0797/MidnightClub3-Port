# Browser port: MC3 running entirely in the browser (WebAssembly + WebGPU)

Goal: open mc3.top, pick your own game disc image once, and play the port in the browser: same game code, online
lobby/rooms/chat through the existing mc3.top relay, nothing installed. The disc never leaves the player's machine:
mc3.top serves only the port (runtime + recompiled code) and the online services. Kept in the browser's private
storage (OPFS) after the first pick.

## What we start from

- **Generated game code** (`templates/game/generated/game/x86_recomp.*.cpp`, 137 files, ~212 MB of C++) is
  freestanding: it includes only `runtime/cpu/x86_ops.h`, `x86_context.h` and `runtime/cpu/freestanding/*`. Every
  guest access is `base + uint32 address` through the `X86_MEM*` / `X86_PTR` macros. No intrinsics, no inline asm.
- **Headless harness** (`harness/`, Linux): the runtime without a window, GPU, audio or pad: `null_backend.cpp`,
  `null_audio.cpp`, `pad_script.cpp`, `reference_renderer.cpp` (CPU renderer into guest memory), `compat/` (the few
  Win32/Winsock/DXGI names portable sources use). Emscripten is close to this target; the harness is the base of the
  web build.
- **Windows-only pieces to replace**: `runtime/gpu/d3d12.cpp` (+ PBR, DLSS, texture packs), `runtime/audio/xaudio2.cpp`,
  Winsock system link (`xnet.cpp`), Win32 window/input, `VirtualAlloc`/`mmap` guest memory.

## The hard part: guest memory

The runtime reserves the whole 4 GB guest space and maps the 64 MB of RAM twice (0x80000000 and 0xF0000000, same
physical pages). WebAssembly has one linear memory, no aliasing, and 32-bit wasm tops out at 4 GB including the C++
heap. Regions in use (`runtime/kernel/memory.h`):

| Guest range | Use | Web layout |
|---|---|---|
| 0x00010000–0x7FFE0000 | image + user allocations | cap `kUserHigh` at 0x10000000 (the Xbox has 64 MB; only placement is sparse) |
| 0x80000000 +64 MB | contiguous/physical RAM | one copy |
| 0xF0000000 +64 MB | write-combined view of the same RAM | folds onto the copy above |
| 0xB0000000 +256 MB | system pool, thread blocks | shrink to what is used |
| 0xD0000000 +256 MB | thread stacks | shrink to what is used |
| 0xFE000000 +16 MB | MCPX registers (zero page) | small block |

**Decision: 32-bit wasm with a top-byte translation table.** Under `__EMSCRIPTEN__`, `X86_MEM*(a)` becomes
`base + a + x86_xlate[a >> 24]` (256 int32 offsets, cache resident), packing the regions above into one block of a few
hundred MB. One extra load per guest access. The runtime's guest accessors (`kernel.h` `G`/`R32`/`W32`, plus the
spans the GPU/audio code read) go through the same table; bulk spans stay valid because each region is contiguous.
Alternative kept in reserve: memory64 (wasm64) keeps `base + a` unchanged and folds only the 0xF0000000 mirror, but
needs a >4 GB linear memory and slower bounds checks; Safari has no memory64.

## Status (2026-11-05)

- Step 1 done (1d88b9e): packed guest memory, all generated code compiles with Emscripten 4.0.15.
- Step 2 done: `tools/web/build_wasm.py` builds the whole game (harness stand ins + runtime + game sources + generated
  code, 36 MB .wasm) and `node tools/web/run_node.cjs <disc image>` boots it from the player's disc: XBE checked
  against the recompiled release, 22,465 functions dispatched, intro movies, legal screens and the title screen, about
  24 frames/s with the CPU stand in renderer drawing nothing. `REF=400` saves frames drawn by
  `harness/reference_renderer.cpp`: [first frames](browser_port_first_frames.png). Left out for now (Direct3D 12 /
  WIC / Media Foundation enhancements): `gpu/pbr_*`, shader cache, texture packs, HD movies, draw evidence; stand ins
  in `tools/web/web_stubs.cpp`.

- Step 3 done: `tools/web/play.html` + `tools/web/serve.py`. The page asks for the disc once, streams it into the
  browser's private storage (OPFS, ~1.5 min for 4.5 GB), then runs the game on a worker thread (PROXY_TO_PTHREAD,
  WASMFS with OPFS mounted at /opfs: the disc copy, logs and hdd saves persist there). Frames from the CPU renderer
  (every 2nd frame, `?ref=N` changes it) go to a canvas; keyboard and Gamepad API into `web_set_pad` (a tap counts as
  held 250 ms). Tested in the Claude app's Chromium and in Firefox: intros, title screen, menus reached with Enter,
  about 9-10 frames drawn per second (the game itself ~20). `serve.py --dev-disc` + `?devdisc` is a loopback-only
  test path that copies the disc without the file picker and starts the game.

## What Underground 2 Revived (nfsu2.xyz) has that this needs (docs/notes/online_lobbies.md)

In priority order:
1. **WebGPU renderer** (step 4): the reason it runs at 60-120 fps; ours draws on the CPU at ~10.
2. **Controllers**: Gamepad API is in (standard mapping, sticks, triggers as analog). Still to do: rumble
   (`vibrationActuator`), button remapping, wheels (G29/G920/G923 axes), touch controls on phones, split screen.
3. **Audio** (step 5): AudioWorklet mixer; the radio with your own music folder.
4. **Settings tab** on the start page: frame cap 30/60/120/240, widescreen, resolution scale, full screen, HUD,
   language; editable settings file; reset.
5. **Saves**: zip backup / restore of the hdd folder (and import from a PC install); optional cloud saves through
   mc3.top accounts (`/api/cloud`).
6. **Online in the page** (step 6): the Online Lobbies window (native `runtime/app/lobby_menu.cpp`) as page UI, the
   built-in link over a page WebSocket; free roam with nearby players; queue when full; live counts; quick chat,
   friends, block list.
7. **Faster start**: stream disc reads on demand instead of copying the whole image first (OPFS copy stays as the cache).
8. **Mobile**: lower resolution scale on phones, memory budget below 2 GB.

- Live on mc3.top (5 Nov 2026): https://mc3.top/play/ (tools/web/stage_site.py stages the page and the build into
  tools/multiplayer/browser/play/, the build in 20 MiB parts under Cloudflare's 25 MiB file limit, joined by the page
  and kept in the Cache API by version; `_headers` gives /play/* COOP/COEP). Runtime at -O2: ~24 game frames/s.

- Step 4 started: WebGPU. The game's threads cannot own WebGPU (adapter and device are promises; those threads
  block), so `tools/web/webgpu_backend.cpp` records each frame's commands into one of two shared buffers and
  `tools/web/gpu.js` runs them on the page's main thread every animation frame, presenting to its own canvas
  (`?gpu=1`, falls back to the CPU picture without WebGPU). First slice: clears and presents (verified: the game's
  presents arrive, the canvas follows the 720x480 back buffer). Next: draws (vertex/index data into the frame
  buffer, a WGSL twin of `runtime/gpu/shaders.cpp`, pipeline cache by PixelKey/FixedKey), textures (decode with
  `runtime/gpu/texture.cpp`, upload once, track writes), render targets and read-backs (`Sync` waits for the page),
  then the F1 menu (ImGui draw lists through the same path). The game waits for the page each frame, so a hidden
  tab runs it at the browser's background rate.

- Step 4, slice 2 (draws): the reference renderer's vertex stage runs on the CPU (vertex programs, fixed function
  transform and lighting, fog) and hands transformed triangles to the page (`reference::SetGpuSink`), with textures
  decoded to RGBA8 only when their guest bytes change and render targets sampled straight from their GPU textures.
  `gpu.js` rasterizes into one texture per game render target with a WGSL port of `ShadeStages` (four fixed function
  stages, alpha kill and test, specular, fog), pipelines cached by blend/depth/cull/mask, and shows the presented
  target. A hidden page keeps running frames through a message loop. Verified: intro movies, title screen and attract
  movie draw correctly at 22-31 frames/s (CPU picture: ~10). Known: 3D scenes (menu city) come out washed out white
  although no texture is missing (fixed function path: blend/fog/depth mapping to compare draw by draw against the CPU
  renderer); pixel shader combiners use a stand in (stage 0 texture x diffuse); `MC3Gpu.snapshot()` reads back black.

## How nfsu2-web is built (github.com/th0mk/nfsu2-web, read 5 Nov 2026)

- wasi-sdk, not Emscripten: `wasm32-wasip1-threads`, `-O3 -msimd128 -mbulk-memory -mnontrapping-fptoint -mtail-call`,
  wasm exceptions and setjmp (no Asyncify), `-fno-delete-null-pointer-checks` (guest address 0 is real memory),
  wasm32 shared memory, 704 MB initial / 4 GB max, guest span below `--global-base` 0x28000000, own page host
  running the game's threads as workers. Lifted code in content-hashed shards.
- Direct3D 9 on WebGPU in `runtime/web/gpu_web.c` (+ `wgsl.h`, `vsemu_wgsl.inc`: vertex shader emulation in WGSL),
  GPU commands marshalled to the WebGPU thread (`gpu_proto.h`); a software fallback at ~13 fps.
- Data streams on demand, compressed, cached per PC; optional preload; service worker for offline start.
- AudioWorklet sound with a latency setting; Gamepad API, wheels (G29/G920/G923, 270°), shifter, RC radios, touch
  with tilt steering, rumble on Chrome; per-device resolution scale / frame rate / car cap with phone auto-tuning.
- Rust server for the virtual LAN, free roam up to 16,000 with a queue; cloud saves end-to-end encrypted by passphrase.

## Milestones

1. **Toolchain + compile spike.** Emscripten installed; one generated partition compiles with em++; the translation
   macros in `x86_ops.h` behind `__EMSCRIPTEN__`.
2. **Headless boot in Node.** Harness sources + all partitions + translated memory, run under Node with the disc
   image from a file: reaches the same frame counts / draw lists as the Linux harness.
3. **Browser boot, CPU frames.** Same build in a page: disc picked with the File System Access API / `<input type=file>`
   and kept in OPFS, frames from `reference_renderer.cpp` blitted to a canvas, keyboard/gamepad into `pad`. Threads via
   pthreads (SharedArrayBuffer: mc3.top must send COOP/COEP headers on the play page).
4. **WebGPU backend.** A `gpu/webgpu.cpp` backend beside d3d12.cpp, porting the existing shaders (HLSL → WGSL). PBR,
   texture packs and HD movies follow later.
5. **Audio.** The mixer into an AudioWorklet (WebAudio).
6. **Online.** XNet system link over a WebSocket straight to the mc3.top relay (`proxy.py`'s job, done in-page),
   joining the room the player clicked on mc3.top, signed in with their account.
7. **Saves.** `hdd` in OPFS; cloud saves through the existing `/api/cloud`.
8. **Ship on mc3.top.** `/play` page with the build; first visit asks for the disc.

## Not doing

- Hosting the game disc or extracted game files on mc3.top for everyone: that would distribute Rockstar's game.
  Each player supplies their own disc.
