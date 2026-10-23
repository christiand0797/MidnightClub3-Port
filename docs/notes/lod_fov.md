# Level of detail, draw distance and field of view (2026-10-21)

Static analysis of default.xbe (capstone disassembly), the generated recompiled sources
(`templates/game/generated/game`) and the tune files in the Dave archives. The game was not run. All addresses are
guest virtual addresses. Implementation: `templates/game/src/game/lod.cpp`, `templates/game/src/game/fov.cpp`.
Both files passed a standalone `clang++ -fsyntax-only -Wall -Wextra` check with the project's include paths. That
check is not a project build or a link.

## What the lead must wire

| Variable (global, defined in) | Meaning | Range used | Suggested setting |
|---|---|---|---|
| `float g_lod_scale` (lod.cpp) | LOD switch distances and prop draw distance × scale; far clip × min(scale, 2) when > 1 | clamped [0.25, 4] | `[graphics] lod`, 0.5..4, default 1 |
| `float g_fov_scale` (fov.cpp) | game camera vertical FOV (degrees) × scale | clamped [0.5, 2], result [5°, 150°] | `[graphics] fov_scale`, 0.75..1.5, default 1 |

Declare `extern float g_lod_scale;` / `extern float g_fov_scale;` where settings are loaded or changed and assign
them. Both are read live by the hooks. The far clip is the exception: it is applied when a camera manager is created
(next race or free-roam load). Plain floats are used as the task specified. A setting written from the UI thread
while the game thread reads it is a benign race in practice, but it is not formally atomic.

## Projection and FOV

- `sub_002D4AE0` sets the projection: viewport, thiscall. Stdcall args are `(fov_deg, aspect, near, far)`, `ret 0x10`.
  - It stores fov at `+0x178` and aspect at `+0x17C`. Aspect 0 means the viewport's `+0x128`.
  - It computes `t = tan(fov * 0.0087266)` (`0x476EBC` = π/360). It stores `+0x13C = t`, `+0x138 = t*aspect`, matrix
    (base `+0x4`) `m11 (+0x18) = 1/t` and `m00 (+0x04) = (1/t)/aspect`.
  - It also stores near `+0x130`, far `+0x134`, and the side planes `+0x140..+0x15C` (used by the visibility tests).
  - **The FOV is vertical**, and horizontal FOV = 2·atan(t·aspect), so the game is Hor+.
- Viewport rect `sub_002D4F10` sets `+0x128 = (w/h) * [0x4B4E88]`. The pixel aspect `0x4B4E88` is written at
  `0x2D23F0` from the display aspect. Widescreen output therefore already widens the view without stretching.
- Camera base visitor `sub_00138470`:
  - `BlendTime +0x7C`, `BlendGoal +0x80`, `CameraFOV +0x84`, `CameraNear +0x88`, `CameraFar +0x8C`.
  - Aspect is `+0x90`. `IgnoreTimeWarp` is a byte at `+0x94`.
- Clip visitor `sub_00138840`: `UseGlobalClip +0x44` (byte), `Clip Near +0x40`, `Clip Far +0x3C`.
- Chase cameras (`tune/camera/*_chaseN.camtrackcs`) have speed-sensitive FOV:
  - `SSFovMin 40`, `SSFovMax 60` (over `SSSpeedMin..Max` 15..55).
  - POV cameras (`*.campovcs`) use `CameraFOV 50`, `CameraNear 0.1`, `CameraFar 1600`.
- Projection calls from camera code, identified by return address. Only these are scaled:
  - `0x13853A`: `sub_00138510`, fov `+0x84`, aspect `+0x90`, near/far `+0x88/+0x8C`.
  - `0x13879F`/`0x1387BE`: `sub_00138750`, fov = `cam+0x84 * this+0x38 * 1.075` (`0x44AFF8`), on the viewport
    `this+0x28`. It uses global clip `+0x3C/+0x40` when `+0x44` is set, else the camera's own.
  - `0x138E6C`: camera blend, generated as both `sub_00138C60` and `sub_00138CFB`. It lerps `+0x84/+0x88/+0x8C`
    between the cameras at `+0x98` and `+0x9C` by `+0xA4`.
  - `0x2C4201`: camera manager setup `sub_002C4040`.
- Re-apply callers pass the viewport's stored fov `+0x178`, which is already scaled. They are not scaled again:
  - `sub_002C3F70`
  - `0x2C4097` in `sub_002C4040`
  - `sub_002D4F10` (`0x2D5062`)
  - the environment map pass `sub_00331840`, which copies the main viewport (`[0x558BB4]`)
- Left alone:
  - Rear view mirror `sub_000DAF50` (hudRearViewMirrorShared `FieldOfView +0x10` etc. in visitor `sub_000DBA30`).
  - HUD arrows `sub_000D11A0` (fov 40).
  - Env-map cameras `0x331A67`/`0x332D6A` (aspect 1, near 0.25, far 1000).
- Unverified: that `sub_00138750` or `sub_00138510` runs every frame for the active camera. If only setup ran,
  the scale would apply on the next camera change rather than live.

## Model LOD (types with a lodgroup)

- `.type` text (`vehicle/<name>/<name>.type`, cutscene and ped entities) has a lodgroup block:
  `lodgroup { mesh { high N meshes.. D0  med .. D1  low .. D2  vlow .. D3  center x y z  radius r } }`.
  - Example (traffic): `high ... 100.0`, `med none 9999`.
  - The parser is `sub_000E99F0` (levels via `sub_000E9880`, distance stored `fstp [lodgroup + level*4 + 0x10]`).
- Lodgroup layout (ctor `sub_000E9170`):
  - Level objects at `+0x00..+0x0C` and switch distances (float) at `+0x10..+0x1C`.
  - Ints at `+0x20..+0x2C` (init -1); center `+0x30`, radius `+0x3C`, box `+0x40/+0x4C`.
- Type classes:
  - The base ctor is `sub_000E6B50`, vtable `0x4540D0`. It puts the lodgroup at type `+0x10`, so the distances are
    at type `+0x20` and the type sphere is center `+0x40`, radius `+0x4C`.
  - The derived class ctor is `sub_001B5C70`, vtable `0x467540`.
  - Users: vehicles (`0x81EAD`, `0x7CFFA` traffic), entities (`0x131777`), peds (`0x22BD57` "$/ped"), fx, the city
    reflectors and checkpoint objects.
- LOD selector: `sub_000E62B0`, vtable slot `+0x24` in both vtables.
  - It transforms the sphere center and calls the viewport sphere test `sub_002D45C0(x, y, z, radius, &depth)`.
  - The level is the first `i < 3` with `depth - radius <= D[i]`, else 3. Invisible gives level 3.
  - Output is `*level` (byte) and an optional `*dist = max(0, depth - radius)`.
  - Draw is slot `+0x20` = `sub_000E6440`, which uses lodgroup `sub_000E92D0(level, ...)`.
- Sphere test `sub_002D45C0`: thiscall viewport, stdcall `(x, y, z, r, float* depth)`, `ret 0x14`.
  - It transforms by the view matrix at `0x559758`, writes `depth = -z` first, then culls against near/far and the
    4 side planes.
  - It returns 0 outside, 1 inside, 2 crossing a side, 3 near, 4 far.
  - Callers: `0x359E3`, `0xE633D` (the selector), `0x110CC8`, `0x21E789`, `0x2B8E87` (debug graph lines), `0x332065`,
    `0x337ADC`.
- **Implementation:** lod.cpp hooks `sub_002D45C0`. Only when the return address is `0xE6342` (the selector's call)
  does it rewrite `*depth = r + (depth - r)/s`. That exactly equals multiplying all four switch distances by `s`,
  including the visibility cut at a "none" level. The type itself is never written.
- Lodgroup prefetch `sub_000E9260`: thiscall lodgroup, args `(x, distance)`, `ret 8`; called from `sub_000E6D00`.
  It compares distance with the same `D[]` and passes it to the level objects. The hook divides arg 2 by `s`.
- The traffic `.vehicle` `DrawDistance` field: -25..-120 (negative in every file), visitor `sub_0007C350`, offset
  `+0x1CC`, vtable `0x44D848`. No float reader of that offset was found, so it is not used. Traffic visibility comes
  from its `.type` lodgroup (`high <meshes> 100..120`, then `none`).

## Props (city props: trees, signs, poles)

- `tune/prop/<name>.propdata` (742 files) sets `cull_dist` (mostly 200, range 100..400) and `particle_emit_dist`.
- The prop type loader is virtual `sub_0019D980`: thiscall prop type, `ret 8`, vtable around `0x465840`.
  1. Default `+0x9C` = radius (`+0xA0`) × 50 or 25 (`0x44A878`/`0x448E0C`), clamped to 800 (`0x44E82C`).
  2. Then `propData` (`sub_0019D4B0`, `"$/tune//prop"`) parses `cull_dist` into `+0x9C`.
  3. Then the prop manager (`[0x4CB6E4]`) range `+0x303C = max(+0x303C, cull)`.
- Prop manager:
  - Ctor `sub_0019A670`, dtor `sub_0019A630`. The global is cleared at `0x19A644`, `0x19BF27` and `0x19C4D9`.
  - Draw is virtual `sub_0019BF50(pass)`. It uses 40 m grid cells (`0x4B01A4`) and a search radius of
    `(20 + range)/40 + 0.5` cells.
  - When `[0x53F2E0]+0x50 != 1` (probably more than one view), the range is halved and the flag byte `0x510019` is set, which halves every
    cull_dist (×`0x447C18` 0.5).
  - The draw loop is `sub_0019A730` with max = `cull * [0x4B2684]` (1.0) and fade start = `cull * [0x4B2680]` (0.75).
  - The sort comparator at `0x19A1FC` and the per-cell test at `0x19C1E5` read the cull distance directly.
- **Implementation:**
  - Each loaded prop type is tracked after `sub_0019D980` returns, using the original cull, the value written, and
    the `+0xA0`/`+0x94` fingerprint with committed-page checks (the tuning.cpp pattern).
  - Prop types are set to original × s at the start of every `sub_0019BF50`. Entries whose fingerprint or value
    changed are dropped, and the destructor `sub_0019A630` clears all.
  - The manager range `+0x303C` is scaled only for the duration of its own draw call and restored afterwards, unless
    a load changed it meanwhile.
  - Risk: fingerprints are a heuristic, as in tuning.cpp. If a prop type is freed while its manager lives and
    identical bytes are reused, this could write 4 bytes into an unrelated block.

## Far clip / city

- Camera manager setup `sub_002C4040` calls `sub_002C3F70(far)` with:
  - 2000 for 1 view (`[0x53F2E0]+0x54` 0)
  - 800 for 2 views
  - 500 for 4 views
- `sub_002C3F70` re-applies the projection with that far unless the viewport's `+0x12C` bit 0 is set. The camera
  then copies viewport near/far into its global clip (`+0x40`/`+0x3C`).
- lod.cpp multiplies that far by `min(s, 2)` when `s > 1`.
- Depth precision: near is 0.25 there, so a 4000 m far plane loses precision. That is why the cap is 2.
- City geometry is baked per city and time of day: `resources/city/<city>_<tod>.xbck` (+`_fog`), with PVS
  `<city>_pvs.aib`. The geometry descriptor parser at `0xA1EE0` reads `inst_cpv:`, `bounding_sphere:` and `lod`.
  Here `lod` lists render passes (`main`, `reflect`, ...), not distances.
  - No distance LOD for city sections was found. Their reach is set by the PVS, fog and the far clip.
  - Not done: following the frame render `sub_0001DD90` and its box test `sub_002D47C0` (callers `0x1E0C0`,
    `0x1E57A`, `0x1E8CA`, `0x1EB63`) to see whether city chunks have any distance cut.

## Other findings

- `DrawDistance:` at `0x26FA8B` was not followed. It is a debug or format string.
- `cull_dist` / `numlods:` / `lod` at `0x19D4F2`/`0x19DA94`/`0x19DB13` belong to the prop loader (`%s_%d.mesh` LOD
  meshes for props).
- `FarClippingPlane`/`FieldOfView`/`YAxisScaleFactor` at `sub_000DBA30` are the rear view mirror's.
- `MinFOV`/`MaxFOV`/`FovOffset`/`DistScale` at `0x2CE273`/`0x2CE289`/`0x2BE70A` belong to other camera tunables and
  were not followed.

## Verification still needed (user playtest)

- **LOD 2 and 0.5:** traffic and peds should switch and pop at about twice / half the distance, and props (trees,
  poles) should be drawn about twice / half as far.
- **Prop search ring:** at LOD 2 the prop grid search ring is wider, so there are more draws.
- **LOD 1:** behaviour should be identical to now.
- **FOV 1.2:** the chase camera should be wider, with the speed-sensitive FOV still changing with speed. The mirror
  and HUD arrow should be unchanged.
- **FOV live update:** confirm that a change takes effect without a reload. If it does not, the camera projection
  is not per frame (see the unverified item above).
