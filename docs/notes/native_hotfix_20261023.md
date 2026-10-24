# Native vehicle and HD-startup hotfix QA

2026-10-23, 23:03–23:09 MDT. Bounded native validation of the executable built from
`3a0ed6832c9a17938cbb3551c606e309334b0a4f`, published as `v0.3.1-dev.20261023`.
Executable SHA-256: `b3b6bd221d22055860de276447911be25fcda81f3ef60039102145f3bd0a14b2`.
This records local native QA; [publication verification](public_release.md) separately authenticates the release.

The run used instance `hdd8`, copied from the current `hdd7` profile before testing. Controller-file presses
were additive to physical port 0 input. No process-memory writes were used for the observations below.

## Observed results

- A second HD-enabled startup played `rockstar.mp4`, `sdlogo.mp4` and `mc3intro.mp4`, each logged at
  1920×1440 and 29.97 fps. It reached title/profile selection, the main menu, garage and street. The first eight
  submission fences advanced from target/completed 1/0 through 8/7; the session contains no delayed-fence,
  command-submission failure or device-removal report. Menus and settled driving logged about 60 fps.
- In the original garage, the selected **1957 Chevrolet Bel Air** (`vp_belair_57`) reached
  **Wheels → Front-Rear Dimensions → Tire Profile**. Profile ratio **97.5%**, permanent save code **33**, was
  previewed and confirmed for both axles. Read-only snapshots show cfg codes `[33,33]`, render codes `[33,33]`
  and both tire diameters `0.8075897097587585` m through garage → street → garage reconstruction. The alias-table
  installation and the 34-entry list were logged. This checks one added code on one car through the real menus.
- F1 front/rear ride offsets of **−0.361 m** reached both street axle tune blocks and all four runtime wheel
  caches (`−0.3610000014305115`). The street player had the concrete human controller vtable `0x47684C` and a
  matching controller backpointer. The cfg dirty flags had returned to zero after refresh.
- The street chassis at `0x8341F1C0` logged **clearance 0.330 → 0.000**. Its resource root `0x833EE700` occupied
  a private cache entry with key/users/pending **0/1/0**. Bound `0x83412150`, with 14 vertices and bottom indices
  `[0,1,6,7]`, retained minimum Y **0.43284299969673157**, equal within float32 precision to the stored original
  minimum **0.3828429877758026 + 0.05 m native skin**. Its vertex-array SHA-256 stayed
  `2a96bef0e7dccb22dd29af2d3557c3656ec80f01d5879df57b2b38ea29a6844c` across the post-profile/pre-PC-lowering
  preview, lowered garage, lowered street and new garage objects. Lowering therefore did not retract this
  measured floor. The unchanged preview hash alone does not prove the guarded street hook ran; the concrete
  street ownership/cache snapshot and matching chassis log supply that evidence.
- A bounded four-second RT press was logged. The operator observed roughly 60 mph before automatic garage
  re-entry; speed is an operator observation, not a value recorded in the JSON probes. The reconstructed garage
  car retained profile code 33 and the same diameters and floor hash. The process ended normally, `Exit (0)` at
  378.750 seconds. No claim of observed sparks is made.

## Evidence and preservation

Local evidence, retained outside source/release assets:

| File | SHA-256 |
|---|---|
| `.local/hotfix8-native-verification.json` (consolidated assertions) | `72f2caaebdbffe08e19eaf7399282386743d6fb84ed8a43be9685a2730f87fce` |
| `templates/game/out/native/logs/mc3-8.log` | `67372024a5600cf2feb804d0a63b826909923d34f2a3a90dd7bc46471246b3d7` |
| `.local/hotfix8-profile33-preview.json` | `4c1ce87a95809e3d3709af259ece85804bea4d3484916fd30d3c895d6451ad3f` |
| `.local/hotfix8-lowered-street.json` | `428c16716f7b1a701ad9320c6a3b18fd98530eb1a6483663602f6b5d8b6ab211` |
| `.local/hotfix8-lowered-street-tuning.json` | `8f717d13911da8f1f06c4842e381067389c6107ae8f9b27c3f11383854e8127d` |
| `.local/hotfix8-profile33-garage-reentry.json` | `0d2ac852f866ec1ea5c6da1beaff5d2f2ccb9045afd8aa821c294695dc18443e` |

After closing the game, all **five hdd7 save files** were independently SHA-256 compared with
`G:\MC3_Backups\saves_2026-10-23_2300_pre-hotfix-qa\hdd7`: identical paths and hashes. Current `settings.toml`
was parsed and compared with that backup: semantically identical, including both ride offsets restored to **0**.
The two temporary PC ride offsets were restored; the user's hdd7 selected parts and other settings were
preserved. The profile-33 confirmation affected only the copied hdd8 session. Native saves/personal settings
are not package assets. The durable media coordinator resumed
its movie phase after the QA process closed.

The existing 21-check CTest run covers the corresponding native sources. An independent review during this
continuation reran `profile_mesh`, `chassis_floor` and `tuning_refresh`: **3/3 passed**, with no new actionable
regression found. See [profile mesh fixtures](garage_profiles.md), [floor guards and fixtures](ground_contact_audit.md),
and [HD fence diagnosis](hd_startup_fence.md) for the separate offline evidence.

## Remaining scope

This is not all-car or all-profile visual certification. A same-model player/AI pair, all four rendered tire
bottoms against actual road normals at extreme width/camber/steering, front-only/rear-only and airbag travel,
visible floorpan alignment, and actual moving scrape/spark response still need their own bounded validation.
The two nonplayer cars in the street snapshot were different models, so their presence does not prove
same-model isolation. Code-33 persistence was observed across object reconstruction within one process;
**fresh-process save/reload was not tested**. Two successful HD starts do not identify the earlier GPU stall's
underlying cause or guarantee every future startup.
