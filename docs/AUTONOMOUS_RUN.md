# Autonomous continuation — 2026-10-23

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, public GitHub source/development releases, and complete playable content bundles.
PBR implementation must wait until the media pass
and a verified full backup. Existing PBR experiments are inherited; the renderer remains on its current path.

## Night continuation — 2026-10-23

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
