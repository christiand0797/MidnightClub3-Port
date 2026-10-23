# Race HUD: minimap black square and off-screen "PRESS BACK" popup

Updated 2026-10-21. This is a source and capture audit. The game was not built, launched or driven. The edits are in
`runtime/gpu/d3d12.cpp` only, in the frame-capture code. Draw rendering, `DrawConstants` and the HLSL cbuffer did not
change.

## Evidence used

- Race captures `templates/game/out/native/captures/204913` (2026-10-20 20:49) and `211022` (21:10). The other captures
  (`0133xx`–`0144xx`, 2026-10-21) are free-roam or menu frames with no race HUD.
- Guest frame: 720x480 `LIN_A8R8G8B8`, target `03D4C000`. The title chose 720x480 itself. In `CreateDevice` the
  present parameters say 720, and the title then picks its widescreen HUD layout tables when the render-target width
  is 720. For example, `cmp [0x4B4D60],0x2D0` at `0xBA9B1`, `0x167674` (which sets the Flash variable "widescreen"),
  `0xB8770`, `0xC455A` and `0xD1504`. The runtime applies **no** HUD or widescreen x transform. `app::Widescreen()`
  only sets the console's display flags (`d3d8.cpp` DisplayFlags 0x10000 and `xboxkrnl.cpp` VideoFlags) and the 16:9
  present rectangle (`d3d12.cpp` present). The 720x480 frame is scaled uniformly (`RenderScale`), then stretched to
  16:9 when it is presented. Each HUD position therefore comes straight from the title's own matrices and viewport.
- **Snapshot BMPs hold alpha.** `CapSave` writes 32-bit top-down BMPs. Pillow opens them as RGB and drops the alpha.
  To read alpha, parse the file yourself: pixel offset at byte 10, BGRA, top-down. The helper is
  `<scratchpad>/hud/bmpa.py`. Earlier notes said the alpha could not be checked. It can.

## 1. Minimap black square

Draws, capture 211022 (204913 numbers in brackets):

| # | What |
|---|---|
| 1229 (819) | `clear flags 80 color 00000000`: alpha-only clear (D3DCLEAR_TARGET_A). No rects, so it clears the viewport, which is the minimap's 128x128 at (47,323): `c-38 64 -64`, `c-37 111 387` |
| 1230 (820) | ring frame, P8 atlas `83655C80`, SRCALPHA/INVSRCALPHA, cw F |
| 1231 (821) | alpha mask, P8 atlas `83645B80`, blend off, **cw 8** (alpha only) |
| 1232 (822) | dark disc fill (8x8 white texture times 0.086 grey), DESTALPHA/INVDESTALPHA, cw 7 |
| 1233 (823) | map `836CAE80` (DXT5), DESTALPHA/INVDESTALPHA, cw 7 |
| 1234–1240 | player arrow and opponent blips, same viewport |

Findings:

- The frame alpha after the HUD (`1242_83D4C000.bmp`, read with alpha) is a clean disc inside the square and 0 in its
  corners. So the mask (1231) and the DESTALPHA blends (1232, 1233) translate correctly. The map and blips are clipped
  to the disc in the captured image.
- The square's corners are **exactly (0,0,0)** over exactly the 128x128 viewport (47..174, 323..450). The pixels
  around them are the scene (about 61,64,67). Only the 1229 clear writes that colour there: the old renderer cleared
  all four channels to the clear colour 00000000, not alpha alone. That is the whole black square. The ring's soft
  grey shadow (1230) is blended over that black.
- Both race captures **predate the fix**. `MaskedClear` (`d3d12.cpp`, `ClearTarget` → `MaskedClear`: a full-screen
  triangle with RenderTargetWriteMask = flags>>4, scissored to each clear rect) was patched in by the scratchpad
  `clear_patch.py` at 21:42 on 2026-10-20, after the 21:10 capture. `mc3.exe` (2026-10-21 03:03) includes it.
- I re-read the fix against the current source and found nothing wrong:
  - The flag-to-mask mapping is right: Xbox R 0x10, G 0x20, B 0x40, A 0x80 map to D3D12 bits 1, 2, 4, 8.
  - The clear covers the same scaled rect as the draws' scissor: `ScaledRect` and `SetViewportScissor` use the same
    uniform scale and truncation.
  - The root signature is shared, and the next draw rebinds the PSO, root constants, tables, viewport, scissor and
    targets.
  - The threaded backend sends the viewport before the clear: `Clear` → `SendState`, and the `vsc`..`rt` range
    contains `viewport`.
  - The title clears only through the HLE `D3DDevice_Clear`. The push-buffer walker has no NV097 clear method.
- Texture pack: the minimap's mask and ring are P8 textures, which the source corpus does not cover. None of the 16
  replacements that exist only as dumps is a minimap texture, so no HD replacement can change the mask alpha.

**Status: no further code change.** The fix that is in the source should remove the square. If the user still sees a
square on a build made after 2026-10-21 03:03, take a race capture with `XBE_CAPTURE_SNAP` set to the clear..map
range (e.g. `1229-1233`; the numbers change per frame, so look for `clear ... flags 80`). Then read the alpha of each
snap:

- If the alpha is not 0 in the corners after the clear, the masked clear did not run.
- If the alpha is 1 in the corners after the mask draw, the mask draw is wrong.

## 2. Off-screen "PRESS BACK" popup (left edge)

- Draw: **211022 #1241, 204913 #833**. It is the last HUD draw of the frame, right after the minimap. 4-vertex quad,
  FVF `0x142` (XYZ | DIFFUSE | TEX1), so it goes through the fixed-function world*view*projection path. The viewport is
  back to the full 720x480 (`c-38 360 -240`, `c-37 360 240`). Blend SRCALPHA/INVSRCALPHA, cw 7, texture `8363BC80`
  (DXT1 128x64): a blue tab with a rounded right end, the BACK-button icon and "PRESS / BACK". Opaque texels: columns
  0–120, rows 6–57.
- Placement in 211022: only x 0..3 and y 67..111 change between the snap before (1208) and after (1242). That strip is
  the tab's white right rim: the visible height of 45 px against 52 opaque rows gives about 0.87 px per texel. So the
  tab spans roughly x = -101..3, which is about 97% off-screen to the left. In 204913 the same draw is entirely
  off-screen. That is two captures 20 minutes apart, both left of the frame.
- Other fixed-function HUD pieces in the same frames land where they belong: the speedo/tach needles (1224/1228),
  which use the same `0x142` FVF and the same full viewport, and the text quads (1210–1220). They share every step of
  the renderer's FF path (`FillTransformConstants`: world·view·projection·viewport, then `FixedVertexHlsl`). There is
  no extra x term, ScreenSpaceOffset only moves the viewport constants by sub-pixel amounts, and no widescreen
  adjustment exists. So a renderer transform bug would have to affect only this one quad. **I found no renderer cause
  in source.**
- The likely cause is game-side. The tab's position comes from the title (Flash HUD / widescreen layout tables chosen
  by RT width == 720) through the world matrix it passes to `SetTransform` (HLE'd, `0x411350`, the only transform
  writer in the D3D library). Possibilities:
  - a widescreen layout value placing the tab left of 0;
  - a hidden or slide-out state that never animates in (60 fps step change: HUD timers);
  - a Flash `widescreen` variable or stage offset the script applies differently from the Xbox.

  None of these can be decided from the existing captures. They carry neither the raw vertices of raw-path draws nor
  the FF matrices: `verts` is printed only for converted-path draws, and every HUD quad here is raw-path.

### Change made: capture instrumentation (d3d12.cpp)

`CaptureRawVertices` (called from `CaptureInputs` when `a.vertices` is empty; `EndDrawCapture` and `CaptureInputs` now
take `sh.fixed_transform`). For every captured small draw (≤ 6 vertices) it adds:

- `v0`..`v3`: each input register's raw value from the stream (`r0(x y z)` floats, `r3(AARRGGBB)` hex for colours);
- for fixed-function draws, `px(x y)` after `r0`: the frame pixel each vertex lands on, computed with the exact matrix
  the shader uses (`c.wvp`, viewport included) and the target size `c.surf`;
- `viewport x y w h`, and the `world`, `view` and `proj` matrices (4 rows each).

How to use it: take a race capture while the popup is showing and find the draw with `t0 ... fmt C 128x64` (the tab)
near the end of the HUD.

- If `r0` is already left of the screen in the title's own coordinates, or `world` puts it there, the title placed it
  there. The next step is game-side: trace the HUD layout or Flash widescreen path at `0xBA9B1` / `0x167674`, or check
  HUD animation timing under the 60 fps step.
- If `world`/`proj` give an on-screen position but `px` is off-screen, the renderer is wrong. Compare `px` with
  `(ndc+1)/2*720` by hand.

Compile check by reading only (no build was run):

- Uses the existing `kTsWorld`/`kTsView`/`kTsProjection` (defined above the capture code), `VertexElement` and
  `VertexBuffer` (`vertex.h`), the `Viewport` uint32 fields and `DrawConstants::wvp[16]`/`surf[4]`.
- No change to `DrawConstants` or the cbuffer.
- Reads are bounds-checked against `VertexBuffer::bytes`.

## What the user should look at

1. Minimap (build after 2026-10-21 03:03): in a race, the corners of the square around the round minimap should show
   the road and scene, not black. If black corners remain, report it with a capture (see above).
2. "PRESS BACK" / racer popup: say whether it is still cut off at the left edge, and how much of it shows: a thin blue
   sliver, or the "PRESS BACK" text half visible. If possible, take a capture (F3 / `CAPTURE` with `XBE_CAPTURE_DIR`
   set) while it is on screen, so the new `v`/`px`/`world` lines show where its vertices come from.
