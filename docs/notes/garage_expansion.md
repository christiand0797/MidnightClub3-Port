# Garage and live vehicle tuning — 2026-10-23

This is the current continuation of `garage_tuning.md` and the binary map in `customization_system.md`.
The user explicitly wants tires to remain on the road while the body intersects the tires, including at the
deepest front and rear settings. A collision bound contacting the road must retain the original scrape/spark path.

## Entry points and controls

The original wheel menus retain 38 tire widths, 105–755 mm, 29 ride selections, all 17 verified rim sizes (12–28
inches) and eight profiles. Existing save indices keep their meanings. New PC controls are available from the
visible **Vehicle tuning (F2)** button while the garage updates, or from the **Vehicle** tab in F1 / Back + Y.
Keyboard navigation, controller navigation and clicking the native menu use the same settings. A short-lived
garage hint is refreshed by the original garage update, `0x1519D0`; it consumes no Flash symbols or new part IDs.

The native tuning controls are additional settings in `settings.toml`, applied only to player-owned cars and
explicit garage previews. They do not replace purchased parts in the profile. The reset button restores the
additional tuning while preserving purchased parts and the extended-garage toggle.

| Control | Range / units | Actual destination |
|---|---|---|
| Front/rear ride offset | -0.5 to +0.5 metres | wheel tune +0x48, added to the purchased garage step |
| Front/rear camber | -30 to +20 additional degrees | render +0x12E8/+0x12EC, added to saved Camber0/1 |
| Front/rear wheel offset | -0.3 to +0.3 metres per side | render +0x12D8/+0x12DC, positive moves out |
| Front/rear spring stiffness | x0.25–3 | wheel tune SuspensionFactor +0x58 |
| Front/rear damping | x0.25–3 | wheel tune SuspensionDampCoef +0x5C |
| Front/rear brake force | x0.25–3 | wheel tune BrakeCoef +0x34 |
| Front/rear grip | x0.5–2 | wheel tune StaticFric +0x68 and SlidingFric +0x6C |
| Horsepower | x0.5–3 | engine MaxHorsePower +0x04, after purchased upgrades |
| Wheel fit size limit | x1–3 | vehModel TireDiameterLimit, as before |
| Allow wheels to tuck | on/off | only the retail cosmetic wheel/arch fit corrections |
| Airbags follow ride height | on/off | actual AirRide mover endpoints, described below |

The original TireOffset bytes at CarCfg+0x14/+0x15 are not a working substitute for the new offset control.
`0x270BD0` reads a small table and passes it as argument two to `0xFB4E0`; the latter never reads that argument.
It instead returns `min(0, TireExtent[axle] - tireWidth/2)`. The new control changes the hub offset that the retail
wheel renderer actually reads. The renderer subtracts this value along the wheel basis at `0xF76E5..0xF76F9`,
so an additional outward distance is subtracted from the original hub offset. Camber is already a real CarCfg
float and the renderer converts its degrees using `0x44B090` before constructing the wheel rotation.

The older customization table mislabeled wheel tune+0 as SlidingFric. The actual `0xA6DA0` visitor registers
SlidingFric at +0x6C (`0xA6FBC..0xA6FC9`); +0 is the vtable pointer written by the wheel tune constructor. The
grip hook uses +0x6C, and the regression explicitly asserts that both axle vtable pointers remain unchanged.

## Both sides and both axles

Physics travel expansion is per wheel-tune block, without a car-name filter. Beyond the original deepest lowering
of 0.15 m, both SuspensionLimit and TravelExtent increase by the additional lowering. Each axle uses its own
selected offset and original tune. The runtime contact solver still determines the grounded position.

The retail renderer independently pushes tires downward to fit the old arch envelope. Suppressing only physical
compression clamps cannot fix this visual error. There are **three** displacement queries in `0xF7170`:

| Return address | Wheel branch | Caller stack arch limit after the query returns |
|---|---|---|
| `0xF74D5` | special/symmetric first-side envelope | +0x58 |
| `0xF7525` | ordinary first-side envelope | +0x58 |
| `0xF7B7D` | ordinary opposite-side envelope | +0x18 |

Earlier code missed `0xF7B7D`, leaving the opposite wheel vulnerable to the same floor intersection. The current
hook handles all three. It runs the original `0xA65D0`, then makes the cosmetic correction zero only for an owned
player wheel below -0.15 m and with wheel tuck enabled. Ordinary branches receive
`query = archLimit - tireRadius - 0.00001`; the symmetric branch receives zero. Signed ordinary arch limits are
valid. Physical callers continue receiving the original compression-minus-cached-offset result. Physical wheel
compression, radius, shared vehModel limits and non-player cars are not changed by this visual override.

## Live updates and ownership

`TuningBindCar` records a concrete car/sim/CarCfg relationship. Before touching render values, code verifies that
the car still owns that sim and cfg, reads its current render object, and checks committed guest pages. Model or
render changes establish a fresh hub-offset baseline. The `0xF32A0` render-application hook adopts new offsets only
when the original routine actually rebuilt wheel fields (force or dirty bits 0x05). Paint-only changes therefore
cannot accumulate an additional offset. Removing player ownership restores original visual/physical values.

Changing live controls requests the original CarCfg dirty refresh (0x81) once through the car update, `0xFFD90`.
This runs the original cached-wheel and suspension refresh instead of showing a slider that merely edits an unused
float. Purchased upgrades interpolate physics in `0x103C50`; after that returns, the additional multipliers are
rebased to the rebuilt values and applied once. Ride baselines remain owned by ApplyCarCfg. Repeated frames and
repeated upgrade rebuilds do not compound the multipliers. Recycled sims retire their pending/tracked fields and
car bindings on destruction.

## Actual airbags

The earlier guess that CarCfg+0xE1 is the runtime airbag position was incorrect: `0x103A50` is a broad base/mods
parameter blend. The actual pneumatic object is allocated by `0x1226F0` and stored at physicsCar+0x188. It owns the
per-vehicle AirRide tune (vehSim+0x88), a car pointer at +0x08, states at +0x10..+0x1C and four live wheel values at
+0x20/+0x24/+0x28/+0x2C. `0x105450` enables it from CarCfg+0x63. Hydraulics are separate, physicsCar+0x18C and
CarCfg+0x64.

`0x1288C0` updates the pneumatic sequence. Its four calls to `0x1284F0` pass each wheel's value pointer, state,
original upper/lower target and that axle's actual chosen suspension offset as the neutral value. State +1 moves
down to argument four; -1 moves up to argument three; zero returns to neutral. `0x128620` applies the results to
runtime wheels through the original cached-offset setter, `0xA6560`.

With the PC option enabled, any nonzero selected axle height **H** has:

- Fully deflated: **H**.
- Fully inflated: **H + 2 × abs(H)**.
- Stock H=0: the retail endpoints and motion remain available.

Thus -0.35 m moves from -0.35 to +0.35 m. For a deliberately raised setup the interpretation is explicit:
+0.125 m moves from +0.125 to +0.375 m, retaining its chosen height as the lowest position and 0.25 m travel.
An in-flight value is constrained to the newly chosen range, so changing from a lowered setup to a raised one
cannot strand it below its minimum. The retail transition rates, sequencing, equipment ownership and wheel
application remain intact. Switching the PC option off restores the original endpoints. This does not install
airbags on cars that do not own them.

Binary evidence: original converted PE SHA-256
`010e2ce8c8b82bddc51f0ccd008a16135422d8aa6dc19afa79fdcecef83490df`.
Parent REA/Ghidra bundles are `.local/analysis/rea-airbag.json` (evidence
`ev_ac65dae1114bc69e95b9e4d3e756ef52b0928bf8060a57a389d0cc7a9d088ba4`) and
`.local/analysis/rea-customization.json` (`ev_95826515b7486972f69dec8aa9829d73799eaa32e71fb1f172d41f52ceaaf23b`).
Generated original instruction bodies corroborate each argument offset. The visitor decompilation has an early
no-return boundary; its missing tail was read from the generated instructions instead.

## Validation and limits

The production-hook tuning test executes the **actual generated original** wheel contact solver, wheel
displacement query and AirRide mover. It checks independent front/rear settings, both ordinary render call sites,
the symmetric branch, physical-call preservation, disabled tuck, original ownership restoration, cache refresh,
paint-only offset stability, performance multipliers, upgrade rebasing and all four airbag channels.

`tools/garage/audit_vehicle_tuning.py` adds optional original-data coverage without checking game assets into Git.
It reads all original `assets_x.dat*` vehicle tunes, writes only a temporary numeric fixture and invokes the same
native hook test. Current source archives contain **188 base/mods tune files**. The audit passes both axles at
-0.175, -0.35 and -0.85 m, three in-range contact requests, three tire radii and both ordinary wheel-side queries:
**6,768 visual-fit queries**. Omitted TravelLimit/TravelExtent fields use the original constructor defaults of
0.5/0.18 m (stores `0xA6ABC..0xA6AC1`). Some bike/traffic models have no standalone arch values; these are explicitly
reported and use zero only to exercise the generic query fixture. Their actual bundled arch geometry remains a
runtime check.

This numerical coverage proves that hooks are generic and do not silently restrict themselves to one car or axle.
It does **not** prove full tire/body/road intersection behavior for every bundled chassis mesh. Live geometry at
extreme widths/camber and scrape/sparks still needs the parent's game captures. Original chassis-bound contacts
continue through `0x1027D0`, generating the game's scrape sound and spark emitters. No artificial sparks are added
at idle, and tests do not pretend to simulate chassis collision.
