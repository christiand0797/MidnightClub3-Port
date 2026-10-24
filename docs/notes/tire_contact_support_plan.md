# Road-normal-aware tire support: scoped source plan

2026-10-23. CPU/source investigation only. No correction, runtime instrumentation, native launch, settings,
asset or physics change was made for this plan. The frozen runtime hotfix and its separate live evidence are
described in [native_hotfix_20261023.md](native_hotfix_20261023.md). The calculations and source identities below
extend [tire_mesh_support.md](tire_mesh_support.md); they do not establish full-car ground clearance.

## Source-backed capture map

Addresses are from original `default.xbe`, SHA-256
`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`. Generated instruction bodies are in
`templates/game/generated/game/x86_recomp.14.cpp`, `.17.cpp`, `.27.cpp`, `.29.cpp`, `.47.cpp` and `.49.cpp`.
These bodies are available locally; no external decompiler or live process was needed for this investigation.

Use **C** for the concrete car, **B = u32[C+8]** for its physics body, **R = u32[C+0x68]** for its render config,
and **M = u32[R+0x1800]** for the visual model. Current production tuning already verifies C/sim/cfg/render/model
relationships; a new observer should additionally require `u32[M+4] == C` and the current wheel pointer from B.

| Needed datum | Original source / exact field | Capture limit |
|---|---|---|
| Axle profile / tire brand | R `+0x11B4/+0x11B8`, brand `+0x11B0` | Saved extended profile numbers are not stock mesh indices; production resource aliases must be resolved. |
| Selected tire resource handles | At `0xFBEB5..0xFBF01`, and refreshed at `0xFD86E..0xFD8BD`, select `DB=u32[0x509E1C]`, table `u32[DB+0x2C]`, descriptor `u32[table+4*profile]`, items `u32[descriptor]`, handle `u32[items+4*brand]`. Store front/rear handles at **u32[M+0x18] +0xC4/+0xC8**. | `u32[M+0x18]` is a resource/model description, not M or R. The special branch at `0xFD82A..0xFD86C` uses DB+0x30 and R+0x11C8 instead. |
| Actual loaded shader-model payload | Resource wrapper `0x1B5F20`: payload **`u32[handle+4]+0x20`**; it tail-dispatches through payload vtable `+0x18`, retaining six stack arguments. | A handle is not the model payload. Null/unloaded handles return unchanged. Payload extent/lifetime must be checked before reading vertices. |
| Asset draw-slot pointers | Offline record +0x30/+0x34 are **payload +0x10/+0x14**, because the record header is 0x20 bytes. | These two slots must remain separate. The exact live slot-selection implementation behind the relocated payload vtable is still unresolved here. |
| Four physical wheels | B `+0xD8` front first side, `+0xDC` rear first side, `+0xE0` front opposite side, `+0xE4` rear opposite side, as consumed in `0xF7170`. | Validate each pointer against the current B wheel list/count and current sim; matching shared tuning data alone does not identify a player wheel. |
| Wheel radius / width | Runtime wheel **+0x100/+0x104**. Constructor/reset `0xA5ED3` copies tune +4/+8; cache refresh `0x105450` updates these values. Tune is `u32[wheel+4]`. | This radius is the scalar solver radius, not a bound on a tilted mesh. Do not overwrite it to force a visual result. |
| Cached ride and compression | Wheel **+0x10C/+0x144**. `0xA6560` changes the cached offset while preserving its compression relationship. `0xA65D0` returns clamped compression minus cached offset. | Neither value alone is a world-space hub height. |
| Contact record / validity | Wheel **+0x48** begins the 0x2C-byte record initialized by `0x1A5530`; point **+0x4C/+0x50/+0x54**, normal **+0x58/+0x5C/+0x60**. The wheel's separate **u8 +0xA0** gates the contact branch in `0xA7B60`. A second initialized record starts at +0x74. | Capture the contact flag, selected record identity, material/shape fields and simulation step together. Do not use a nonzero old normal when airborne. Selection/provenance of the second record still needs tracing. |
| Contact plane | The +0x58 vector is dotted against body-up in `0xA7BAE..0xA7C1C`, then copied into wheel **+0xC0/+0xC4/+0xC8**. The +0x4C point is passed to rigid-body point-velocity query `0x342BF0` at `0xA7DD1`. | A plane can be constructed as unit normal n and `d=dot(n, point)` only after finite/unit/orientation checks and same-step validation. The upstream collision producer and moving-surface lifetime are not fully traced here. |

The ordinary resource-handle selection is already exercised by the generated-original `profile_mesh` fixture;
it does not execute the eventual shader-model virtual draw or prove which of its two slots is visible.

## Where the wheel matrices become available

`0xFCF20` calls `0xF7170` at `0xFCF38`. Inside `0xF7170`, for each axle the first-side descriptor is
`D = u32[u32[M+0x18]+0x3818+4*axle]`; the opposite-side descriptors start at `+0x3838`. Null ends each list.
Descriptor fields **+4 and +8** select two matrix destinations. Their base is **`u32[u32[M+0xD0]+8]`**, with
**0x3C-byte stride** (`0xF7296..0xF72B9`, `0xF7970..0xF798B`). Do not mistake these descriptor indices for
shader-model draw-slot indices.

The source copies each runtime wheel's 3x4 transform beginning at **wheel+0x10**, including its translation
at **+0x34/+0x38/+0x3C**. It applies the width/full-diameter scale, the arch correction, camber plus the wheel's
live +0x158 angle, and hub offset. It passes this through original matrix multiply `0x85930` at **0xF7728**
and **0xF7D4D** to the descriptor +4 destination. The descriptor +8 destination is separately written at
`0xF789D..0xF792F` and `0xF7E0F..0xF7EA1`, with original wheel spin and the opposite-side float32-pi yaw.
The destination roles must be confirmed against the actual draw arguments; do not assume the +4 result alone
is the full tire transform.

Crucially, the multiplier passed into `0x85930` was constructed by **`0x87860(M+0xA0)`** at `0xF7191`.
The generated `0x87860` calculates a general affine inverse, with a determinant/failure branch. Thus these
palette matrices are relative to the model's **M+0xA0** transform. Treating their translation directly as a
world hub is incorrect. `0x85930` uses 12 floats, basis vectors at +0/+0x0C/+0x18 and translation at +0x24.
The later draw path uses a separate **`u32[u32[M+0xD0]+0x0C]`** array with 0x30-byte matrix stride. Trace and
test that conversion/composition before calling any captured matrix the final GPU/world transform.

The ordinary draw routine `0x106F90` reads the front/rear handles, loops the side descriptors and calls
`0x1B5F20` for the tire at return addresses **0x1080C5** and **0x109A9C** (other pass/branch sites also exist).
There the wrapper's `this` is the selected front/rear handle. This supplies a precise future observation point
for the actual six draw arguments, payload and selected slot after resolving the payload's virtual implementation.
The loop counter runs 0..4 across draw passes here; **it must not be labelled the tire LOD index**.

## Safe observation points and unresolved integration

The existing `0xA7250` hook can collect a bounded read-only contact/radius snapshot for verified human wheels.
Its incoming arguments are requested compression, contact byte and a scalar third argument; contact is 1 from
`0xA81EB` and 0 from `0xA8657`. The original caller `0xA7B60` continues after return, calculating contact-patch
positions and other state. Consequently the existing hook cannot claim a completed final contact patch.
The existing `0xA65D0` hook is also too early: final camber, hub offset, spin and parent composition follow it.

A future **read-only wrapper around `0xF7170`**, taking M before the original and observing its checked
destinations after return, can pair completed per-wheel palette values with current contact snapshots. Current
`0xF32A0` is only the config/rebuild boundary and is not sufficient to observe moving wheels each frame.
A scoped observation around the tire-specific `0x1B5F20` callers can then verify which payload/slot/matrix was
actually drawn. Capture step/frame IDs, axle/side, original caller and load generation, rather than joining
asynchronous records by wheel address alone.

These observers can avoid AI changes by retaining original calls, filtering through concrete C/B/sim/cfg/M
ownership, and copying finite checked data into bounded host-owned records. Never mutate the shared tire pool,
descriptor resources, loaded shader payloads or an unrelated car's palette. Shared-preview resource handles
and identical cfg pointers remain insufficient ownership evidence. No production observer is added by this plan.

Only after the missing draw/space/step checks pass should a correction be designed. For an actual world affine
transform (A,t), contact point q and oriented unit normal n, measure `min(dot(n,A*v+t-q))` over referenced
vertices of the visible tire and rim. A proposed move along a verified suspension axis a would need division by
`dot(n,a)` and rejection near zero; adding a world-Y radius difference is not equivalent on a slope. A visual
lift alone disconnects the rendered hub from physical traction. Changing radius globally would also change
torque, inertia, forces, ray lengths and chassis tire-size compensation. Neither is an approved implementation.

## Proposed generated-original fixture

Extend the existing extraction/linkage pattern in `tests/tuning_solver_fixture.cpp` and
`tests/profile_mesh_fixture.cpp`, without replacing original arithmetic with an imitation:

1. Execute actual **0xF7170** with all four wheel pointers, independent axle configs, descriptors and matrix
   storage, plus actual **0x87860, 0x85930, 0x869D0, 0x898C0** and the needed rotation helpers. Stub unrelated
   material/timer branches only at explicit boundaries and fail loudly if an unmodelled branch runs. Test a
   nonidentity parent transform so a missing inverse/world composition fails; assert ABI and x87 balance.
2. Execute actual **0xA7B60 -> 0xA7250**, with controlled contact records and rigid-body point-velocity boundaries.
   Keep actual contact normal/up tests, suspension clamp and contact-patch arithmetic. Existing solver tests
   execute A7250 but do not establish the larger contact producer/consumer path. Include airborne, rejected
   normals, slopes, moving surfaces and stale records; do not turn zero normals into a flat road implicitly.
3. Execute the ordinary resource selection and actual **0x1B5F20** tail dispatch with checked original-format
   tire bytes and a narrowly recorded virtual draw. This proves the selected payload and six argument values.
   After resolving the real shader-model virtual function, execute that function to prove slot selection and
   the conversion from the 0x3C palette to the 0x30 draw matrices.
4. Compare captured original-path transforms against the offline vertex-support calculator for both sides and
   axles, all eight aliased mesh variants, width/profile extremes, camber signs, steer/spin, body roll and ramps.
   Keep slot-specific results and rim coverage. Add two same-model cars with different then identical cfgs,
   resource reuse/destruction and shared garage previews; assert byte-for-byte preservation of the other car.

This is a fixture and instrumentation plan, not an implemented correction or a passing new runtime test.
The already-published hotfix's profile and private-floor validation remains separate from future camber/road
mesh support. All-car road contact, special/bike transforms and motion-dependent scrape/sparks remain native QA.
