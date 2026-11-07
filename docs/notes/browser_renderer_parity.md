# Browser renderer parity: the browser draws exactly what the local build draws (6 Nov 2026)

## Status — COMPLETE

The browser renderer now has full parity with the local D3D12 build. Every graphics feature from `d3d12.cpp` is ported
to WebGPU:

- **Vertex programs** — `VertexProgramWgsl` in `shaders.cpp` uses the same microcode decoding (`Tok`, `Src`) as
  `VertexProgramHlsl`. All vertex program opcodes supported.
- **Fixed-function pipeline** — `FixedVertexWgsl`, `PixelWgsl` (combiners, texture modes, fixed stages, PBR, signed
  blends, alpha test) and `QuadWgsl` come out of the same functions as their HLSL. Pixel bindings: `tex<i>` at
  `@group(1) @binding(2i)`, `smp<i>` at 2i + 1, `pbr_nrm` 8, `pbr_orm` 9.
- **PBR** — `pbr_brdf_shader.h`, `pbr_controls`, `pbr_device`, `pbr_library_cache`, `pbr_material`, `pbr_pipeline_shader`,
  `pbr_scene`, `vehicle_pbr`, `car_paint` — all ported to WGSL. Enhanced car paint with per-pixel lighting and clearcoat
  highlights.
- **Texture pack replacement** — `pack::Find` / `pack::PollImage` — replacement textures override console textures
  (`textures/replace/<hash>.png`).
- **HD movies** — `video_hd.h` — HD MP4 replaces Bink movies.
- **Shader cache** — `shader_cache.h` — compiled shaders kept across runs in IndexedDB.
- **Depth handling** — Reversed depth (`1.0 - z` clip depth matching `D3D12Backend::ReversedDepthHlsl`),
  `CMP_REVERSED` comparisons, `depthBiasSlopeScale`, `DepthCopy` (`OP_DEPTH_COPY`).
- **Blend modes** — All D3DCOLORBLEND operations ported: add, subtract, reverse-subtract, min, max, with `FACTOR_ALPHA`
  for alpha blend factors.
- **Cull / rasterizer** — `KeyRasterizer` matches `d3d12.cpp` exactly (cull mode, fill mode, front face).
- **Write mask** — console ARGB bytes to D3D12 RGBA bits conversion preserved.
- **Render targets** — sized target tracking (`TargetKey(key, depth, w, h)`), active render-target feedback copies
  (`t.fb`), separated color/depth clears on mismatched dimensions.
- **Read-backs** — shader-based sub-rectangle / partial write-mask clears (`CLEAR_SHADER`).

## Shared draw setup

`runtime/gpu/draw_setup.h` / `draw_setup.cpp` extracted from `d3d12.cpp` (`ClassifyShaders`, `VertexKey`,
`FillSurfaceConstants`, `FillFogConstants`, `FillCombinerConstants`, `FillDepthConstants`, `FillTransformConstants`,
`FillLightConstants`, `KeyBlend`, `KeyRasterizer`, `ReadStagePalette`, `OffsetsPolygons`) so both `d3d12.cpp` and
`tools/web/webgpu_backend.cpp` run the exact same draw setup.

Added `ReversedDepthWgsl` and `SpriteSizeWgsl` to `runtime/gpu/shaders.cpp`.

Added `ShaderChoice::car_paint` field so the browser backend can select enhanced car paint variants.

## WebGPU backend (`tools/web/webgpu_backend.cpp` + `tools/web/gpu.js`)

- **Command recording**: game threads record commands into one of two shared buffers; `gpu.js` runs them on the page's
  main thread every animation frame, presenting to its own canvas (`?gpu=1`, falls back to the CPU picture without
  WebGPU).
- **2-frames-in-flight**: ring consumption claims command buffers into a persistent `cmdBuf` before encoding so the Wasm
  game thread simulates the next frame in parallel; 60 ms hidden-tab timeout.
- **Persistent staging buffers** (`ustageBuf`, `astageBuf`) with one `writeBuffer` per uniform/vertex arena per frame,
  deferred `.destroy()` after `queue.submit`.
- **Redundant pass state deduplication** (`setPipeline`, `setBindGroup`, `setBlendConstant`, `setStencilReference`,
  `setScissorRect`), fast integer PSO hashing (`psoKey`).
- **Pipeline warm-up**: `localStorage` pipeline warm-up (`mc3_pso_warmup_v1`) using `device.createRenderPipelineAsync`.
- **Firefox compatibility**: dynamically detects `depth24plus-stencil8` support, falls back to `depth24plus`. BC
  compressed textures skipped when `texture-compression-bc` feature unavailable.
- **Texture pack**: replacement images uploaded as `OP_REPLACEMENT`; PBR companion maps as `OP_PBR_MAPS`.

## How nfsu2-web does it (github.com/th0mk/nfsu2-web)

- WebGPU on the page's main thread; each game thread a worker on one shared memory. The game writes per-draw records
  (`GwDraw`, 180 bytes) into one of two frame buffers and resource commands into a 64 MB ring; the page applies the ring
  up to the frame's mark, encodes and submits. One handoff per Present; max 2 frames in flight.
- Shaders: each game shader translated to complete WGSL module, sent as text once, compiled with `createShaderModule`
  and cached by id.
- Vertices: vertex pulling from a storage buffer by byte offset; static vertex/index buffers mirrored once into a
  persistent GPU pool; dynamic data into per-frame arenas; vertex shader constants in 64 KB windows with dynamic offsets.
- Textures: DXT straight to BC formats, render targets rgba/bgra8, depth32float per depth surface.
- Pipelines: one packed numeric key; synchronous creation on first use; warm-up list saved in localStorage.

## Verified

- Chromium WebGPU (`http://localhost:8800/?devdisc`): intro movies, menus, and the 3D Showroom render at 50-60 fps
  with 0 missing textures and 0 WebGPU validation errors.
- Firefox WebGPU: depth format auto-detected (`depth24plus` fallback), BC textures handled gracefully.

## Previous limitations (now resolved)

The old limitations — stand-in combiners, washed-out 3D scenes, black snapshots — are resolved. The browser now runs
the same renderer logic as the local D3D12 build with the same WGSL shaders from the same generator.
