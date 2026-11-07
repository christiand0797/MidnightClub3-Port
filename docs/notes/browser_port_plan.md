# Browser port: MC3 running entirely in the browser (WebAssembly + WebGPU)

Goal: open mc3.top, sign in, and play Midnight Club 3: DUB Edition Remix in the browser — nothing installed, no disc
required. The disc image is bundled with the site at `/disc.iso` (served in 20 MiB parts under Cloudflare's file limit)
and copied once into the browser's private OPFS storage on first visit. Everything stays on the player's machine:
mc3.top serves only the port (runtime + recompiled code) and the online services.

## IMPORTANT — Do NOT remove game files from the browser build

The following files are REQUIRED for the browser port and must NOT be deleted or excluded:

- `tools/web/play.html` — the game page (disc download, audio, F1 menu, lobbies)
- `tools/web/gpu.js` — WebGPU executor with shader compilation, pipeline caching, texture upload
- `tools/web/webgpu_backend.cpp` — WebGPU command recorder; records frames to shared memory
- `tools/web/web_audio.cpp` — Web Audio API + AudioWorklet sound output
- `tools/web/web_stubs.cpp` — browser stubs for Windows-only APIs (input, settings, menu, car paint, etc.)
- `tools/web/build_wasm.py` — Emscripten build script for wasm (targets node|web)
- `tools/web/serve.py` — local dev server with COOP/COEP headers; serves `/disc.iso`
- `tools/web/stage_site.py` — stages the site for mc3.top Cloudflare Worker deployment (splits disc.iso into 20 MiB parts)
- `tools/multiplayer/browser/static/relay-client.js` — relay client for online lobbies/chat
- `tools/multiplayer/browser/static/account-ui.js` — account/cloud UI (sign in, cloud saves)
- `tools/multiplayer/browser/play/` — staged site output (index.html, mc3.js, mc3.wasm parts, disc.iso parts)
- `tools/multiplayer/cloudflare/` — Cloudflare Worker for mc3.top deployment (wrangler.toml, src/)

**Do NOT** add `disc.iso` or `disc.iso.*` to `.gitignore`. They are part of the browser build.
**Do NOT** remove `runtime/gpu/pbr_*`, `texture_pack`, `video_hd`, or `shader_cache` from the browser build — they now
have WebGPU equivalents.

## What we start from

- **Generated game code** (`templates/game/generated/game/x86_recomp.*.cpp`, 137 files, ~212 MB of C++) is
  freestanding: it includes only `runtime/cpu/x86_ops.h`, `x86_context.h` and `runtime/cpu/freestanding/*`. Every
  guest access is `base + uint32 address` through the `X86_MEM*` / `X86_PTR` macros. No intrinsics, no inline asm.
- **Headless harness** (`harness/`, Linux): the runtime without a window, GPU, audio or pad: `null_backend.cpp`,
  `null_audio.cpp`, `pad_script.cpp`, `reference_renderer.cpp` (CPU renderer into guest memory), `compat/` (the few
  Win32/Winsock/DXGI names portable sources use). Emscripten is close to this target; the harness is the base of the
  web build.
- **Windows-only pieces replaced for the browser**:
  - `runtime/gpu/d3d12.cpp` → `tools/web/webgpu_backend.cpp` + `tools/web/gpu.js` (WebGPU, full PBR / combiners / fog / depth)
  - `runtime/audio/xaudio2.cpp` → `tools/web/web_audio.cpp` (Web Audio API + AudioWorklet)
  - Win32 window/input → `tools/web/web_stubs.cpp` + `play.html` (keyboard/gamepad into `web_set_pad`)
  - `VirtualAlloc`/`mmap` guest memory → WASMFS with OPFS mounted at `/opfs`

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

## Status (2026-11-06)

- **Step 1 done**: packed guest memory, all generated code compiles with Emscripten 4.0.15.
- **Step 2 done**: `tools/web/build_wasm.py` builds the whole game (36 MB .wasm, 44 KB .js) and boots from the
  bundled disc image: XBE checked against the recompiled release, 22,465 functions dispatched, intro movies, legal
  screens and the title screen.
- **Step 3 done**: `tools/web/play.html` + `tools/web/serve.py`. The page downloads the disc image from `/disc.iso`
  (bundled on mc3.top, split into 20 MiB parts) into OPFS on first visit, then runs the game on a worker thread
  (PROXY_TO_PTHREAD, WASMFS with OPFS mounted at /opfs: the disc copy, logs and hdd saves persist there).
  `serve.py --dev-disc` + `?devdisc` is a loopback-only test path.
- **Step 4 done**: WebGPU renderer with full D3D12 parity:
  - Vertex programs, fixed-function transform/lighting/fog, fog, pixel shader combiners (4 stages, alpha kill/test, specular)
  - PBR: `pbr_brdf_shader.h`, `pbr_controls`, `pbr_device`, `pbr_library_cache`, `pbr_material`, `pbr_pipeline_shader`,
    `pbr_scene`, `vehicle_pbr`, `car_paint` — all ported to WGSL
  - Texture pack replacement: `pack::Find` / `pack::PollImage` — replacement textures override console textures
  - HD movies: `video_hd.h` — HD MP4 replaces Bink movies
  - Shader cache: `shader_cache.h` — compiled shaders kept across runs in IndexedDB
  - Depth handling, blend modes, cull, write mask, render targets, read-backs
  - Pipelines cached by PixelKey/FixedKey; warm-up cache saved to localStorage
- **Step 5 done**: Audio — Web Audio API + AudioWorklet mixer; sound plays through the game's XAudio2/DirectSound path
  via `audio::StartOutput` → `web_audio_render` → AudioWorkletNode.
- **Step 6 done**: Online — Network menu (F1) with account sign-in/sign-out, lobby list, room create/join, chat.
  `MC3Relay.Client` connects to `wss://mc3.top/ws`; `MC3Account.Client` handles sign-in via `/api/login`, `/api/register`,
  cloud saves via `/api/cloud`.
- **F1 menu**: Settings (display, quality, effects, car paint, picture, input presets), Vehicle (tuning profiles,
  ride height, camber, springs, dampers, brakes, grip, engine power), Network (lobbies, chat, sign in).

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
3. **Browser boot, CPU frames.** Same build in a page: disc bundled at `/disc.iso` and kept in OPFS, frames from
   `reference_renderer.cpp` blitted to a canvas, keyboard/gamepad into `pad`. Threads via pthreads (SharedArrayBuffer:
   mc3.top must send COOP/COEP headers on the play page).
4. **WebGPU backend.** `tools/web/webgpu_backend.cpp` beside d3d12.cpp, porting the existing shaders (HLSL → WGSL).
   PBR, texture packs and HD movies now ported too.
5. **Audio.** The mixer into an AudioWorklet (WebAudio) — done via `web_audio.cpp`.
6. **Online.** XNet system link over a WebSocket straight to the mc3.top relay (`proxy.py`'s job, done in-page),
   joining the room the player clicked on mc3.top, signed in with their account — done via F1 Network menu.
7. **Saves.** `hdd` in OPFS; cloud saves through the existing `/api/cloud`.
8. **Ship on mc3.top.** `/play` page with the build; disc image bundled so every user can sign in and play.
