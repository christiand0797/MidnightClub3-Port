# Road contact and chassis scrape investigation

2026-10-23, paused for the higher-priority tire-profile/Vehicle-menu report. This is evidence and remaining work,
not a claim that live driving, every chassis, or extreme camber has been validated.
The [driving ownership repair](vehicle_ownership.md) must be included before comparing garage and street tuning;
the [34-profile extension](garage_profiles.md) keeps physical and visual tire diameter readers consistent.

## Original collision floor

The earlier description of an entirely immutable chassis bound is incomplete. `0x121DE0` loads the per-model
`<car>_bound`. `0x121150` finds its minimum vertex Y, records four vertices close to that layer at physics-car
`+0x190..+0x19C`, and caches the lowest Y (`+0x1A0`), next layer minus 0.05 m (`+0x1A4`), and half width
(`+0x1A8`). `0x121440`, continued at `0x1214F0`, subsequently rewrites those four vertex positions and rebuilds
the bound through `0x2395F0` and its virtual `+0xB0` method. Physics-car type 2 skips this adjustment.

After the tire/ride refresh, `0x105450` calls `0x121440` at `0x105707` with:

```
max(0, mean(stock tire radius) - mean(current tire radius) - mean(cached suspension offset))
```

`0xF11E0` returns twice the stock tire node's radius (`node+0x14`); the caller halves the mean diameters.
The rewritten floor is `min(original minimum Y + tune[0xD8] + argument, next layer minus 0.05 m)`.
The tune constructor `0x121550` sets `+0xD8` to 0.05 m and `+0xDC` (side taper angle) to zero.
For equal stock/current radii, negative ride height therefore **raises the lowest collision vertices inside
the chassis**. It can prevent the visible floorpan from making the expected contact even though the wheel
solver is grounded. A future correction must be scoped to owned player cars, preserve tire-size compensation,
and compare the actual bundled bound against the visible body before changing this argument. No correction
has been installed by this investigation.

## Original spark conditions

`0x1027D0` uses a real contact set (count at `+0`, contacts at `+0x20`, stride `0x80`; normals at set
`+0x38`, then the same stride). It returns immediately after maximum accumulated damage. It sums contact
response vectors through `0x22DE30`/`0x22D2B0` and requires their magnitude above **30** before the spark paths.
The sliding branch additionally requires relative body linear speed above **5**, then checks the contact-normal
dot relative-velocity direction strictly between -0.7 and +0.7 when normal Y is above 0.5, or -0.6 and +0.6
otherwise. These constants correct the older approximate 0.75/0.25 description.

That branch retains the game's `0x36F60` spark emitter, `0x10B130(speed,1,1,0,-1)` scrape sound, material-6
exclusion, per-car `Spark*` properties, integer particle budget and contact-count distribution. The budget is
halved again at each eligible contact; no spark count should be invented just because a ride slider is low.
Roof sparks are a separate path when the chassis up Y is below 0.1. A car resting still must not emit artificial
sparks. A complete generated-original contact regression was planned but deferred before files were added.

## Wheel width/camber limitation

The scalar physical tire radius alone does not bound the lowest rendered vertex of a wide tilted tire.
For an ideal cylindrical tire on a flat road, `R*cos(camber) + width/2*abs(sin(camber))` is the vertical
envelope. Applying this as a mesh shift would require verifying the original tire mesh dimensions, both
side-specific transforms, steering, suspension axis, body roll, and the actual road normal first. That mapping
is still open. The existing both-axle/both-side solver regression does not prove this extreme visual case.

## Bounded live checks still needed

Use a low coupe, sedan, SUV/pickup and motorcycle. Keep original and modified captures for each. On flat road,
check independent front/rear lowering, equal lowering, then a shallow ramp at low and moderate speed. Confirm
each tire bottom, visible floorpan contact, original scrape sound and motion-dependent sparks; record stationary,
no-contact and normal-ride comparisons. Repeat representative widths/camber in both directions and with airbags
fully down/up. Live UI/game control belongs to the parent agent; no capture has been made by this investigation.

Evidence: original `default.xbe` and its generated instruction bodies. Converted analysis PE SHA-256:
`010e2ce8c8b82bddc51f0ccd008a16135422d8aa6dc19afa79fdcecef83490df`.
