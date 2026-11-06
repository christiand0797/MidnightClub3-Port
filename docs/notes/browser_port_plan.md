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
