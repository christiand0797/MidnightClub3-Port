# DLSS in the D3D12 renderer: what it takes

Status: assessed 2026-10-31, not implemented. DLSS 5 is excluded at the user's request.

## What DLSS needs, and what this renderer has

| DLSS input | In this port today |
| --- | --- |
| Scene colour before HUD and post | No. The title draws 3D, its own glow/blur passes and the HUD into one console framebuffer; `D3D12Backend::Present` only sees the finished frame (`PostProcess`: grade, FXAA, CAS). |
| Depth | Yes, per render target (reversed depth, `GeometryConstants::depth`, `zparams`). |
| Per-pixel motion vectors | No. Nothing renders velocity. |
| Sub-pixel camera jitter | No. |
| Camera matrices | Partly: fixed-function draws carry world/view/projection (`State::transforms`, `kTsView`, `DrawConstants::wvp`); vertex-program draws (cars, characters, skinned) only have their constant registers. |

## Ray Reconstruction

Ray Reconstruction is DLSS's denoiser for ray-traced lighting. The game, and this port, rasterise with baked lighting
(the PBR path included), so there is no ray-traced signal for it to reconstruct. It needs a ray-traced lighting
renderer first.

## Plan for Super Resolution / DLAA

1. **SDK.** NVIDIA Streamline (`sl.dlss`) or the NGX DLSS SDK, D3D12; ship `nvngx_dlss.dll` (redistributable) and
   fall back to today's path on non-RTX GPUs.
2. **Jitter.** Halton(2,3) sub-pixel offset added to clip-space xy of every 3D draw in the shader translator
   (`shaders.cpp`), scaled by the scene target's size; none for screen-space and HUD draws.
3. **Motion vectors.** A second render target written by 3D draws: current minus previous clip position.
   - Fixed function: previous `wvp` per object, keyed by vertex buffer + world matrix + draw signature.
   - Vertex programs: run the position computation a second time with the previous frame's constant registers,
     matched by the same key.
   - Unmatched draws: camera-only vectors from depth and the previous view-projection.
4. **Insertion point.** Find the frame's last 3D draw (before the first screen-space/HUD draw) and run DLSS there,
   upscaling the scene target to output size; the title's glow, blur and HUD then draw at output resolution. This
   means switching the bound target's size mid-frame, the largest risk.
5. **Settings.** F1 → Quality: Off / DLAA / Quality / Balanced / Performance, plus sharpness. Render scale then means
   DLSS input resolution.

## Expected benefit

On an RTX 3050 at 200% render scale the frame is already 4x supersampled, which is cleaner than DLAA. DLSS mainly
buys frame rate: near the same image at 50-67% input resolution. Weaker RTX GPUs gain the most. Wrong motion
vectors (step 3) show as ghosting on cars and the HUD, so the feature must stay off by default until every draw
class has verified vectors.

Estimated effort: several days of renderer work plus validation per city and car type.
