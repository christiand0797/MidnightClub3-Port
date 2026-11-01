# DLSS in the D3D12 renderer

Status 2026-11-01: DLSS 4 Super Resolution and DLAA implemented and verified in game on an RTX 3050 (title backdrop,
San Diego free roam while driving: 60 fps, no ghosting on the player car, traffic or HUD). DLSS 5 is excluded at the
user's request; the NGX SDK is pinned at 310.9.16.

## How it works

- **SDK.** `cmake/DLSS.cmake` downloads the NGX headers, `nvsdk_ngx_d.lib` and `nvngx_dlss.dll` at configure time,
  each SHA-256 checked, into the build's `_deps` (never committed: the SDK license forbids putting it under this
  repository's license). `XBE_DLSS=OFF` builds the stub in `runtime/gpu/dlss.cpp`. The DLL and
  `NVIDIA_DLSS_LICENSE.txt` are copied beside mc3.exe and into releases.
- **Frame target.** Any of the last three presented surfaces (the title flips front buffers). Its draws bind a second
  render target, the velocity target (RGBA16F): uv motion, HUD mask, and 1 where a draw's own motion is known.
- **Roles** (`VelocityRole`, `PsoKey::vel`): depth-writing 3D draws are jittered and write motion; transparent 3D is
  jittered and leaves the target; HUD draws (no depth test, no depth write) are not jittered and blend a mask by
  their coverage; screen effects (a 4-vertex quad sampling a render target: glow, speed blur) leave it.
- **Motion.** Vertex programs (cars, characters) run a second time on last frame's constants for the same object
  (`VertexProgramHlsl(..., velocity)` adds `PrevPos` reading `cprev` in b2; objects keyed by program, buffers, range
  and repeat count). Everything else, static world and the PBR path included, gets the camera's motion at Present
  (`kDlssFillHlsl`): world position from depth (z buffer through the inverse view-projection, w buffer from the clip
  w) reprojected with last frame's camera, recorded from the frame's first fixed function 3D draw.
- **Jitter.** Halton(2,3), 8 phases times the upscale ratio squared; `DrawConstants::taa.xy` in the shaders,
  folded into `model_to_clip` for PBR draws. `MC3_DLSS_JITTER=-1` flips the sign DLSS is told (diagnostic).
- **Present.** `DlssFrame` makes motion (RG16F), mask (R8) and depth (R32F) from the velocity target and the frame's
  depth buffer, evaluates DLSS into an RGBA8 output the size of the shown region, and the picture chain grades and
  sharpens that instead of the frame target (FXAA is skipped). Reset on mode or size changes.
- **Settings.** `[graphics] dlss` (0 off, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance) and
  `dlss_preset` (0 driver, 10-13 J-M); F1 > Quality. While on, `app::RenderResolution` renders at the mode's share
  of the output. Releases start with DLAA (`tools/release/default-settings.toml`); without an RTX card it is greyed out.

## Ray Reconstruction

Not applicable: it denoises ray-traced lighting, and this renderer rasterises with baked lighting.

## Open

- One "pipeline creation failed" was seen once while idling in free roam before the velocity outputs moved to the
  end of `VOut` (a plain pixel shader under a car paint vertex shader would not have matched); the log now names the
  pipeline if it recurs.
- Not yet checked: Atlanta/Detroit/Tokyo, rain, replays and race starts with DLSS.
