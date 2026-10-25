# Autonomous continuation — 2026-10-23

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, public GitHub source/development releases, and complete playable content bundles.
PBR implementation must wait until the media pass
and a verified full backup. Existing PBR experiments are inherited; the renderer remains on its current path.

## Current continuation — 2026-10-24

The latest user reports are tire sidewall/rim gaps and intersections, width growing only inward, broken NPC
limbs/animation, touchy garage navigation, glitched garage loading artwork, and road penetration at stock ride
steps6–8. Prioritize concrete corrections and original-code regressions; do not claim complete reverse engineering.

- Width centering and per-draw tire bead repair are implemented but **not released**. The source adds owned
  host-side radial deformation; original shared tire resources stay unchanged. CPU checks cover 89,760 original
  vertex cases, 2,288 actual original resource draw checks and raw/converted threaded draw assembly. All28 CTests
  passed after the ride-step/input and constant-shadow/cursor work. See tire_profile_bead_fit.md and tire_width_centering.md.
- Original ordinary tire shoulders reach normalized X=-0.263825; a too-tight initial runtime guard failed
  closed. The corrected .27 guard is in isolated candidate `.local/native-tire-fit-20261024-r2/`, EXE SHA-256
  `14fcdff99fb6e7f94de02e28a2d3add3d0b1ae5150ac6ba79c46e839ae7c8a5c`. Native verification is pending.
  The installed r5 candidate is now `e528b3a1265fdea0b0d212c39fef23f7ac1e9178c2c313d2af711aa713a01606`
  (23,615,488 bytes), built normally with all affected dependencies. It includes the .27 guard and the other
  CPU-tested changes below, but has not yet had a fresh native run. The earlier `585d7edd…` was used by a human
  in instance8; controls were left with them. `--instance9` clamps to8: use an unused instance2–7 for QA.
- The stock low ride offsets are indices6=-.125m,7=-.1375m,8=-.15m. Their visual tuck bypass previously required
  a value below-.15m. The new source applies the cosmetic bypass for every owned lowered axle when tuck is
  enabled; physical queries/travel remain separate. The regression now also passes188 original vehicle tunes,
  both axles and20,304 render queries across original and extended low settings. Native confirmation remains open.
- Garage navigation is integrated with immediate directional pulses,300ms initial repeat and150ms cadence,
  stick hysteresis, a fresh garage-only lease and explicit concrete-street-driver clearing. Raw GUI input,
  action buttons, triggers and driving remain unchanged. Time-grid regressions pass; native menu QA is pending.
- NPC skinning and loading-image failures remain under source/asset investigation. Older constant/state fixes
  alone do not establish correct pedestrian animation. A verified original-XDK constant-shadow parity gap is
  being repaired with an original setter/state-block-reader fixture; it is not yet proven to cause the NPC defect.
  Exact original setter/state-block-reader fixtures now verify the guest shadow, partial uploads, the raw load
  cursor and method-specific component lanes. New F3-only skin diagnostics and a relative-pose analyzer enable
  same-NPC two-frame checks; animation itself is still unverified. The loading audit verified475 images against205
  original bundles; no corrupt PNG/alpha was proved. Pixel registration establishes six shared source columns
  between garage photo panels; exact Flash fill transforms are retained separately. Separate AI inference
  can create a facial seam across that join.
  Visual thumbnail triage is complete for all12,716 original source/dump textures:7,081 HD replacements plus
  5,635 originals (5,603 masks,23 flat,9 tiny),128 sheets and79 larger outlier pairs. The parent verified every
  coverage sheet hash and exact inventory equality. No decode or original-alpha mismatch was found. Eight
  joint-context CPU x4plus garage candidates are complete (471.046 seconds total), with strict split/alpha
  proofs and static improvements; no active image was replaced. Native A/B and approved-fix integration remain.
- Strict original source/model-weight video provenance is now mandatory for the worker and final backup/PBR
  gate. Legacy unverified startup outputs are regenerated. Video23/23 and coordinator27/27 tests pass; original
  RNinja remains verified. The earlier coordinator PID10928 has now exited cooperatively (verify live PIDs).
- At the latest inspection, strict videos were9/106 with zero failures; the GPU worker finished rg55 and stopped.
  Another human game (Fortnite client PID19236 at22:08 MDT; verify live) is running, so native QA and GPU inference
  wait without touching it. The coordinator now also waits/drains for that actual installed client;28 pipeline
  regressions pass. The unchanged root marker `root-tire-bead-native-qa-20261024` was removed after the worker
  exited, and the single new coordinator PID4912 is safely waiting for the Fortnite client to close. Its logs
  are `.local/overnight-coordinator-gaming-20261024-2215.{out,err}.log`. No user stop flag was cleared. Future
  native QA must request another cooperative boundary before launch. A guarded unused-instance6 launcher is
  `.local/launch-bead-npc6.ps1`; it refuses live game/media processes and recent keyboard/mouse activity, verifies
  the candidate hash, backs up and hashes every HDD/settings/log file, and uses fresh hdd8→hdd6 copies.
  Complete all106, final audits and a new all-file verified backup before new offline PBR. Runtime PBR is unchanged.
- Pre-test copies of every HDD/settings and the old EXE/PDB are in
  `G:\MC3_Backups\saves_2026-10-24_105112_pre-tire-fit`. Current human save/settings changes must be preserved.

### Overnight checkpoint — 2026-10-25 01:04 MDT

- Public source is pushed through `f898d0c` (runtime fixes `c0559ca`, scheduling `89b19ab`, loading/CPU tools
  `f898d0c`). Visual/reviewed-fix audit helper handoffs were preserved when agent usage was exhausted; avoid
  respawning them before the reported03:05 reset. Parent checks pass5 visual and13 reviewed-fix tests.
- The scheduler resumed automatically after Fortnite closed and reached14/106 strict movies, zero failures.
  Root requested another cooperative stop with exact marker `root-r5-native-qa-20261025`; r68corv finished
  intact and all movie workers/coordinator exited. Do not clear that marker until this native QA is complete.
- Fresh native instance6 is PID15320 (verify live), launched from the r5 hash above. All HDDs, settings and logs
  were copied and SHA-256 checked first in `G:\MC3_Backups\saves_2026-10-25_010415_pre-r5-native6`; hdd6 is a
  fresh hdd8 copy. No old saves were modified. HD intro and rendered title run without fence failures so far.
  The additive live file is `.local/pad-bead-npc6-live.txt`, captures `.local/bead-npc6-captures/`.
- Staged photo proof: `.local/loading-garage-joined-20261024-r3/`; final split record SHA-256
  `12d86dced46c952b48aedb3ea12474c25aed76c4d6c185424a9d8084a37e54a5`, integrity proof
  `041588adddcf14cfce54faf40c4dc37b1f8fa0e4cb1ad99f4ecc58da081bf4fa`. See loading_art_audit.md.
- `reviewed_fixes.py` is a tested, read-only future audit wrapper with no active ledger or adoption command.
  Scheduler support for approved supersessions must be integrated/tested before installing a candidate, or the
  normal base-pass worker would restore it. Preserve the original pass ledger and archived original outputs.

## Historical night continuation — 2026-10-23

This checkpoint supersedes the historical build/publication counts below.

- The frozen public content release is [v0.3.0-dev.20261023](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.0-dev.20261023).
  Its 13 remote assets, first-launch installation and public HTTPS downloads were verified. The tested
  [v0.3.1 runtime hotfix](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.1-dev.20261023)
  is now public against those immutable content assets: ZIP/sidecar remote digests and an unauthenticated ZIP
  download passed; its exact source tag is `3a0ed68`. Original disc archive
  volumes are also in the source repository. The full frozen 38,043-file checkpoint is published in the separate
  private backup repository. See [public release](notes/public_release.md) and [backup](notes/github_snapshot.md).
- Native ride-only tuning now refreshes immediately and persists through garage/street reconstruction in the
  tested Golf R32. There are 34 profile choices and a ±2 m PC ride range. A follow-up found the numeric profile
  extension also needed rendering-resource aliases. The correction passes actual original mesh-reader tests;
  native '57 Bel Air QA previewed and confirmed added code 33 (97.5%) on both axles and retained it through
  garage → street → garage. Independent fresh-process save/reload remains open.
- **21/21 CTest checks pass**, including the production private chassis-floor hook and original floor writer.
  [Collision ownership](notes/collision_bound_ownership.md) distinguishes private street resources from shared
  garage previews. Native Bel Air QA retained all four -0.361 m wheel offsets outside the garage; the private
  floor hook removed the lowering term (0.330 → 0.000) and preserved the actual 14-vertex floor at original Y
  plus the native 5 cm skin. Actual scraping/sparks, all-car clearance and a same-model player/AI pair remain open.
- The fresh original-derived UltraSharp texture pass and named-pass integrity audit are **complete: 7,081/7,081**.
  All issue lists are empty; 5,603 vinyl masks remain excluded. See [texture proof](notes/texture_second_pass.md).
  The already-published content bundle predates this completed pass.
- Video processing stopped cooperatively after 23 new reward clips were rejected for FFmpeg's default MP4 clock
  approximation. Exact encoder/track clocks, full PTS/duration validation and preservation of completed failed
  candidates are implemented; **18 video tests and 21 pipeline tests pass**. Existing validated clips keep their
  historical provenance. One recovered RZonda candidate was examined externally but remains unadopted; see
  [video timing repair](notes/video_timing_repair.md). The complete 106-movie pass remains unfinished.
- A previous HD-enabled startup stalled on the GPU frame-completion fence. Submission/error checks and safe
  periodic fence-completion checks are now in the installed native candidate (`b3b6bd22…`, source `3a0ed68`).
  It passed **two HD-enabled startups**, including rendered menus and driving, without delayed-fence diagnostics.
  The original stall's underlying cause is still unknown; see [GPU evidence](notes/hd_startup_fence.md).
- Bounded native QA used a new `hdd8` copy of the user's current `hdd7`. All five `hdd7` save files still match
  their pre-test backup. Only the two QA ride settings changed, and they were restored to the preserved zero
  values with a full semantic TOML comparison. The owned QA game was closed; physical input was never blocked.
  See [native proof](notes/native_hotfix_20261023.md).
- The read-only [tire mesh audit](notes/tire_mesh_support.md) decodes all 98 models / 196 separate geometries;
  **28 tests pass**. Wide, cambered shoulders can extend beyond scalar radius. This is evidence for future
  road-normal-aware contact work, not a production no-ground-clipping guarantee. The
  [source-backed capture/fixture plan](notes/tire_contact_support_plan.md) identifies the physical contact
  fields and tire resource/matrix paths; actual virtual draw-slot selection and final world composition remain open.
- The corrected durable coordinator resumed at 23:03 MDT (05:03 UTC), waited for the owned QA game, confirmed
  the texture pass needed no new outputs, and started its single movie worker at 23:10. The removed stop marker
  was root-owned (`root-video-framerate-repair-20261023`); no user stop request was cleared. Do not start another
  GPU worker. Full final audits and an all-file verified post-media backup remain mandatory gates before new
  offline PBR generation; runtime PBR remains unchanged.

## Current priorities

- Expand tire widths further while preserving existing saved selection indices.
- At maximum lowering, keep tire contact on the ground, allow wheels to tuck through the body, and use actual
  chassis road contacts for scraping/sparks. Verify garage preview and driving separately.
- Finish UltraSharp texture coverage without treating categorical vinyl masks as ordinary colour artwork.
- Finish the detailed Real-ESRGAN x4plus movie pass from original Bink sources, with validated atomic publication.
- Keep the complete project and game data in a verified local backup; publish source plus a full snapshot archive
  to the new private GitHub repository where file-size limits permit.

## Live state

- Saved data copied before game control to `G:\MC3_Backups\saves_2026-10-22_autonomous` (5 files, zero copy failures).
- Native executable launched from `templates/game/out/native/mc3.exe`; observed working intro, rendered title
  background, existing Player 1 profile, and profile loading. Further menu checks are ongoing.
- Existing detailed video worker was already running; no duplicate GPU job started. At initial audit 48/106 movies
  had x4plus markers. The complete older HD movie set remains usable while each new movie is generated.
- Three workers cover customization physics/tests, video reliability/detail, and texture coverage/reliability.
- New private repository created: https://github.com/christiand0797/MidnightClub3-Port . Source push and full snapshot
  upload are pending. The upstream remote is preserved; `github` points at the user's new repository.
- Fixed a Windows macro collision in the settings regression (`near` renamed `assert_close`).

## 2026-10-23 checkpoint

- Source commit `abc4caf` is pushed to private GitHub main. Full-project snapshot publisher is prepared separately.
- 38 tire widths (105–755 mm), explicit direct-garage preview ownership, original solver fixtures, and a cosmetic
  arch-fit bypass are implemented. Both front/rear tuning use model data; road scraping and all-car visual validation
  remain open. The user explicitly wants body intersection for wheel tuck, and no intersection with the ground.
- Follow-up corrected signed ordinary-tire arch limits; zero/negative model-space heights must not be rejected as
  invalid radii. The real original query is exercised by tests. Captures are under `.local/autonomous-captures/`.
- Fixed a stale shared-config/simulation association that could retire a newly reused simulation address.
- New functional settings controls: Performance/Balanced/High detail quality presets, input deadzone/threshold
  controls, controller-response reset, and reset of extra vehicle tuning without changing purchased parts.
- Removed the former exclusive live-input test mode after it blocked player input. Physical keyboard/controller
  input is always read; live and timed scripted inputs only add to it. Do not reintroduce an exclusive mode.
  A copied profile in `hdd2` is used for QA; original profile is retained.
- Latest native build and **13 C++ + 13 video Python + 10 texture Python + 10 maintenance Python checks pass**.
- Legacy movie worker stopped at the completed `story/saponint` publication boundary; detailed pass now 49/106.
  No PBR generation or rendering changes started. The durable media/verified-backup/PBR coordinator is ready.
- New user scope: expand usable in-game resolution and settings controls from 480p through16K, with custom
  dimensions and controller navigation. A complete checkpoint is being taken before these larger graphics edits.

## Latest integration checkpoint — 2026-10-23

- Public repository: https://github.com/christiand0797/MidnightClub3-Port (`github` remote), main `8bc7f42`.
  Full project/saves/configuration backups use the separate private `MidnightClub3-Port-Backups` repository.
- User explicitly requests supplied game data in the source repository and a complete playable release. Preparing
  immutable original-disc archive volumes with complete decompression/SHA-256 verification. Portable release work
  adds native first-launch setup, desktop shortcut, pinned content delivery, and offline content assembly.
- Full checkpoint `G:\MC3_Backups\MidnightClub3-Port_2026-10-23_004252_github_76e97c` verified **38,043 files,
  28,281,848,318 bytes**. Every copied file matches its source SHA-256. The initial verification caught worktree
  registration changes in `.git`; refreshing those nine metadata files and rerunning verification passed.
  Manifest SHA-256: `792275c1aa39fc2e616ba7cfad9b10b61ad908a3a0d6bb5ade18a1753a6d0faa`.
  Archive/private upload is running from the verified frozen copy. This is not the post-media/pre-PBR gate.
- Three agents share an isolated managed checkout: graphics/display, garage/physics, and portable installation.
  Independent scene dimensions, staged display confirmation/recovery, camber/offset, suspension/brakes/grip/power,
  the opposite-side wheel-fit caller, and actual AirRide endpoint mapping are implemented for integration.
  Targeted fixtures pass; parent native build and live game verification remain required.
- Airbag implementation uses actual controller `0x1284F0`: for a lowered owned axle with baseline H, down=H and
  up=-H, preserving the original transition/rates. Stock Airbags strings alone do not identify this controller.
- Original profile is preserved. Parent live QA uses the backed-up copied profile in `hdd2` and the native EXE.
- The stopped legacy video worker published 49/106 detailed movies. No new PBR generation has run. The durable
  texture/movie/audit/full-backup/PBR coordinator will start after integration and game QA release the GPU.

## Verification and remaining work

This file is an active ledger, not a claim that the entire game or extreme stance is verified. Native builds and
regression checks follow integration. Road scrape behavior requires real contact and motion, not cosmetic particles
alone. PBR maps will not be enabled in the default renderer during this continuation.
