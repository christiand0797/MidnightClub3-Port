# Car paint lighting and the enhanced car paint mode (2026-10-28)

PBR plan tier T2, first increment. Opt-in: F1 > Quality > Car paint > **Enhanced car paint** (`[graphics] car_paint`,
default off). Off keeps the console's shaders byte-for-byte except the new trailing `carpaint` constant.

## How the console lights a car (car vertex program, e.g. vs_49394CB7F2D74C70 in captures/f9496-*)

| Output | Computation |
| --- | --- |
| position | `v0 + damage morph` (v3, c116, c118+a0), clip rows `c100..c103`, viewport `c58`/`c59` |
| lit colour oD0.rgb | `(c99 + max(N'.-c108, c106.x) c96 + max(N'.-c109, c106.y) c97 + max(N'.-c110, c106.z) c98) * c107`, `N' = (v2.c134, v2.c135, v2.c136)` |
| fresnel oD0.a | `clamp((v2.c102 + c116.z) * c146.w, c146.y, c146.z)` |
| env uv oT2 | linear in the clip-projected normal (`v2.c100/c101`) and clip position, scaled by c146.x/c116.w, c112 |
| paint uv oT0 | same family (`c117.z`): the paint/pearl ramp target is indexed by view angle |
| oT1 | v9 (decal/vinyl UVs) |

Everything is per vertex, and linear except the light floors, so interpolating per pixel alone changes little; the
low-poly faceting comes from evaluating lights at the vertices. The pixel side (carpaint.fx) lerps the vinyl over the
paint, multiplies by 2*v0, then lerps toward the environment target by 2*t2.a*v0.a.

## Enhanced mode (runtime/gpu/car_paint.h)

- Applies to vertex programs whose generated HLSL reads the whole model above (`LightingProgram`, cached per program
  in `VertexShader::car_lighting`), not to quads or pretransformed draws. Separate vertex/pixel cache keys
  (`kCarPaintSalt`, `PixelKey::car_paint`).
- The vertex variant adds TEXCOORD6 = light-space normal (`N'`) and TEXCOORD7 = light-space view vector; the camera in
  object space is where clip x, y and w vanish (Cramer on c100, c101, c103; `CameraFromClipRows` is the tested C++
  mirror).
- The pixel variant keeps the combiner result (paint, vinyl, env reflection, fog), multiplies it by
  `lerp(1, clamp(perPixelLit / v0.rgb, 0, 4), relight)` and adds GGX/Smith/Schlick (F0 0.04) highlights of the same
  three lights times c107, scaled by fog visibility.
- `DrawConstants::carpaint` (appended last, HLSL `float4 carpaint` last in `DC`): x relight, y highlights, z roughness.
- Tests: car_paint (detection, camera solve, D3DCompile of both variants), settings.

## Not done yet (next T2 steps)

- Reflection direction and fresnel are still the console's per-vertex ones (the env target is sampled at oT2).
  A per-pixel version needs the env target's projection (the draws rendering 82F0E000/83008F80).
- Light directions are in the space of c134..c136; that the view vector shares it assumes c134..c136 is the rotation
  part of the same transform. Check in game: highlights should sit where the scene's lights are.
- No HDR, no environment prefiltering, chrome/glass/tyre materials unchanged beyond relighting.
