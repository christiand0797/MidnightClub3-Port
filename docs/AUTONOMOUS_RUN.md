# Autonomous continuation — 2026-10-23

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, public GitHub source/development releases, and complete playable content bundles.
PBR implementation must wait until the media pass
and a verified full backup. Existing PBR experiments are inherited; the renderer remains on its current path.

## Latest checkpoint — 2026-10-25 17:49 MDT

- Maintenance changes are pushed as `f346b2de76a51d514b7521b79d7150b915df5bd1`. The public
  v0.3.2 runtime/release assets and exact source tag remain unchanged. No native C++ change was made today
  after the already validated r9 release.
- One hidden coordinator, PID **9824**, is alive in `waiting-for-game-and-existing-media-workers`, correctly
  waiting on the active installed BlackOps3 client. No MC3 or GPU/media child is running. The new coordinator
  will continue automatically when the game closes; do not start a second worker or take over the human game.
  Logs: `.local/overnight-coordinator-game-pause-20261025-174736.{out,err}.log`.
- Root retired only the exact unchanged owned QA request into
  `.local/overnight_pipeline/live/retired-qa-stop-20261025-1747.txt`. User texture/video stop files and the
  active texture-transaction marker were absent. Old unrelated private markers remain preserved.
- Source-derived textures remain 7,081/7,081 complete. All **25/106** strict movie records still match exact
  original-source, current-model and output hashes; **81 movies / 90,390 frames** remain. The coordinator's
  inherited `progress` and `command` fields can still show the historical 24-movie checkpoint until its next
  worker starts; use `video-progress.json` and the actual live child PID for current evidence.
- Production video scheduling now retains the same child, encoder and decoded originals across game pauses.
  Each NCNN invocation is capped at eight frames; the current invocation finishes, so there is no fixed
  wall-clock latency guarantee. Explicit stop or owner loss exits2 and discards only the in-flight temporary
  movie, preserving published results. MC3, Fortnite and BlackOps3 are recognized; launchers alone are not.
  Texture batch scheduling is unchanged. See `notes/media_game_pause.md` and `notes/video_cooperative_pause.md`.
- Root independently passed **49 coordinator + 38 complete video + 12 planner + 22 transaction tests**.
  Independent review also passed, including a real Windows parent/child owner-handle probe. Root proof is
  `.local/media-game-pause-validation-20261025-root.json`, SHA-256
  `73c593fe015d7bc2af99ae65593b30305565cfa99aa2bbf390209f15b1d2a9b8`.
  Coordinator SHA-256 is `5b0c38d697f8c263488b4c9d9cd979658a038015f4e22df476680509f1dc3762`;
  worker SHA-256 is `4693056a2615fe45a9ca008ffcae1862d565e3ecc8a1f4189be2d0838c16f4b8`.
  Live GPU pause overhead remains unmeasured; the historical ~64 GPU-hour estimate predates smaller batches.
- Native QA is closed and has no active UI owner. Human activity prevented a new launch. The preserved
  San Diego baseline is genuine raw JPEG despite its `.png` filename, with visible loading-wipe overlays.
  `baseline-evidence.json` records its actual format/hash and contextual limitations. A comparable candidate
  capture and accepted review are still missing. Private S copies hold the staged candidate; canonical S
  images, the canonical ledger and real transaction marker remain unchanged/absent. Do not adopt CPU-only
  comparisons as native acceptance.
- The current readiness helper also refuses BlackOps3, SHA-256
  `43163eb2e5476ad68da4008506faea77b6803be099c4bc8844731e5c8c168c48`. The frozen r1 proposal keeps its old
  helper pin and is stale under current checks; build a **new** accepted r2 after actual native proof. Private
  launch/switch helpers also now refuse the installed client. No actual canonical installation occurred.
- The native avoidance-leap capture shows articulated arms/knees; its raw JPEG format/hash are recorded in
  `notes/npc_source_weights.md`. Full-body walking/footplant, all-car contact and sparks validation remain open.
  Private HDD6/settings/log/variant records have a new independent SHA-256-verified backup at
  `G:/MC3_Backups/saves_2026-10-25_1743_post-closeup` (8 files, 249,936 bytes).
- The hourly follow-up now includes BlackOps3 protection and the reviewed pause/proof rules. Continue one
  coordinator-owned strict pass, then both final media audits and a newly verified full-project backup before
  offline PBR generation. The small save backup is not that full-project gate. Runtime PBR remains unchanged.
  Schedule later native QA only after safely coordinating media; never leave it blocking the queue unattended.

## Earlier checkpoint — 2026-10-25 12:05 MDT

- v0.3.2-dev.20261025 is now **published**, ID423384119, exact tag/source08bcab9687e55db8245e149d39fd92db78944dff.
  The9703711byte ZIP has SHA-256 `0a4fb2fa3fb2f22ff46c35e658ec31a85781038578eae2259f397e0982050ed7`.
  Both remote assets and an unauthenticated public ZIP download match. All15 older assets/bodies/tags remain
  unchanged. External control is the r1 directory below; do not rerun its draft uploader or replace its assets.
- Strict movies are25/106, zero failures,90390frames remaining. The old coordinator/worker stopped cleanly at
  RF355; no inference is currently running. Root marker remains `root-r9-closeup-qa-20261025-0604`.
- Native UI sole owner is `texture_visual_review`, performing a short cold BASE/candidate San Diego capture
  attempt after a native avoidance jump with visibly articulated arms/knees. Walking/footplant QA remains open.
  Root must not send simultaneous game input. Preserve its native artifacts/private HDD6 and never touch the
  other7073 QA hardlinks. Canonical personal settings stay restored; fresh11:40 private save/log backup exists.
- New transactional apply/recover helper passes22 Windows fixture tests twice, including independent review,
  prepared-temp corruption, foreign edits, hardlinks, interrupted rollback and durable owner-marker cleanup.
  Apply SHA-256 `961679835dd7293037299d9b1844cc7e5ab87dc6f55651d8c6ee786542f59caf`.
  Coordinator now refuses **any** `.local/reviewed_texture_install/active.json` presence before cached reuse,
  GPU phases, backup or PBR;36 tests pass independently. No real canonical apply/active marker has occurred.
  Obtain actual accepted native evidence/new r2 proposal before apply; never bypass unknown marker contents.
- All25 completed movies/75 exact-PTS frame pairs have actual full-scene/detail visual review. Larger contours
  improve, but fine grain often smooths and specific synthetic-detail examples are documented.150 frame PNGs
  and50 sheets are verified; sampler6/6 tests pass.81 movies and temporal stability remain unreviewed.
- CPU NPC reconstruction from145 original vertices/150 triangles shows coherent limbs and distinct strides
  after root cancellation, without claiming actual post-VS output. Its private proof/report is preserved.
- Next: finish bounded native attempt, close only owned QA, apply S correction only with accepted proof and
  clean transaction checks, then remove only the unchanged owned QA stop marker and resume one coordinator
  with the new gates. Keep completing strict movies; full media audit/verified full backup must precede PBR.

## Earlier checkpoint — 2026-10-25 06:23 MDT

- Public HEAD is `08bcab9687e55db8245e149d39fd92db78944dff`. The new reviewed-correction coordinator
  integration passes34 tests; its actual reviewed-chain report integration passes14 tests. Live coordinator18212
  still runs the preceding loaded code and must restart after the owned stop before image adoption.
- Strict movies advanced to24/106, zero failures. Coordinator18212/child7564 is cooperatively stopping after
  `reward/rf355.bik`; verify live. Root's exact shared marker is `root-r9-closeup-qa-20261025-0604`.
  Do not launch native/GPU work until all old workers exit. No user stop request was cleared or process killed.
- A new tested0.3.2 runtime is staged in owned GitHub **draft423384119**, not published. Exact source08bcab9,
  unchanged r9 EXE7bb7fcd1…, ZIP9703711bytes SHA-256
  `0a4fb2fa3fb2f22ff46c35e658ec31a85781038578eae2259f397e0982050ed7`.
  External control: `G:/MC3_Backups/release-work/runtime-hotfix-0.3.2-dev.20261025-r1/control`.
  All19 ZIP entries/dependencies/content pins, fresh headless archive interoperability and28 CTests pass.
  Both old releases'15 assets/bodies/tags are unchanged. Only root may run `publish_reviewed_draft.py
  --publish-reviewed` after final bounded visual QA and review; the new draft reuses frozen content, not new media.
- Root is preparing fresh NPC close-up and persisted San Diego loading A/B evidence. Guarded launch:
  `.local/launch-r9-closeup-6.ps1`; live file `.local/pad-r9-closeup-live.txt`; captures
  `.local/r9-closeup-captures/`. Private QA is currently closed. Its HDD/settings/log and independent candidate
  PNGs were copied/hashed to `G:/MC3_Backups/saves_2026-10-25_0613_pre-closeup`. Canonical settings stay restored.
  `.local/switch_private_s.py` switches only the two independent San Diego QA copies, refuses live game/media
  workers and checks nlink1 plus exact old/new hashes. Never overwrite the other7073 QA base hardlinks.
- Original NPC audit found no further verified input/weight/order fault across48 vertex ranges/10665 vertices
  from all four city bundles and96 fresh captured vertex cases. Current menu actors are only10.5–34.6console
  pixels high. Native walking/elbow/knee/foot inspection still needs a full-body close-up, ideally80pixels or more.
  Findings: `notes/npc_source_weights.md`. No runtime code was changed by that audit.
- The San Diego-only proposal `.local/reviewed-san-diego-proposal-20261025-r1/` is frozen and deliberately
  pending native review. New `install_reviewed.py` is a dry-run plan builder, not an installer;11 tests pass,
  current7081/7081base audit passes and independent archives are verified. No canonical PNG/archive/ledger was
  changed. Persist fresh native before/after captures before building a new r2; see its documentation/schema.
- One agent is sampling completed strict movies for visual detail/artifacts with bounded CPU decoding.
  This starts no inference and changes no existing media. Other agents have completed draft staging and the
  pending correction plan. Full project inventory is52864files/39.74GB at06:10; backup space is sufficient.

## Earlier checkpoint — 2026-10-25 02:08 MDT

The active reports concern tire bead/rim gaps, inward-only width growth, NPC animation, garage navigation,
loading portraits and lowered stock ride steps6–8. Preserve current user work and original saves.

- Public source is pushed through `cd8a43c`, including the native-tested raw push-command boundary correction
  and evidence below. Width centering, bead deformation, lowered-step visual tuck,
  navigation cadence and original constant-shadow/cursor parity are implemented. All28 CTests pass. The tire
  CPU checks include89,760 original vertex cases,2,288 original resource draws,704 stock negative controls,
  1,144 wrong-brand guards and188 vehicle tunes/20,304 render queries. Runtime PBR is unchanged.
- The current r9 executable is23,617,536bytes, SHA-256
  `7bb7fcd1928156db88e33c78cd5a799ed9b04d4c9c19c9a9bbd6b00e2aa9d1ad`.
  Private native QA used `.local/native-artwork-qa-r7/mc3.exe` (historical folder name), instance6.
  HD startup, rendered menus, garage navigation, profile purchase/save/reload and garage→street transitions
  passed without a recurring fence stall. No physical controls were blocked.
- Native Golf R32 checks confirm both axle selections33/33, calibrated rim0.6096m, tire diameter0.625231m,
  purchased755mm widths and centered hub translations. F3 captures `020022`/`020102` contain two actually
  applied180-vertex0.83→0.975 corrections; a30% preview `020232` contains two0.415532→0.3 corrections.
  Scoped VB identities match the actual draws. One captured tire draw at a different angle is not evidence
  that the other axle failed: visibility and the actual captured frame must be inspected.
- Stock ride steps6/7/8 now have native previews at−0.125/−0.1375/−0.15m. The visible tire's clip-matrix
  components differ by at most0.00005 across a fixed garage camera while the body lowers. Step8 purchase
  retained all four−0.15m offsets outside the garage. This is a bounded Golf check, not all-car road/spark QA.
  Evidence: `.local/r9-native-validation-20261025.json`, SHA-256
  `2caa21db509984ecb2303381961b596323577a2b13892d4a1ee4e9792c835253`.
- Fresh NPC frames `013050`/`013051` in `.local/bead-npc6-trace-captures/` each contain12 indexed skin draws,
  correct c9.w=256 and no analyzer issues. All12 inferred geometry/root pairs show changed relative articulated
  poses across173 nonroot comparisons. This supersedes older zero-c9 captures, but close-up walking/limb
  visual validation remains open. Do not claim every NPC animation is fixed.
- All12,716 original textures have thumbnail visual triage:7,081 active HD plus5,635 excluded originals
  (5,603 masks,23 flat,9 tiny),128 sheets and79 larger outlier pairs. Coverage hashes and exact inventory
  equality were independently verified. No PNG decode or original-alpha mismatch was found. This is complete
  thumbnail coverage, not full-pixel or every in-game UV certification. See `notes/texture_visual_review.md`.
- Loading audit confirms475 original sources/205 bundles. Exact Flash UV/layout reconstruction and six-column
  original overlap registration produced four joined CPU x4plus composites/eight candidate PNGs. All four
  actual CPU jobs completed in471.046s with original-alpha/split/model proofs. Private San Diego native A/B
  shows cleaner continuous face/hand detail; the other three cities still need native comparison.
  No canonical replacement is installed. The QA directory's7073 base hardlinks are read-only by convention;
  never overwrite them. Eight candidate files are separate copies.
- `reviewed_fixes.py` passes14 tests and the actual absent-ledger audit verifies7081/7081 unchanged base
  replacements. Strict coordinator preflight/final supersession auditing and dependency SHA-256 signatures
  are implemented with34 passing coordinator tests. Invalid proofs stop before base regeneration; good mixed
  reports preserve exact coverage/provenance and publish atomically. No accepted correction ledger exists.
  Reversible installation remains open. The current live coordinator loaded the preceding code: restart it
  at an owned cooperative movie boundary before any adoption; preserve the historical base pass.
- Movies are14/106 strict original/model-provenance verified, zero failures,96,474frames remaining at resume.
  After Fortnite closed, native QA requested and received a cooperative movie-boundary stop. The root marker
  `root-r5-native-qa-20261025` was removed only after private QA closed and its contents were checked unchanged.
  One coordinator PID18212 resumed at02:08MDT, child PID7564 (verify live), logs
  `.local/overnight-coordinator-after-r9-20261025-020817.{out,err}.log`. No user stop flag was cleared.
  Wait for MC3 and the installed Fortnite client to close before any new native launch or GPU inference.
- Canonical personal settings were restored and match the pre-QA file byte-for-byte. All original HDDs remain
  preserved; test purchases are in the isolated instance6 copy. Pre-test backup:
  `G:/MC3_Backups/saves_2026-10-25_010415_pre-r5-native6`. Agents exhausted their allowance until03:05MDT;
  their finished work is saved. Avoid repeated quota retries before then.
- Remaining gates: close-up NPC and broader native vehicle checks; other-city loading A/B; authenticated
  correction adoption;106/106 detailed movies and final media audits; new all-file verified full backup;
  offline starter PBR only afterward. Keep current public release assets and personal saves/settings intact.
  Publish a new development release only after its required native checks; public source may advance separately.

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
