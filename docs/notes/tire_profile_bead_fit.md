# Tire profile bead repair — 2026-10-24

The added profile ratios require a geometric correction in addition to the earlier safe mesh lookup. The
nearest stock mesh has an inner opening sized for its own ratio, while the car's selected rim diameter and
tire diameter use the requested ratio. This mismatch directly produces a gap or an intersection at the rim.

## Original mesh measurements

The original ordinary tire pool has eight profile shapes across eleven brands, each with two separate draw
geometries: **88 models, 176 geometries, 21,120 vertices**. Positions are float3 at byte 0, a signed packed
11:11:10 normal is at byte 12, color at byte 16 and two float UVs at byte 20, with a 28-byte stride. None of
these source vertices has a zero packed normal. The ten special/bike models are outside this ordinary-shape
correction's measured scope.

Each ordinary shape's smallest YZ radius is its stock profile ratio divided by two, within stored float32 mesh
precision. The largest deviation is about 7.3e-6 in normalized coordinates. The outer tread radius stays near
0.5, with the original small bulge reaching approximately 0.50021. For example:

| Requested ratio | Nearest stock ratio | Original opening radius | Required opening radius |
|---|---:|---:|---:|
| 0.300 | 0.415532 | approximately 0.207766 | 0.150000 |
| 0.975 | 0.830000 | approximately 0.415000 | 0.487500 |

All values in this table are normalized before the original width/diameter transform. Increasing or decreasing
the entire tire uniformly cannot correct the opening while preserving both the chosen rim and outer tire size.

## Pure correction and ownership contract

`runtime/gpu/radial_fit.h` maps a point's radius r from stock ratio s to requested ratio t:

`r' = t/2 + (r - s/2) × (1 - t)/(1 - s)`

This places the bead at t/2 and leaves radius 0.5 fixed. X is unchanged. The original tread and small outer
bulge at r ≥ 0.5 are preserved exactly. Ratios must be finite and strictly between zero and one; malformed,
nonfinite or unrepresentable candidates fail before publishing any point/normal change. Exact same-ratio
inputs preserve all valid source position/normal bits, avoiding cumulative quantization.

Normal vectors use the inverse transpose of the radial map's Jacobian. In axial/radial/tangential coordinates,
its scales are 1, `(1-t)/(1-s)`, and `r'/r`. Applying one scalar to the normal would be incorrect for rounded
shoulders. The helper decodes the original signed packed normal, applies these three inverse scales, normalizes
it and repacks it. The caller must preserve UVs, color, topology and ownership.

This helper must run on a **draw-owned host copy** selected by the exact tire vertex-data identity and the
current player/profile context. It must never edit the shared guest buffer then restore it: the render thread
reads vertex data after the title's draw call returns. The helper itself does not install game hooks, select a
car, mutate resources or prove that the queued draw metadata reaches every original tire draw branch.

## Checks run

`tests/radial_fit_test.cpp` independently checks known bead/midpoint/tread endpoints, axial preservation,
reverse mapping, unchanged identity/outer-bulge bytes, signed packed-normal decoding and atomic rejection.
Its general shoulder-normal check numerically differentiates the point map and verifies the repaired normal
remains perpendicular to both transformed surface tangents; it does not repeat the normal formula as an oracle.

The read-only `tools/garage/verify_tire_profile_fit.py` uses the existing bounded pool decoder to extract exact
28-byte source vertex records. It reads the eight stock ratios from the original XBE and the 26 requested
extensions from the current append-only catalogue. It sends a temporary binary fixture to the compiled C++
test, exercising the actual production helper for every brand, draw geometry and selected nearest alias:

- **89,760 vertex cases passed**: 21,120 stock identity cases and 68,640 extended-profile cases.
- All 34 requested profiles are covered across eleven brands and both ordinary draw geometries.
- Source X, color and UV bytes remain unchanged; stock and outer-tread records remain byte-identical.
- The worst resulting bead-radius error is **7.74544e-6 normalized**, consistent with the original stored
  opening precision. Both stock and extended output normals remain finite; repaired normals retain unit length
  within packed-normal quantization.
- The existing original-format/support suite also passes **28/28 tests**.

Local proof is `.local/tire-profile-bead-verification-20261024-final.json`, SHA-256
`4065e170fa5d06977c1f54ab5d5d524aeedfb9f6a41186b9aba0ff96b23bd109`; it records the pool/map/XBE, catalogue,
helper, compiled executable and fixture SHA-256 values with the exact test output. The original pool, map and
XBE are hashed again after execution and must match before a successful proof is published.

Reproduce after compiling `tests/radial_fit_test.cpp` with C++20, optimization and assertions enabled:

```powershell
.\out\tests\radial_fit_test.exe
py -3.12 tools/garage/verify_tire_profile_fit.py --executable out/tests/radial_fit_test.exe --report .local/tire-profile-fit-new.json
py -3.12 -m unittest discover -s tools/garage -p test_tire_mesh_support.py -v
```

The optional `tests/radial_fit.cmake` adds the pure fixture to CTest. The original-asset driver is separate so a
source checkout without the user's supplied loose assets can still run the pure endpoint/normal checks.

## Original resource-guard replay

An initial native profile-33 check exposed a second assumption: the production resource guard allowed only
nominal axle coordinates |X| ≤ 0.251. Actual ordinary high-detail rounded shoulders reach **X = −0.263825**,
while the global maximum is +0.25. The guard rejected a valid original shape before any draw-owned deformation
could run. The measured 0.27 bound now includes that original bulge.

`tests/tire_draw_scope_fixture.cpp --original <temporary fixture>` now replays **all 88 complete original
8,752-byte ordinary shader-model records**, including both draw geometries, through the production resource
guard and scope wrappers. The read-only `tools/garage/verify_tire_draw_scope.py` creates that fixture locally
with the bounded original pool parser. It rebases only explicit shape/table/geometry/index/VB pointer fields
into the six-MiB synthetic guest; vertex positions, packed normals, colors, UVs and indices are not searched
or rewritten. VB physical Data receives the corresponding low fixture address under the fixture's explicit
`VirtualFromPhysical` identity adapter. A recorded virtual shader body is the final narrow boundary after the
**actual original `0x1B5F20` tail dispatcher**. The original `0x106F90` body is represented by its relevant
prologue/six-push ABI recorder; its strong production wrapper and thread-local scope run unchanged.

The complete resource replay passed:

- **2,288 extended draws**: 26 requested profiles × eleven brands × both axles × both tire callers × full or
  nearest-stock fallback render codes. Both original slot Data identities must be present and the requested
  source/target ratios must match; merely seeing any active fit is insufficient.
- **704 stock negative controls** keep fit metadata disabled. Every recorded draw leaves the next draw's
  metadata clear and the entire rebased original resource byte-identical.
- **1,144 wrong-brand guard checks** fail closed. Distinct wrappers for all eight stock profiles and eleven
  brands ensure an incorrect resource lookup cannot pass because every test entry uses the same handle.

Proof: `.local/tire-original-resource-guards-20261024-final.json`, SHA-256
`be40151933e786c4017fe29966dae9917e7bdb6991af4536f2bc973a19f845fd`. Original pool/map/XBE SHA-256 values were
unchanged after execution. No source game bytes are committed: only the reusable test and fixture driver.

```powershell
py -3.12 tools/garage/verify_tire_draw_scope.py --executable out/tests/tire_draw_scope_test.exe --report .local/tire-draw-scope-new.json
```

This compound CPU check would reject the former ideal-cylinder X bound using real shoulder vertices. It still
does not replace a native preview showing the corrected bead on the rendered rim.

These checks establish the bead math and original-data coverage. Native profile previews/purchases, both
axles, different rims, saved reconstruction, correct queued draw selection, and player/AI isolation remain
separate integration checks. Width centering is a different original hub-placement rule and is not changed by
this helper. No all-car road clearance or scrape/spark claim follows from this bead test.

## Native scope and push-command boundaries — 2026-10-25

An isolated Golf R32 save purchased profile33 on both axles, retained it through garage→street, and loaded it
in a new process. Read-only checks confirm selected/render profile33/33, rim21/21, calibrated rim0.609600m and
tire diameter0.625231m. Both hub fit translations are zero with purchased755mm widths. The native scope
confirms owner/geometry validation and source0.83→target0.975 for both axles.

Normal `LOG_FIRST` uses Info severity, hidden in release builds. Its absence does not show that fitting failed.
Opt-in `XBE_TIRE_FIT_TRACE` now emits bounded Report-level diagnostics. F3 also records actual assembled fits
and scoped VB identities. `.local/bead-npc6-trace-captures/013141/frame.txt` confirms a180-vertex correction;
per-visible-draw coverage and lower-profile visual checks remain under native investigation.

Raw GPU commands may remain pending after a resource-render function returns. The wrapper now parses older
commands before setting its metadata and the resource's last commands before restoring it. The new
`d3d::FlushPendingCommands()` parses existing CPU push packets without a GPU/fence wait. The deferred fixture
distinguishes a previous unscoped draw from the fitted draw, checks independent axle targets and restoration.
The real HLE parser fixture checks scoped metadata on a pending raw draw and idempotent empty draining.

All28 CTests pass after the boundary change. Original resource replay still passes2,288 extended draws,704
stock controls and1,144 wrong-brand guards with original bytes unchanged. New proof:
`.local/tire-raw-boundary-original-resources-20261025.json`.

The r9 private executable SHA-256 is `7bb7fcd1928156db88e33c78cd5a799ed9b04d4c9c19c9a9bbd6b00e2aa9d1ad`.
Fresh F3 captures `020022` and `020102` each contain two actually applied180-vertex tire draws at0.83→0.975;
the latter uses a garage angle with front/rear tires visible. A30% preview (`020232`) contains two actual
0.415532→0.3 corrections. Both extremes were inspected in-game: the sidewall meets the rim; the30% ratio
creates a physically large outer tire because this game's ratio is rim diameter divided by tire diameter.
The game retains its original rim size and overall tire-diameter rule.

Native stock ride previews6/7/8 confirm four suspension offsets of−0.125/−0.1375/−0.15m. At the same camera
angle, the captured tire clip-matrix components differ by at most0.00005 while the body visibly lowers.
Step8 was purchased in the private save and retained all four−0.15m offsets on the street, with profiles33/33.
This covers the Golf R32 and selected wheels; the original-data CPU fixtures cover the other ordinary variants.
It is not an all-car road clearance, scraping/sparks or every-rim native certification.

Bounded native evidence: `.local/r9-native-validation-20261025.json`, SHA-256
`2caa21db509984ecb2303381961b596323577a2b13892d4a1ee4e9792c835253`. All original user HDDs were preserved;
the canonical personal settings match the pre-test copy byte-for-byte. The private QA instance was closed
before the single durable movie coordinator resumed.
