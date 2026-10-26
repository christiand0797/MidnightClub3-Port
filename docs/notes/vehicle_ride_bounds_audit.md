# Original vehicle bounds and uncovered ride-range checks

2026-10-25. CPU-only source/resource audit and focused fixture extension. No game, GPU worker, runtime asset,
save, setting, coordinator marker or production physics code was changed.

The earlier 188-tune / 20,304 opposite-side render-query run remains valid evidence for its tested radii
(.18/.35/.60 m). Its preserved log is `.local/stock-ride-catalogue-20261024.log`, SHA-256
`a53d7c773b3468694c3a5d74e0e3ec002de81300069f85faf6a89a1257acde4c`. This pass did not rerun it.

## Concrete coverage gaps

The original clearance skin is usually 5 cm, but F355 base/mods explicitly author **8 cm** and SLR base/mods
**9 cm**. Of 188 `.vehsim` files, 162 author .05 m, four author these larger values, and 22 omit the field
and use the .05 m constructor default at `0x121671`. The runtime floor writer already reads `sim+0xD8`;
there is no demonstrated implementation defect requiring a new uniform clearance. The old synthetic fixture's
single .05 m input did not cover these authored values.

The allowed calibrated rim/profile tables yield radii from **0.195384622 m** (rim12/profile33) to
**1.312333345 m** (rim28/profile8). The latter exceeds the old .60 m solver probe. These are the game's
calibrated rim values divided by its profile ratio, not a physical inch-label conversion. The appended profile
ratio is unrelated to a real tire's sidewall percentage of tire width.

## Original-resource inventory

`tools/garage/audit_ride_bounds.py` reads the user's original Dave archives without extracting asset bytes.
It restricts the vehicle resource census to each family's main, `_g` and `_o` bundle, rather than counting
bumpers and other parts as complete cars. The report is
`.local/vehicle-ride-bound-audit-20261025-r1.json`, SHA-256
`09574e0fd91d9131c4fa7b64165329e62747866b9be540d54cd16df81f9b1a7a`.
It pins every measured resource/tune byte hash, the original XBE hash, current garage catalogue source hash,
tool hash and prior solver log hash. It records that the prior solver was not reexecuted.

The XBCK payload begins at file .80 and uses baked pointers: `fileOffset = pointer - header[0] + .80`.
The root's +8 points to the model descriptor and +C to the bundled bound. The parser validates the declared
payload length, type-3 bound, full u32 counts, bounded and nonoverlapping vertex/polygon arrays, finite float3
positions, all four u16 polygon indices, and both positive primary axle node radii. Descriptor +.3808 is the
original type-2/type-4 classification; +.3818/+ .381C select the primary axle radius nodes at node +.14.

The original classifier at `0x121150` uses float32 `minimumY ± .05`. Both bottom comparisons are strict;
the next layer is also strictly **greater than** the upper threshold. A vertex exactly at that upper threshold
belongs to neither set. The seven synthetic parser tests cover this boundary and its float32 rounding,
pointer relocation, front/rear radius independence, nonfinite/corrupt data, overlap and authored clearance.

All **282 variants / 94 families** parsed successfully. The 237 four-wheel variants (79 families) have exactly
four bottom-layer vertices; the 45 type-2 variants (15 families) have two. None overruns the classifier's four
cached slots. Every four-wheel variant's `Y0 + authored skin` remains below its next-layer cap; the smallest
remaining margin is **0.055737972 m**, on the McLaren F1 variants. Minimum bound Y over all variants ranges
from .186342999 to .704429984 m; these chassis-local values are not world road heights.

| Original main family | Skin | Minimum Y | Cap Y | Cap minus skin-floor |
|---|---:|---:|---:|---:|
| F355 | .08 | .186342999 | .411587983 | .145244986 |
| SLR | .09 | .300000012 | .644999981 | .254999965 |
| McLaren F1 | .05 | .280997008 | .386734992 | .055737972 |

All 188 stock wheel tunes omit or author zero `SuspensionOffset`. Of their AirRide blocks, 146 are empty;
42 explicitly author zero front/rear rise/drop and .1 deflate/inflate rates. An empty block does not establish
the controller constructor's rate defaults. The report's zero placeholders for omitted AirRide scalar keys
must be read with `airride_explicit=false`, not as measured runtime defaults.

Reproduction, with a fresh report filename:

```powershell
python3.12.exe -m unittest discover -s tools/garage -p test_audit_ride_bounds.py -v
python3.12.exe tools/garage/audit_ride_bounds.py --report .local/vehicle-ride-bound-audit-new.json
```

## Focused original-instruction tests

`tests/tuning_test.cpp` now combines stock ride steps 6/7/8 (-.125/-.1375/-.15 m), distinct front/rear settings,
the actual minimum and maximum allowed radii, and the representative .35 m radius. For 18 axle/radius cases
and three in-range compression requests each, the actual `0xA7250` contact solver preserves the requested
compression. Both ordinary render caller identities suppress the arch's downward correction, the special
symmetric branch stays zero, physical queries retain their original displacement, disabling tuck restores
the original query, and querying does not change compression or either tune's ride offset.

The actual AirRide mover through its production wrapper independently converges all four channels for three
mixed stock-step front/rear pairs. Every tick remains within that channel's `H .. H+2*abs(H)` range; down and
neutral reach H, up reaches the high endpoint, and another channel's target bytes remain unchanged. Existing
tests retain zero-baseline retail travel, positive baselines, disable-option behavior and extended negative
baselines. The separate original refresh fixture establishes channel-to-axle mapping.

`tests/chassis_floor_fixture.cpp` now supplies .05/.08/.09 m clearance inputs to the actual classifier,
writer, continuation and polygon rebuild. It combines mixed stock6–8/front-only lowering with all three
radii above, retains smaller-tire radius compensation and the next-layer cap, checks the tune skin/cache
unchanged, and verifies the upper vertices and polygon material through the existing checks. These remain
explicit synthetic shapes; the F355/SLR names identify the measured clearance inputs, not an imported visible
floorpan or a rigid-body road simulation. The production wrapper is covered separately by `tuning_refresh`.

Validation: seven parser tests passed; Release `tuning_test` and `chassis_floor_test` rebuilt and focused CTest
`^(tuning|chassis_floor)$` passed 2/2 in .04 s. No production runtime changes were justified by these results.

## Remaining limits

The chassis floor is one plane driven by the mean front/rear ride; pitch/roll and separate visible floorpan
contact still require native observation. Type-2 resources follow the original skip path. Wide/cambered tires,
rims, steering, slopes and extreme profile shape support require the final mesh/world contact transform,
not only a scalar-radius solver check. Smaller tires intentionally retain stock-minus-current-radius floor
compensation, which can reach the stored cap; that policy has not been replaced with an invented underside
measurement. Genuine scrape/spark response, stock/AI isolation in a same-model live pair, and all-car physical
settling remain the bounded native checks in [ground_contact_audit.md](ground_contact_audit.md).
