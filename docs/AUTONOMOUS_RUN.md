# Autonomous continuation — 2026-10-22

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, and a new private GitHub repository. PBR implementation must wait until the media pass
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
- Added opt-in `XBE_PAD_LIVE_EXCLUSIVE=1` for deterministic unattended QA. Normal player launches still merge
  keyboard and controller input. A copied profile in `hdd2` was used for QA; original profile is retained.
- Latest native build and **13 C++ + 13 video Python + 10 texture Python + 10 maintenance Python checks pass**.
- Legacy movie worker stopped at the completed `story/saponint` publication boundary; detailed pass now 49/106.
  No PBR generation or rendering changes started. The durable media/verified-backup/PBR coordinator is ready.
- New user scope: expand usable in-game resolution and settings controls from 480p through16K, with custom
  dimensions and controller navigation. A complete checkpoint is being taken before these larger graphics edits.

## Verification and remaining work

This file is an active ledger, not a claim that the entire game or extreme stance is verified. Native builds and
regression checks follow integration. Road scrape behavior requires real contact and motion, not cosmetic particles
alone. PBR maps will not be enabled in the default renderer during this continuation.
