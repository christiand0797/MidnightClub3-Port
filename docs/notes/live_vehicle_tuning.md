# Live vehicle tuning and garage exit

## Report and confirmed cause (2026-10-23)

The F1 Vehicle numbers changed, but effects appeared only after another garage option refreshed the car.
The extra ride height disappeared on returning to the street.

The human and computer factory wrappers rejected factory addresses at or above `0x84000000`.
Those factories are temporary objects on a guest thread stack; this runtime's stacks begin at `0xD0000000`.
The generated driving caller constructs its factory with `lea ecx,[esp+0x6C]` at `0x2C004C`, then invokes
the build path from `[esp+0x54]` at `0x2C0055`. The garage similarly constructs its factory from a stack
local at `0x13C919` and invokes it from `[esp+0x0C]` at `0x13C960`.

Consequently, the normal factory hooks did not register the resulting human car. A later garage status
callback registered only the preview. The different driving car remained unowned by PC tuning.

Read-only inspection of the old running build confirmed the distinction: street car `0x8338B720` had a
human controller at `0x83435F30`, vtable `0x47684C`, with controller `+0x50` pointing back to the car.
Its saved garage suspension was `+0.275 m` on both axles. Both tune offsets and all four cached physical
wheel offsets were still `+0.275 m`, despite the PC settings containing an additional `+0.500 m` per axle.
The old garage previews had already been destroyed. No process memory was modified for this inspection.

## Implemented correction

Factory memory is validated by committed range, including stacks, rather than by a heap-only address range.
Explicit human versus computer factory ownership is retained. A defensive driving rebind also verifies the
actual human controller type, its car backpointer, and the car's human/AI byte; a shared CarCfg alone must
never promote a computer car.

Recovery requests dirty flags `0x81` on the actual CarCfg. The same-frame original refresh writes the purchased
axle baseline before adding the PC offset; it does not adopt an already tuned value as a fresh baseline.
The original setup then copies the value into all four runtime wheel caches, and the render hook reapplies
camber/offset after a rebuild. Stale simulation aliases are retired before a replacement is bound.

## Native Golf R32 garage-to-street validation: passed

The parent launched the corrected native build, changed the Golf R32's F1 controls, then left the garage and
inspected the actual street car, render data and physical wheel caches. Native EXE SHA-256:
`72ade3b85ea2292c57867f5ae6d635a20304d83d9830742c1c96b85eca12c5ab`.

The purchased ride baseline was `+0.275 m` on both axles. Changing only the PC front ride offset to `+0.204 m`
updated the garage's front caches immediately from approximately `0.270` to `0.479 m`, without changing another
garage part. The rear PC offset was `-0.005 m`, producing `0.270 m` at both rear wheels.

After leaving the garage, a distinct street car `0x8339AA90` owned sim `0x8339AB10` and cfg `0x80EE88D0`.
Its human controller still matched vtable `0x47684C` and its car backpointer. The native readback was:

| Check | Observed street value |
|---|---|
| Front wheel tune and left/right runtime cache | `0.479000002 m` (`0.275 + 0.204`) |
| Rear wheel tune and left/right runtime cache | `0.270000011 m` (`0.275 - 0.005`) |
| Front camber, with `-10.1°` additional PC setting | `-12.6000004°` in wheel render data |
| Front hub offset, with `+0.099 m` outward PC setting | `-0.216499999 m` in the renderer's signed convention |
| Engine power with PC multiplier `×1.49` | `761.688049` in the actual engine tune |
| Airbag targets while fully down | Both fronts `0.479000002`, both rears `0.270000011 m` |

This confirms immediate ride updates and retention of the tested ride/camber/offset/power state through the
garage-to-street transition on this Golf R32. Local read-only evidence is retained in
`.local/fixed-garage-to-street.json`. These are actual guest state and renderer values, rather than settings-file
values alone; no process-memory writes were used for the proof.

## Regression coverage and remaining scope

**19/19 CTest checks pass** (`.local/final-refresh-tests.log`). The new
[refresh fixture](../../tests/tuning_refresh_fixture.cpp) executes the original generated `0xFFD90`, `0x121970`,
`0xF32A0`, `0xFF220`, `0x105450` and `0xA6560` instructions with the production hooks. It verifies four-wheel
cache refresh after a ride-only change, render reapplication, no cumulative offset/multiplier drift, reconstruction
at reused addresses and independent airbag axle targets. The garage regression separately covers high stack
factories, concrete human/AI identity and shared-cfg exclusions; see [vehicle ownership](vehicle_ownership.md).

The **±2.000 m** ride slider limits are implemented, and both extremes pass settings save/load round trips.
This does not establish physical clearance at either extreme on every car. Spring/damping/brake/grip wiring
has code-regression coverage; driving feel, extreme width/camber, every chassis' floor contact and motion-dependent
scrape/sparks still need bounded playtests. The [road contact investigation](road_scrape.md) tracks those limits.

The GitHub release remains **DRAFT** pending the parent's actual first-launch installation QA and publication.
The additional media pass remains ongoing, and PBR conversion remains planned work; neither is declared complete
by this vehicle validation.
