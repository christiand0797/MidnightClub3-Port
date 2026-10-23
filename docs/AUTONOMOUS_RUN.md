# Autonomous continuation — 2026-10-23

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, public GitHub source/development releases, and complete playable content bundles.
PBR implementation must wait until the media pass
and a verified full backup. Existing PBR experiments are inherited; the renderer remains on its current path.

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
