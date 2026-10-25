# Tire, garage and constant-handling candidate — 2026-10-24

The normally rebuilt local candidate contains the tire bead repair, symmetric width growth, stock lowered
ride-step tuck correction, garage-only direction cadence and proven vertex-constant parity corrections.
It is **not yet a native-validated release**. The current public portable build remains v0.3.1-dev.20261023.

- EXE: `templates/game/out/native/mc3.exe`, 23,615,488 bytes.
- SHA-256: `e528b3a1265fdea0b0d212c39fef23f7ac1e9178c2c313d2af711aa713a01606`.
- PDB SHA-256: `f1944de7563ceed58a8e07480e69f6092722f3c5a7c5f28b14cdf980938c56c2`.
- Preserved candidate: `G:\MC3_Backups\release-work\candidate-tire-npc-r5-20261024`.
- Build log: `.local/build-bead-npc-r5-20261024.log`; the normal build tracks all State/header consumers.
- Full CPU suite: **28/28 CTests**, `.local/final-native-tests-r5-20261024.log`.
- Original-data checks: 89,760 tire vertex cases, 2,288 resource-guard draws and 188 vehicle tunes / 20,304
  visual displacement queries. These are CPU original-path checks, not rendered all-car certification.

The media coordinator drained its ninth strictly verified movie at the requested movie boundary. A human
session in another installed game is running, so no MC3 launch, window focus or GPU inference was attempted.
Physical keyboard/controller input remains merged and untouched. The media scheduler now recognizes that
client as another GPU scheduling owner and waits until it closes; it never stops a game process.

## Next bounded native checks

Use an unused instance2–7, with a fresh copy of the user's latest saves and backed-up settings/logs. Instance9
is clamped to8 and must not be used to claim isolated save testing. Preserve the user's current HDDs and all
changes made in the earlier instance8 session.

1. Confirm HD startup and responsive physical input; observe no GPU fence failure.
2. Verify large/small added profiles meet the rim on front/rear and both sides, then garage→street→garage.
   Require both `garage: tire bead fit` and `vertex: fitted tire bead` diagnostics; a selected numeric code
   alone does not establish that the renderer used the corrected host copy.
3. Check wide tires grow outward as well as inward; explicit offset/camber still work. Check stock ride
   indices6/7/8 with tuck enabled, stock/raised controls, and driving separately from preview.
4. Check immediate first D-pad/stick response, slower held repeats in customization, and normal steering
   immediately after exit. Do not mask the physical devices with a scripted exclusive mode.
5. Capture the same visible NPC two or three times at different short intervals. Check c9=256, bone row
   selection, relative-pose change with `tools/diagnostics/analyze_ped_skinning.py`, and rendered limbs.
6. Capture loading artwork with the source/HD toggle to verify the original Flash overlap and staged joint
   photo candidates. No staged candidate is approved merely because its PNG hashes or alpha pass.

Only after these checks pass should a new exact-source development release replace the recommended runtime.
Preserve the old published assets and pinned complete-content metadata. See the focused tire, navigation and
[NPC investigation](npc_skinning.md) notes for the proven causes and remaining limits.
