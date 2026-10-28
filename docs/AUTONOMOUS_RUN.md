# Autonomous continuation — 2026-10-23

The user authorizes unattended game control, parallel implementation, downloads, a second texture/movie upscale,
a complete project backup, public GitHub source/development releases, and complete playable content bundles.
The latest user request authorizes PBR code/material-system development in parallel as an opt-in path.
New offline PBR asset generation still follows the media pass and a verified full backup. The renderer
remains on its current path until a real PBR integration is implemented and validated.

## Latest checkpoint — 2026-10-28, actual feature controls

- Public `94fc467` includes the input latch and GPU diagnostics. The separate feature build
  `c724425f...` compiled successfully from that exact source, with unchanged consumed source
  inputs. It has not replaced the installed executable or become a published release.
- Native R3 now navigates the original profile/main/garage menus with short keyboard taps.
  The current copied 4% San Diego career loads the expected 1957 Bel Air. F1 observes a
  one-shot $90 → $1,000,090 change, still present after the original save-and-exit flow;
  saved-file interpretation and the garage's cached wallet display need separate checks.
- Native per-car switching retained Bel Air front camber -5.1°, Golf +2.0°, then Bel Air
  -5.1° again. Global master disable exposed the zero fallback, and re-enable restored the
  Bel Air override. Sidecar entries remained distinct. Full parameter visuals and native
  process-restart acceptance are still open.
- Actual street validation found the population gate too narrow: normal career cruising
  uses state 2 / `find_hook` mode 1. The exact read-only player/mission observation supports
  a small 0/1-mode correction; its regression fails before the fix and passes afterward.
  Story/race modes and invalid ownership remain excluded. Fresh native activation is pending.
- R3 was left idle in F1 by a quota interruption. On resumption root verified the owned
  session idle, closed it normally, and observed the same coordinator/video/encoder resume.
  All 53 current canonical personal files remain byte-identical to their current backup.
  Result `9dd07222...` includes independent log/private-after copies and actual Sky captures.
  See [native evidence](notes/feature_native_20261028.md). No new release/install claim.
- PBR geometry research identifies a bounded fixed-function bridge and the actual car morph/
  skinning register snapshot points. Lighting and a real material render path remain open;
  see [geometry evidence](notes/pbr_geometry_bridge_20261028.md). Original-derived PBR assets
  still follow complete strict media and the verified full-project backup.

## Earlier checkpoint — 2026-10-28, bounded feature validation

- Public source `12c4d93` and the isolated feature EXE `bc90298e...` built successfully,
  including the PBR pack preflight tool. Its synthetic CLI fixtures passed. No new executable
  has replaced the installed build, and no new release was published.
- Two private native runs used independent current Bel Air/settings copies. The first hit the
  earlier GPU completion-fence stall during HD startup; the second, explicitly activated run
  rendered HD video, the 3D title world and F1 Display/Career controls without a fence warning.
  This comparison does not establish a focus-related cause or resolve the intermittent stall.
  Both owned windows exited normally; the same media worker resumed and all 53 current
  canonical personal files remained unchanged. See [bounded evidence](notes/feature_native_20261028.md).
- Short keyboard taps could miss guest state reads. Additive window-event latching now retains
  foreground gameplay taps until a real state read, while connection discovery cannot consume
  them. Physical keyboard/controller held input remains enabled; focus loss and F1 UI events
  cannot queue stale gameplay taps. Thirteen focused integrated CPU checks pass.
- Opt-in D3D12 diagnostics add bounded driver messages, startup Present results and descriptor
  counts, DRED failure information and missing allocator/list/Present HRESULT checks. They do
  not change fence/descriptor/upload lifetime policy or claim to fix the stall. This updated
  code still needs its own build and bounded native acceptance before installation.

## Earlier checkpoint — 2026-10-28, feature integration

- World/Career source controls are implemented with actual original population setters,
  one-shot career money edits, token-bound UI commands and normal native save semantics.
  CPU menu/economy/world tests pass, including 480p tabs and disabled/stale-career requests.
  Native visible populations/wallet persistence are still separate gates.
- Per-owned-car PC tuning metadata and source-backed config-copy lineage are integrated,
  with distinct same-model cars, global fallback/master controls and conservative external
  roster handling. Save-game bytes and formats remain unchanged. The runtime is not installed
  until the complete feature candidate passes its own native checks.
- Twelve focused integrated CPU checks pass: input mapping, timing, tuning/refresh,
  per-car store/game broker, economy, population, PBR materials, gameplay menu, settings
  and graphics menu. See [per-car behavior](notes/per_car_tuning.md). This does not yet
  certify the new candidate's native rendering or installation.
- PBR importer/CPU+HLSL BRDF tests now pass. Exact reviewed material bindings, channel/mip
  policy and a CPU pack preflight tool are implemented; geometry/root/PSO/light integration
  remains open. No visible PBR or completed renderer claim. See [PBR system](notes/pbr_system.md).
- A new isolated feature build is under `G:/MC3_Features_20261028`; dependency tools/libraries
  have built without changing the installed executable. Sealed R12 and frozen content remain
  unchanged. New source/build evidence must not inherit R12 native acceptance.
- The prior media processes disappeared without an exit receipt; cause remains unknown.
  Fresh game/marker/worker checks allowed one coordinator 3572/video 15108 to resume the normal
  workflow. All 3,339 old partial/temp/foreign-marker files were verified unchanged. Strict
  movies are 50/106, zero failures; bounded still review remains 46. Discover actual PIDs.

## Earlier checkpoint — 2026-10-27 16:16 MDT

- The user's missing-save report was a profile-routing mismatch. The earlier Bel Air is
  preserved in canonical `hdd7`; the desktop shortcut now selects `--instance 7` without
  changing save functionality or overwriting any career. All 53 current personal files
  have a newly verified independent backup. The later default career remains preserved.
  See [save routing and proof](notes/save_profile_recovery_20261027.md).
- R12 actually completed offline native installation and HD/3D startup, then exited normally.
  Its same movie worker resumed afterward. The read-only post-install audit now verifies
  all 7,188 installed files against the frozen cohort, exact runtime ZIP and V2 inventory;
  report `75651e05...`. Explicit R12 input/F1 acceptance remains open. No release was uploaded.
  See [actual offline results](notes/r12_offline_install_20261027.md).
- After a host reboot, all old PIDs were dead. The incomplete Stokmech encoder/temporary
  files remain preserved; current tools cannot resume that encoder. Fresh exclusion/marker
  checks permitted one coordinator 13084/video 7684 and a new original-derived render.
  Discover actual current identities before scheduling. Strict progress remains 46/106.
- New user scope: working F1 traffic/pedestrian multipliers and an explicit career-money
  editor, per-owned-car vehicle settings with an optional global fallback, and PBR system
  development. Reverse engineering and implementation are in progress. Keep the original save
  system; money must use a one-shot game-thread action, never an automatic startup balance
  override. Preserve immutable R12/release artifacts and test new source separately.
- Remaining media, native visual checks, final verified full backup and offline PBR gates
  remain in force. Runtime PBR is unchanged. Do not repeat completed broad audits or mutate
  frozen release cohorts as live progress grows.

## Earlier checkpoint — 2026-10-26 21:44 MDT

- The immutable corrected bundle is built and accepted: 7,188 payload files, eight chunks,
  14,124,573,292 bytes, manifest `1c8c72b7...`, archive `b84957a3...`, acceptance `92c2fdde...`.
  All TSV path/size/SHA entries bind the frozen payload, with independent chunk/whole hashing
  and native reader PASS. See [bundle evidence](notes/content_bundle_20261026.md).
- Fresh R12 from exact source `c68d964...` completed with no reused objects/libraries/generated
  code. EXE `a08f330c...`, checker `9f80fb07...`, final seal `5e7b5ac2...`; corrected independent
  closure `59b86046...` authenticates all 1,595 consumed dependencies. All 133 generated files
  match canonical bytes. Failed materialization/configuration and the original narrow closure
  receipts remain preserved. See [R12 build seal](notes/runtime_clean_r12_20261026.md).
- The fresh checker passed the actual bundle too. Runtime ZIP `300ea5f0...` is assembled with
  the explicit sealed CRT/content descriptor. Independent private offline inputs are staged at
  `G:\MC3_QA_r12_offline_r1`; 53 protected-file hashes stayed identical. Only the precise private
  shortcut marker is preseeded. No R12 native game has launched or release/upload occurred.
  See [offline preparation and reviewed protocol](notes/r12_offline_qa_preparation_20261026.md).
- Human Black Ops III PID 7612 opened at 21:38 MDT. Native QA is deferred. Coordinator 13372 and
  video 18468 retain Stokmech/encoder 8800 at 2,024/2,210; actual cooperative pause is acknowledged
  and no NCNN remains. Preserve this human-game pause; do not clear/restart it. Discover actual
  games/workers before later native work; use a new gate/context with the reviewed protocol.
- Live strict coverage is 46/106 with zero failures, while the immutable release cohort remains
  44 strict plus 62 historical outputs. All frozen 106 movies/7,081 PNGs already passed the full
  audit; do not repeat it or replace frozen movies as live progress grows. Native/offline tests
  still gate publication. All-106 strict media, final audits and verified full-project backup
  remain pre-PBR requirements; runtime PBR is unchanged.
- New CPU-only r6 review covers the two additional strict Satlcut/Svincint clips: six exact-PTS
  pairs, all 3,410 packet timestamps/durations matching, both overview/detail sheets jointly
  viewed, unchanged input hashes and no sampling failures. Bounded still coverage is now 46;
  smoothed/reconstructed fine texture remains, without a temporal/realism-recovery claim. See
  [r6 review](notes/video_visual_delta_20261026_r6.md).

## Earlier checkpoint — 2026-10-26 16:41 MDT

- Frozen validation completed with zero errors/warnings: all 7,081 RGBA PNGs passed fresh integrity
  decoding and original/reviewed proof joins; all 106 movies / 105,272 frames fully decoded and
  matched exact original frame timing. All 7,391 inventory files and recorded inputs rehashed
  unchanged. Report `ad99118f...` is external in `validation-r1`; see
  [actual frozen validation](notes/frozen_content_validation_20261026.md).
- Provenance remains 44 strict movies, 5 marker-only and 57 unrecorded historical outputs. Decode
  success does not make the latter 62 strict. The report keeps release readiness false until the
  native archive, source/runtime and clean-install gates pass. New content-bundle construction
  and a fresh isolated runtime build from sealed source `c68d964...` are now authorized/in progress;
  no game launch or publication has occurred. Preserve their partial evidence and inspect actual
  live jobs before starting any duplicate work.
- Bounded still review now covers 44 strict clips: SqMuscle added three exact-PTS pairs and six
  jointly viewed sheets with 1,497 matching packet timestamps/durations. Reconstructed patterned
  texture and smoothed skin remain limitations; no full temporal or realism-recovery claim. See
  [SqMuscle review](notes/video_visual_delta_20261026_r5.md).
- Coordinator 13372/video 18468 continue Svincint with one GPU worker. Discover real PIDs before
  scheduling. No owned maintenance pause is held. The final 106 strict movies, media audits and
  all-file verified full-project backup still precede new offline PBR; runtime PBR is unchanged.

## Earlier checkpoint — 2026-10-26 16:28 MDT

- The corrected content cohort is now frozen externally at
  `G:\MC3_Backups\release-work\corrected-content-0.3.3-dev.20261026-r1\content-snapshot-r1`.
  Manifest `46505b35...` seals 7,188 playable files and 7,391 total inventory files including
  separate evidence. Actual copied evidence gives 44 strict original/model records, 5 marker-only
  and 57 unrecorded historical movies. The historical 39/67 plan and proof ZIP remain unchanged.
- The graceful stop followed authenticated SqMuscle publication. The snapshot completed 13:01 MDT.
  Retirement then failed because a C: to G: rename is not atomic across volumes; the exact owned
  request stayed preserved. Usage exhaustion delayed recovery. A PATH-selected Python 3.15 retry
  lacked Pillow and refused before retirement. Reviewed recovery used explicit Python 3.12,
  same-volume retirement and a separate verified G: evidence copy at 16:26 MDT. One coordinator
  **13372** and video child **18468** now process Svincint; discover actual live identities before
  future work. No human game/input or foreign stop was interrupted. See
  [snapshot and recovery](notes/content_snapshot_20261026.md).
- The read-only frozen validator passed 23 focused CPU tests independently and at root. Full
  inventory hashing, fresh integrity decoding of 7,081 PNGs, authenticated original/reviewed hash joins
  and all 106 movie decode/frame/timing checks are now running in the new external `validation-r1`
  directory. Its source must stay unchanged while that audit runs. Historical timing findings
  cannot become strict provenance or silent release approval. Native archive/clean first launch
  and exact runtime/source sealing still gate a new release; none was uploaded or published.
- San Diego remains installed as 7,079 original-pass images plus 2 reviewed corrections. Bounded
  movie still review covers 43, not the newly frozen 44th clip. Other-city native comparisons,
  NPC walking/footplants and road scrape evidence remain open. Final 106 strict outputs, final
  audits and a verified full-project backup still gate new offline PBR; runtime PBR is unchanged.

## Earlier checkpoint — 2026-10-26 11:40 MDT

- The original coordinator17636/video11568 pair remains healthy with no duplicate inference
  worker or game session. The frozen report now has43 strict completed movies, zero failures;
  SqMuscle is in progress. No new stop request or full-content copy was started.
- Six newly completed clips received actual CPU-only visual review:18 exact-time pairs and12
  sheets, zero sampling failures and6,808 matching packet timestamps/durations. Bounded coverage
  is now43 distinct strict movies. Fine reconstructed lettering/grille detail and smoothed
  faces/fabric remain limitations; this is not full temporal certification. See
  [six-movie review](notes/video_visual_delta_20261026_r4.md).
- The reviewed-texture proof archive is built and fully reread locally:110 required files plus
  embedded inventory,157,905,338 bytes, SHA `21105dea...`. It preserves exact required history and
  explicitly lists metadata-only private/historical observations. No actual saves/settings or
  token markers were included. Optional authenticated auxiliary-archive publisher support passed
  all21 root tests without network activity. See [proof tools](notes/release_proof_tools.md).
- Snapshot tooling passed all25 root Windows fixtures and a real read-only7,188-entry plan.
  An actual API test disproved nonblocking concurrent replacement on this host: held targets
  make os.replace return WinError5 despite delete sharing. Concurrent copying therefore refuses.
  Explicit quiescent mode requires an unchanged owned stop, exited real workers, and coordinator/
  install/video output locks. See [required snapshot window](notes/content_snapshot_window.md).
  Preserve the current movie; arrange a later graceful stop after authenticated publication,
  then copy/verify and restart one coordinator. No real snapshot, new release or upload occurred.
- San Diego canonical ledger/proof and independent old archives remain installed/preserved.
  Other-city native comparisons, NPC walking/footplant and road scrape evidence remain open.
  Runtime PBR is unchanged; full strict media/final audits and verified full backup still gate
  new offline PBR generation. The sealed old release plan remains immutable as progress advances.

## Earlier checkpoint — 2026-10-26 07:20 MDT

- The two native-reviewed San Diego panels are now installed in the canonical HD pack through
  the tested r2 transaction path. The full mixed audit passes **7,081 accounted outputs**:
  7,079 unchanged original-pass images plus two reviewed corrections, all final issue lists empty.
  Journal `9428aa3c...` is committed, ledger `f8ed40b4...` matches r2 exactly, and the active
  transaction registration is retired. Both old PNG archives are independent verified copies.
  See [installation and restart evidence](notes/reviewed_s_install_20261026.md).
- Sqtuner published all996 frames with strict original/model/output provenance before the owned
  maintenance stop. All39 published strict movies are retained, zero failures. The next temporary
  Sqperfom movie had reached24 frames and restarts; do not call this a zero-work-loss stop.
- Root retired only its exact unchanged maintenance request after commit/full audit, then started
  one hidden current-source coordinator **17636** with video child **11568**. The new worker contains
  tested identical-original reuse (`fb400b34...`) and Sqperfom is advancing. No actual reused output
  is claimed yet. The coordinator's own fresh mixed-pack audit passes and preserves the new PNGs.
  The former9824/18460 pair exited normally; old private launch helpers pin those PIDs and need
  fresh reviewed contexts before any future QA. No game or physical input was stopped.
- Independent install review `ad5a7ab2...` confirms actual PNG/archive/ledger bytes, retired markers,
 31 pinned native run references, unchanged settings and all10 private save files. Frozen r1 and
  accepted native proof stay preserved. Other-city native comparisons, full-body NPC walking/
  footplants and all-car road contact/sparks remain open.
- Canonical r9 and older public release assets are unchanged. A future release must freeze and
  authenticate the new corrected content explicitly. Complete the remaining strict movies and
  final audits, then verify the full project backup before new offline PBR. Runtime PBR is unchanged.
- Fixed an actual content-packaging bug: recursive MP4 collection selected retained/in-flight
  `.part.mp4` files. The builder now selects exactly the106 original-source-relative names and
  rejects missing/ambiguous/unsafe/linked selections. Root and agent passed9 focused fixture
  groups plus existing portable synthetic tests; real read-only selection was106 of108, with
  both partials preserved. See [release selection and next snapshot](notes/release_movie_selection.md).
  A corrected-content development release can honestly freeze39 new/67 historical playable
  movies now; it needs a new manifest, exact source proof, separately authenticated reviewed
  texture proof graph and clean-install validation. No new release or large archive was created.

## Earlier checkpoint — 2026-10-26 06:20 MDT

- Public source includes `5b5e2d7`, the independently CPU-tested opt-in startup gate. Isolated r11
  EXE `73279609...` completed three HD-enabled starts and normal exits. Canonical/public r9 stays
  unchanged. See [native photo/handoff evidence](notes/native_photo_r11_20261026.md).
- All owned QA games are closed. Coordinator9824 and the same video child18460/encoder retained
  `story/sqtuner.bik` at24/996 across the actual pause, then resumed and advanced after the final
  normal close. Strict coverage is38/106, zero failures. No user stop marker was cleared or duplicate
  inference worker started. A usage interruption left the candidate idle/queue paused for several
  hours; this delay is recorded, not presented as continuous processing.
- The first four actual BMPs caught only the outgoing menu and are explicitly rejected as a photo
  baseline. Fresh BASE/candidate sequences at later ordinals now contain actual visible San Diego
  face/hands in1920×1080 lossless backbuffers. Both source variants are pinned at gated release,
  before pack initialization. Root and independent review accept the two S panels as a local
  face/hands/join improvement; residual wipe phases differ, so no whole-frame equality claim is
  valid. Accepted proof SHA `4c44b4dc...`; fresh r2 proposal is
  `.local/reviewed-san-diego-proposal-20261026-r2` (plan `fe63c428...`). Its full base audit still
  passes7,081/7,081. Actual installation correctly remains blocked by live media/coordinator
  workers; do not bypass this check. Preserve frozen r1 and the current in-flight movie. Prefer
  a safe published-movie boundary for a needed paired restart/apply and activating tested
  identical-original reuse. Canonical textures and reviewed ledger remain unchanged.
- Both private HDD6/HDD7 save sets and settings match their initial receipts. The regular desktop
  shortcut was found pointing at old private QA r7, backed up exactly, restored to canonical r9,
  and stayed unchanged during all new QA launches. Old private saves and assets remain preserved.
- Full-body NPC gait/footplants, all-car road clearance/sparks, other-city portrait native review,
  strict movie completion/final audits and the verified post-media backup remain open. No new
  offline PBR was generated and runtime PBR is unchanged.

## Earlier checkpoint — 2026-10-25 23:04 MDT

- Source changes are public through `db7df67`: `9fd0900` adds exact-original movie reuse, `b2ec102` adds
  authored vehicle-bound audits/extreme fixtures, and `db7df67` fixes capture-session collisions and adds
  opt-in same-present photo evidence. Public v0.3.2 assets/tag remain unchanged. Canonical r9 EXE remains
  SHA-256 `7bb7fcd1928156db88e33c78cd5a799ed9b04d4c9c19c9a9bbd6b00e2aa9d1ad`.
- Coordinator **9824** and video child **18460** continue normally. Strict coverage is **33/106**, zero
  failures, **85,804 frames / 73 movies** remaining; `reward/rgroupb.bik` is in progress. No game session,
  new QA stop marker, actual canonical texture adoption or additional GPU worker was started. Preserve
  current in-flight work and user stop requests. Do not pause this healthy queue solely to await QA capacity.
- The live worker loaded the earlier `4693056a...` script. The current source adds byte-identical reuse;
  editing it does not retrofit that child. A later safe paired restart activates the new path. All existing
  picture recipes remain unchanged; no canonical alias was applied. Two original tutorial pairs have exact
  full-Bink/header equality and can avoid 3,089 repeated inference frames. The independent copy requires
  exact original/model/encoder/timing provenance, complete decode/PTS validation and destination anchors.
  Root passed all **51 video tests**; independent reuse review passed all 13 new fixtures. See
  `notes/identical_movie_reuse.md`. Current worker source SHA-256 is
  `fb400b342157a512b679e1fc81485f54558d9112adb6d43663d111b18d19e965`.
- The bounded original-resource audit covers 282 main/_g/_o variants, 94 families and 188 tunes. It found
  no demonstrated ride implementation defect. Authored F355 .08 m and SLR .09 m clearance skins already pass
  through unchanged; .05 m is the usual/default skin, not universal. Focused real-solver/AirRide/floor tests
  now include mixed stock steps 6–8 and actual maximum radius 1.312333345 m. Root independently passed both
  CTests and all 7 parser tests. Road contact/sparks/all-car native appearance remain unverified. See
  `notes/vehicle_ride_bounds_audit.md`; production physics was unchanged.
- Eight additional strict movies received actual CPU-only visual review: 24 matched-time pairs and 16 sheets,
  zero sampling failures. This extends the historical 25 sampled clips to 33 distinct reviewed movies;
  neither set certifies all frames or temporal stability. Sharper outlines coexist with synthetic tiny
  grille/badge/sign details and smoothed grain. See `notes/video_visual_delta_20261025.md`. No media recipe or
  output was changed by review. Recent uninterrupted eight-frame batches take about 3.2 seconds/frame;
  the older ~64 GPU-hour estimate is stale and remaining work is still multiple days.
- Found and fixed a real diagnostic bug: same-second F3 requests previously overwrote/mixed evidence.
  New session directories/files are exclusive and record frame/present/wall/monotonic identifiers.
  New opt-in photo capture caches original content identities, filters optimized shader texture access,
  and saves the actual current composed backbuffer after DrawFrame, without Shrink. Four attempted frames
  at most, 64 MiB each; disabled by default. Map failure is checked and readback restoration is covered by
  CPU fixtures. These are diagnostic changes, not a new demonstrated NPC animation fix.
- Root passed the 5 focused diagnostic/vehicle CTests; all 3 new diagnostic tests also pass independent builds.
  Native isolated candidate **r10** linked successfully at
  `.local/native-photo-candidate-20261025-r10/mc3.exe`, 23,643,648 bytes, SHA-256
  `b08ddda04f871b726cf1060bbd8f344404c92ac6923ee7e9991b81da6fb571af`.
  Its build manifest proves canonical outputs/build graph unchanged. Additional source/header receipt
  `root-proof.json` has SHA-256 `d637a1f52c5771c104e377a42d5dd486478015608f868ba4b5098af04bfe13e9`.
  This is a build-only candidate: fresh isolated QA assets/save/settings must be staged before launch.
- No r10 native session or GPU readback validation occurred. Keep r9 installed/public until required native
  checks pass. After a safely scheduled media handoff and sufficient QA capacity, stage a private r10 instance
  and collect fresh BASE/candidate S sequences using `XBE_CAPTURE_TEXTURE_HASHES=4D145117D9DD0F63,A21582D8BE650E3F`,
  `XBE_CAPTURE_TEXTURE_FRAMES=1,4,8,16`, and a nonempty unique `XBE_CAPTURE_DIR`. Inspect actual images and
  comparable wipe phases; shader access alone does not prove visible contribution. Preserve the old raw-JPEG
  baseline. Source/CPU tests cannot replace that native check. Details: `notes/loading_photo_capture_plan.md`.
- Native UI has no owner. Private r7 S images remain staged candidates and its other 7,073 hardlinks must
  stay untouched. Canonical textures/ledger remain unadopted. A new accepted r2 proposal under current helper
  pins is still required. Full-body NPC walking/footplants remain open; source review found no basis for a
  timer rewrite. `notes/npc_capture_evidence_gap.md` identifies full uploaded VB/index/CB and post-VS gaps.
- Keep completing strict media, then final audits and a verified full-project backup before offline PBR.
  Runtime PBR is unchanged. No new release or full-backup gate was issued during this diagnostic/source pass.

## Earlier checkpoint — 2026-10-25 17:49 MDT

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
