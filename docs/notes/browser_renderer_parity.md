# Browser renderer parity: the browser draws exactly what the local build draws (6 Nov 2026)

## Why the browser build looks worse

The local build renders through `runtime/gpu/d3d12.cpp` with shaders from `runtime/gpu/shaders.cpp`. Every graphics fix
of this project lives there (vertex programs bit-exact with `precise`, combiners, fixed-function lighting and texgen,
fog, render-target pixel offsets and upscaled-target reads, signed blends, depth handling, PBR, texture packs, HD movies).

The browser build (steps 3-4, `tools/web/gpu.js` + `tools/web/webgpu_backend.cpp`) never runs that code: the vertex
stage is the harness's CPU reference renderer (`harness/reference_renderer.cpp`, written for headless tests) and the
pixel stage is a small WGSL port of its `ShadeStages`. Combiner shaders are a stand-in, texture coordinates and fog come
from the reference path, and none of the d3d12.cpp fixes apply. Patching that stand-in would never converge on the local
build; the browser has to run the same renderer logic.

## How nfsu2-web does it (github.com/th0mk/nfsu2-web, runtime/web/gpu_web.c, gpu_proto.h, vsemu_wgsl.inc,
web/src/gpu/encoder.ts, read 6 Nov 2026)

- WebGPU on the page's main thread; each game thread a worker on one shared memory. The game writes per-draw records
  (`GwDraw`, 180 bytes, state copied per draw) into one of two frame buffers and resource commands (texture create /
  upload / free, buffer pool writes, shader text) into a 64 MB ring; the page applies the ring up to the frame's mark,
  encodes and submits. One handoff per Present; the game waits only for the page to claim the previous frame (not finish
  it), max 2 frames in flight; a hidden tab's pending frame is replaced after 60 ms.
- Shaders: each game shader is translated to a complete WGSL module in wasm (`vs_to_wgsl`, `ps_to_wgsl_ex2`, semantics
  copied instruction for instruction from their Metal backend), sent as text once, compiled with createShaderModule and
  cached by id. Vertex shaders run on the GPU ("the CPU interpreter is 3-4x slower in wasm"); unsupported ones fall back
  to CPU-transformed vertices. Fixed function and alpha test/fog are shader code; one binding ABI for every module.
- Vertices: no vertex buffer layouts: vertex pulling from a storage buffer by byte offset with the declaration baked
  into the module. Static vertex/index buffers are mirrored once into a persistent GPU pool (hash table keyed by guest
  address + generation, freed ranges reused after 8 frames); dynamic data goes into per-frame arenas with one
  writeBuffer per arena per frame; vertex shader constants in 64 KB windows bound with dynamic offsets.
- Textures: DXT straight to BC formats (`texture-compression-bc`), ASTC or CPU unpack as fallbacks; render targets
  rgba/bgra8, depth32float per depth surface; half-texel shift per draw.
- Pipelines: one packed numeric key (blend, shader ids, cull, depth mode); synchronous creation on first use, a
  warm-up list saved in localStorage and created with createRenderPipelineAsync at the next start.

## Plan

1. **WGSL from the same generator.** `shaders.cpp` gets a WGSL output beside its HLSL one for every entry point the
   D3D12 backend uses (`VertexProgramHlsl`, `FixedVertexHlsl`, `PixelHlsl`, `QuadHlsl`, the hooks): the same decoding of
   vertex program microcode, combiners and fixed-function keys, only the spelling differs (no multi-component swizzle
   assignment, no scalar swizzles, `select` for vector conditions, bitcast infinities, `@invariant` position for the
   `precise` EQUAL-depth passes). Checked against the HLSL path by running both on recorded draws (DUAL compare).
2. **A WebGPU backend that mirrors d3d12.cpp.** Same structure and decisions: target tracking (`FindTarget`,
   `EnsureTarget`, sized keys), vertex input from `vertex.cpp`, textures from `texture.cpp` (BC uploaded natively),
   DrawConstants as one uniform block, PsoKey-style pipeline cache, the same copy/blit/depth-copy paths, read-backs.
   Recorded as per-draw records + a resource ring as nfsu2-web does; the page side becomes a thin executor.
3. **Vertex programs on the GPU** (falls out of 1+2): raw vertex buffers mirrored into a GPU pool, constants in
   windows; the CPU reference path stays only as the comparison oracle.
4. **Same extras as the local build**: the F1 settings menu (ImGui draw lists through the same records), HD texture
   packs, PBR, HD movies.
5. **Performance**: pipeline warm-up list, frame handoff on claim, 2 frames in flight, per-frame arenas.

Milestone order: 1 (vertex programs first: most draws), then 2's draw path for the menus, then 3D scenes, then targets
and effects, then 4.
