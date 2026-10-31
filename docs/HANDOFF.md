# MC3 port — handoff (read this first)

Last updated 2026-10-23 by the Codex continuation. Read [AUTONOMOUS_RUN.md](AUTONOMOUS_RUN.md) for the latest active work,
then section 0 and docs/REANALYSIS.md. This is the state of the work, how
to continue it, and everything learned, written so another agent can pick up without the conversation.
`PORT_DOCUMENTATION.md` (repo root) has the long-form history; this file is the current picture. Agent research
notes land in `docs/notes/*.md` (see "Work in flight").

---

## 0. Current continuation (supersedes the older work-in-flight descriptions below)

**2026-10-31 late (road markings and overlays through PBR; what stays legacy):**
- **Program.** The world's blend-layer pixel shader (combiner count 0x11103, rgb_in C8C40000/C9C40000/1DCDCC3D,
  no final combiner) computes r0 = t0*v0, r1 = t1*v0, `r0.rgb = r1.rgb*r1.a + r0.rgb*(1 - r1.a)`. It draws road
  markings, cracks and stains over asphalt and walls. `pbr::BlendLayerProgram` (pixel key) and
  `scene::BlendLayerShader` (raw D3DPIXELSHADERDEF, `BakedProof::blend_layer_kept`) match it word for word.
- **Shader.** `options[3] = 2`: the stage-1 texture times the vertex colour, laid over the encoded surface by
  `t1.a*v0.a`; output alpha `t0.a*v0.a`.
- **Texture transform.** The overlay's coordinates usually go through `D3DTS_TEXTURE1` with `D3DTTFF_COUNT2`. The
  fixed mesh packs `mul((u,v,1,0), T)` into `uv1` like `FixedVertexHlsl`, and the matrix is part of the mesh cache key.
- **Result.** Tokyo: about 800 materials through the path, up to 786 draws a frame, 60 fps (PBR 6–9 ms).
- **Kept legacy on purpose:**
  - Additive glows and decals (other blends, depth write off): a material model adds nothing to light halos.
  - The environment-bump program 0x11101 (glass and water reflections).
  - Vertex-program draws: mostly skinned "rider" characters, plus a few animated facades. Their geometry exists only
    after a GPU vertex program, which the CPU-built mesh path cannot reproduce.

**2026-10-31 night (facade night windows through PBR):**
- **The cascade.** Captured from the facades: s0 `tmp = tex0*diffuse`, `tmp.a = tex0.a`; s1 `cur = tex1*TFACTOR` on
  texcoord set 1; s2 MODULATEALPHA_ADDCOLOR `tmp.rgb + cur.rgb*tmp.a`, alpha `tmp.a`. `pbr::WindowLayerSurface` (pixel
  key) and `scene::WindowLayerCombiner` (raw state, with `BakedProof::window_layer_kept`) match exactly that.
- **Vertex.** The PBR vertex is 56 bytes. `uv1` holds the layer's coordinates, read by `FixedProof::layer_uv`; `Gate`
  rechecks stage 1's index on refresh.
- **Binding and shader.** `GeometryConstants::options[3] = 1` marks a layer. `device::Input::game_layer` binds the
  stage-1 texture as t4, and `emission_color` = TFACTOR. The baked shader adds
  `saturate(tex1*TFACTOR)*albedo.a` after encoding and before fog. Alpha stays `albedo.a` (no diffuse alpha packed).
- **Descriptor tables.** One per (albedo, layer) pair (`MaterialEntry::game_tables`). Facades sharing a base texture
  alternate window textures, and one cached pair had exhausted the append-only heap ("budget exceeded").
- **Result.** Title backdrop: 333 materials, 471 draws a frame through the path, 60 fps. Windows look as before.

**2026-10-31 evening (cutouts through PBR):**
- **What draws now.** Trees, fences, railings and sign edges draw through the dedicated path. These are draws with a
  GREATER/GEQUAL alpha test, optionally blended SRC_ALPHA/INV_SRC_ALPHA, plus ONE/ZERO blending (opaque).
- **Shared check.** `pipeline::KeepsLegacyBlend` is used by `DescribePipeline` and the device's `OriginalStateValid`:
  the PSO keeps the draw's blend state. `device::Input::draw_alpha_test` lets the draw's own test set coverage.
- **Clip.** The cutoff is `(ref ± 0.5)/255`, so `clip(a - cutoff)` equals the legacy `round(a*255) > ref`/`>= ref`.
  The baked shader clips `albedo.a * vertex alpha`, the alpha the original tests.
- **Baked scene.** `BakedProof::coverage_kept` (with `combiner_alpha_packed`) admits blending and those tests. Lit
  draws with coverage stay legacy, since they write texture alpha.
- **Result.** Per-material outcomes in a run went from 371 to 543 drawn. Tokyo and San Diego at 60 fps, 2.8 ms.
- **Still legacy.** 211 two-texture combiners (s0 tex*diffuse → TEMP, s1 tex1*TFACTOR, s2 MODULATEALPHA_ADDCOLOR:
  facades with lit night windows), 50 vertex programs (cars, peds), 38 other blends (additive glows), 29 with depth
  write off.

**2026-10-31 later still (road and wall detail):** the city pack now ships the generated normal maps (roads and ground
512, ORM 256; the rest at their own size up to 128; about 1.35 GB decoded, inside the 1.5 GB budget) instead of one
flat normal. The baked branch of `pbr_pipeline_shader.h` shades them as relief against the surface's own normal and
adds a roughness-scaled GGX gloss of the baked light. Native San Diego: 211 draws through the path, 3.2 ms, 60 fps;
pavers and asphalt show relief and sheen. Pack digest `2104f550…` installed.

**2026-10-31 later (songs skipping on hard turns: the arrow keys):**
- **Cause.** The keyboard fallback maps the arrows to the D-pad, and while driving D-pad left/right changes the song.
  Each arrow-key turn started a new music stream; W/A/D steering never did. Reproduced in the San Diego cruise: a new
  `text: "music_..."` key appeared on the first right-arrow turn.
- **Fix.** `templates/game/src/game/arrow_driving.cpp` (`GameInfo::pad`): while the player drives, the held arrows
  are steer, gas and brake, and their D-pad bits are cleared. A controller's own D-pad is untouched.
- **Driving signal.** The human car controller's per-frame update renews a 150 ms lease. That update is vtable
  `0x47684C` method 8, `0x2C8BC0`; methods 2, 7 and 14 behave the same. Probed at 60 calls/s while in control,
  standing still included, and 0 under the pause menu. Frame time, the time scale (`0x4B2D6C`) and the player car
  pointer all stay live while paused, so none of them work. `0x2C9260` (the replay pad read) is not called in a cruise.
- **Verified.** Eight arrow-key hard turns, no song change. The pause menu navigates with the arrows. The profile and
  main menus too.
- **Load stalls.** `hooks.cpp` logs `timing: a frame took N ms` past 100 ms. In 70 s of hard turns: no stall, and
  music gaps of 20–25 ms. One 330–350 ms stall right after a cruise loads gives an audible gap; a separate issue.
- **Side effect to tell the user about.** A menu script early this session pressed Enter on "Create Profile". The
  save now has a **Player 2 (0% – San Diego)** profile. Player 1 is untouched. Not deleted: that is the user's call.

**2026-10-31 (upstream SDK merged: 36 commits of TGP482/Joshhhuaaa through "add nightfire"):**
- The recompiled code now builds into `mc3.dll` beside `mc3.exe` (delay loaded). `add_xbe_image` takes the exe name as
  a sixth argument, `game.toml` has `library = "mc3.dll"`, and the `mc3` target builds both. The exe's post-build
  touches the DLL: `runtime/app/game_code.cpp` rebuilds a DLL older than the exe at startup, which takes minutes.
- Depth is reversed in a D32 float buffer (`g_reversed_depth`). The PBR path takes the legacy draw's
  `FillDepthConstants` zparams in `GeometryConstants::depth` (now 320 bytes) and applies the same ReversedClip.
  Its PSO already copies the legacy depth format and comparison.
- Upstream's per-draw bind cache (`binds_`) is reset after every dedicated PBR draw, and the signed-blend second
  pipeline updates it. The constant re-upload for screen effects keeps `last_dc_`/`binds_.dc` in step.
- Kept our `dsound.cpp` mixer: upstream's rewrite (DSP image, 3D ear delay, bins) would have dropped the song-overlap
  fix, SetEG/SetLFO envelopes and Set3DVoiceData. `dsp_image.cpp` compiles but is unused; porting it is a follow-up.
- Kept our vertex program slots (WriteSlots/SelectProgram/SyncProgram: the vinyl fix) over upstream's StoreProgram.
  We took upstream's push walk split (ApplyImmediate) and `SetFrameBuffers`/Reset.
- Upstream's texture dumper/replacer (`textures\replacement\`) runs beside our texture pack and does nothing unless that
  folder has images. Car paint now anchors on the sblend line, since `alphatest()` is emitted only when alpha
  testing is enabled.
- A stream refilled after starving logs `dsound: stream X (N ch) silent M ms until its refill`: songs reported to skip
  on hard turns.
- Verified: 58/58 CTest checks, 8 tools/pbr tests, the menu world at 60 fps with 425 PBR draws a frame.

**2026-10-30 final (all four cities converted, built in):** maps generated for every city texture (3,259 of
3,260; the one left is an 8x8 "skip"); `pbr-cities` holds 3,259 materials, loads in ~10 s and is installed in
settings.toml (parallax 0.04). Native 20-s drives, PBR on, no other load: San Diego 60 fps (211 draws through the path,
3.3 ms), Atlanta 60 fps (464 draws, 7.6 ms), Tokyo 60 fps (346 draws, 6.5 ms). Detroit's textures are in the pack
but the city is locked on this save. Player guide has a PBR section and troubleshooting rows; AUTONOMOUS_RUN,
pbr_system and pbr_reachability notes updated. 58/58 CTest checks and 8 tools/pbr unittests pass.

**2026-10-30 later (whole-city PBR pack, no captures):** a material can now name its texture alone
(`vertex_shader = "any"`, `pixel_shader = "any"`, `uv_set = "any"`: `pbr::kAnyShader`/`kAnyUv`; every draw-state gate
still runs, and the mesh uses the draw's own coordinate index) and take `albedo = "game"`: the device binds the draw's
own accepted colour texture as t0 (a per-material SRV table appended whenever that resource changes; the resource is
retained per frame), `GeometryConstants::options[2] = 1` decodes its gamma after sampling (the legacy filter order),
and no albedo copy or alpha proof is needed (coverage is the original's at every mip). Lookups are indexed by texture
(`Library::by_texture`, `pbr::ForTexture` falls back to a scan for hand-built libraries; device material cache
indexed too). Limits: loader 4096 materials; device 4096 materials, 40960 SRVs. `tools/pbr/test_pack.py --cities
[--install]` writes `pbr-cities`: every Detroit/San Diego/Atlanta/Tokyo texture in source_index.json with generated
maps (ORM 256, height 128, shared flat normal). Generation of the 1568 city textures that lacked maps runs from
`tools/pbr/sets/cities_missing.json` (two shards; four ran out of RAM). Tokyo with 1821 materials: 262 draws through
the path at ~60 fps; the rest are blended, alpha-tested, programmable or multi-texture. Detroit stays locked
("roadtrip" did not unlock it in Arcade); its textures are in the pack regardless. Restart Replay replays identically.
Startup: never press Enter blindly; the save prompt can start "create new save" (it happened once, backed out).

**2026-10-30 (PBR finished for baked world draws; replays exact on screen):** real pack `pbr-test-real` (817
materials from 27 captures; albedo = the accepted colour, normal/ORM/height from textures/pbr generated maps resized
to 256, flat maps for 287 textures without them) is installed with parallax 0.04; 415 Tokyo materials draw at
59-60 fps (path ~6-7 ms for 350-390 draws). Changes: budgets raised (loader 1024 materials / 1.5 GB decoded; device
1.5 GB resident of a 2 GB hard cap, 1024 materials, 8192 SRVs); `ValidateMesh` memoizes successes by content hash; a
mesh needs a triangle with area (collapsed UVs only mean one texel); a material without a height map draws with no
height effect, and displacement falls back to parallax. Still legacy (correctly): blended draws, GREATER-tested
cutouts, a texture*TFACTOR-only combiner. Replays: the player car transform is recorded per frame and restored on
playback (notes/replays.md); a 1600-frame replay played to the end without a drift flag. draw_evidence_test now
clears its synthetic output folder (fixed stamps exhausted the 64 unique names after many runs).

**2026-10-29 late night (replays, music check, PBR in four cities, perf):** replays ended at the first position check
even with recorded steps; a per-frame trace shows timing-dependent float drift (see notes/replays.md), so playback now
plays on past a failed check: a 1244-frame Tokyo replay played to the end. No song overlap or refill-while-stopping
lines across menu -> cruise -> replay (new `dsound:` overlap/refill log lines watch for it). PBR: 27 captures (title,
San Diego, Atlanta, Tokyo; Detroit locked) give 817 candidate assignments, 410 fit the loader budget; Tokyo shows
them widely. Perf: the path cost 16-19 ms/frame for ~270 draws (Tokyo at 33-43 fps). Now ~5 ms/270 draws, 60 fps:
`RecordDraw` takes the caller's plan (no second PlanDraw), `KeyEqual` compares the assignment before the digest, and
`TryPbrDraw` caches built meshes by a hash of every input (`fixed_mesh::Refresh` redoes the state gate, constants and
projection checks). `perf: pbr per frame ...` is logged every 10 s while it draws; the remaining cost is mostly the
device's own `ValidateMesh` in PlanDraw. settings.toml points at the 410-material `pbr-test-marked` (magenta).

**2026-10-29 night (songs overlapping, PBR in cruise):** songs overlapped after the 10-28 skip fix: a stream
XACT is stopping (FlushEx with an envelope release) that starved during a load kept the starved-grace PLAYING, so
the stopped song stayed alive, got refilled and played over the next one. `StreamState::stopping` drops the grace,
and `Render` deactivates a stopping stream whose packets run out (tests: `stream_status`). Not yet heard in game.
PBR in Arcade cruise: San Diego and Atlanta captures (Detroit is locked on this save) plus the title give 592
candidate assignments; `tools/pbr/test_pack.py` now merges captures, uses shared 4x4 flat normal/ORM maps and stops
at the loader's 512 MB decode budget (373 of 592 fit). Varying-alpha textures are accepted at runtime where the
written alpha is provably zero (TFACTOR alpha 0, no blend, ALWAYS test).

**2026-10-29 evening (PBR test pack):** captures now record per-draw shader keys and per-stage texture hashes;
`tools/pbr/test_pack.py --latest --install` builds and approves a test pack from the newest capture (see the
reachability note, "Test pack"). **First native result:** 72 of 73 title-screen world materials draw through the
dedicated path (69 before frame-less triangles were allowed) (magenta with `pbr-test-marked`; `pbr-test-neutral` matches PBR off within ~2/255 mean), after
exact fixes for the world's TFACTOR stage, ended cascades, ALWAYS alpha test and packed output alpha. settings.toml
currently points at `pbr-test-marked` (height mode none); `settings.toml.bak` holds the previous file.

**2026-10-29 afternoon (PBR face normals, baked mode, fog):** steps 2–4 of the reachability note landed
(uncommitted) and complete Codex's half-finished baked vertex-diffuse mode, which had left the tree unbuildable.
Normal-less world streams shade with the triangle's own plane; LIGHTING-off draws use the vertex diffuse as the
light (decoded like the albedo, so neutral maps reproduce the original); fog matches the legacy fixed path. Vertex
is now 48 bytes, SceneConstants 704. 8,824 captured draws now pass the capture-visible gates. 58/58 CTest checks
pass and mc3.exe is rebuilt, but nothing has been drawn through the dedicated path on a GPU yet.

**2026-10-29 morning (PBR reachability):** the working-tree dedicated PBR path builds into mc3.exe and all 58
CTest checks pass, but a census of 40 captures (71,920 draws) finds **no draw that can reach it**: world draws are
strips/fans, carry no vertex normal and are lit by baked vertex colour with lighting off. See
[notes/pbr_reachability_20261029.md](notes/pbr_reachability_20261029.md) and `tools/pbr/draw_census.py` for the
numbers and the ordered list of what the path needs next (strip expansion, geometric normals, baked-irradiance
scene mode, fog).

**2026-10-28 late (camber bend / songs skipping):** user reported camber "bends the wheel instead of tilting it" and
songs skipping on their own.
- Camber: F7170 scales the wheel matrix rows (axle ×2·width, diameter rows ×D) at 0xF7405, *then* rotates the axle and
  vertical rows by the camber angle with 0x869D0 (0xF7714 / opposite side 0xF7D39). Mixing rows of unequal length is a
  shear; invisible at stock −2.5°, a visible bend at large PC camber. New `templates/game/src/game/wheel_camber.{h,cpp}`
  overrides 0x869D0 only for return addresses 0xF7719/0xF7D3E: rotate the unit axes, restore each row's length.
  `wheel_width_pivot` now runs the real F7170 at −30°/+20° and checks orthogonal axes, 2·width / D lengths and the
  tilt angle, and that the original sheared.
- Songs: XACT's IsPlaying (0x40F198) is only `GetStatus & PLAYING`, and its voice update (0x40D871) ends the wave once
  no read is pending and the stream isn't PLAYING. The real XDK GetStatus (0x3C4FA9) drops PLAYING when starved, which
  the console never is, but the port is whenever the title thread stalls past the queued audio (mc3-7.log: songs
  changing every ~5 s after a race load). Real ends always get Discontinuity (0x40F7DD). `runtime/audio/stream_status.h`
  keeps a starved stream without one PLAYING for up to 10 s; dsound.cpp tracks `starved_at` and LOGRs the first 16
  starvations ("ran out of packets mid-wave"). Test: `stream_status`. Neither fix is verified in game yet.

**2026-10-28 evening (neon / vinyl / per-car):** see [notes/neon_vinyl_20261028.md](notes/neon_vinyl_20261028.md).
Neon brightness now keeps hue (shaders.cpp `sblend.w` caps colour gain at the brightest channel); P8 textures are
never sampled as a stale render target (`CanBeTargetFormat`, texture.h). Built into the canonical
`templates/game/out/native/mc3.exe`; the previous `d3879439...` EXE is backed up in
`.local/exe-backup-20261028-neon/`. Neither fix is verified in game. Per-car tuning: code reviewed, no defect
found; needs the user's concrete symptom.

**2026-10-23 night:** the frozen public playable content release is
[v0.3.0-dev.20261023](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.0-dev.20261023).
The garage-to-street ownership repair passed native Golf R32 checks and 19 CTest checks; ride-only F1 changes
refresh immediately and retain independent front/rear values after leaving the garage. See
[live_vehicle_tuning.md](notes/live_vehicle_tuning.md). The full source/game checkpoint is also published in the
private backup repository; this does not replace the later post-media/pre-PBR backup gate.

The installed hotfix EXE is `b3b6bd221d22055860de276447911be25fcda81f3ef60039102145f3bd0a14b2`,
native source `3a0ed6832c9a17938cbb3551c606e309334b0a4f`. It passes all **21 CTest checks**. Native '57 Bel Air
QA confirmed profile code 33 on both axles through garage → street → garage and four -0.361 m cached offsets.
The private floor correction fired and preserved the original floor plus 5 cm skin. Two HD-enabled startups
reached rendered menus/driving without delayed-fence logs. See [native proof](notes/native_hotfix_20261023.md).
All-car/camber ground clearance, real sparks, same-model AI isolation and a fresh-process profile reload remain
open. The shader/texture capture and save copies are private local evidence, not release assets.

The fresh original-derived UltraSharp pass is **complete: 7,081/7,081**, with the named-pass SHA-256,
dimensions and PNG integrity audit passing and no missing/corrupt/orphan replacements. Report:
`.local/texture-pass-20261023-final-audit.json`. All 5,603 vinyl masks remain excluded. The x4plus movie pass now
uses explicit exact encoder/MP4 clocks and complete source-grid checks; **18 video + 21 coordinator tests pass**.
The coordinator resumed at 23:03 MDT, waited for the owned QA game, and started one movie worker at 23:10.
The removed shared stop marker was root-owned. Preserve any new stop request and never start a duplicate worker.
Final media audits, a verified full post-media backup, then the offline starter conversion remain outstanding;
runtime PBR is unchanged.

A follow-up asset audit found that the new saved tire-profile codes also index the original eight-entry mesh
resource table. The corrected table lifetime/fallback passes actual generated-reader tests and the bounded Bel Air
native check above. Numeric diameter/save-code coverage alone did not cover these mesh readers. The
[ground-contact audit](notes/ground_contact_audit.md) also identifies an original lowering-induced chassis-floor
retraction; the guarded private-player correction and original-writer fixtures are now built and tested. The
read-only [mesh audit](notes/tire_mesh_support.md) adds 28 passing tests and shows why width/camber need actual
vertex support for a broader road-contact guarantee. The public
[v0.3.1 runtime hotfix](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.1-dev.20261023)
uses unchanged v0.3.0 content; remote metadata and an unauthenticated ZIP download passed. Existing release
assets were preserved. See [publication proof](notes/public_release.md).

The paragraphs below retain the earlier continuation history; their build/media counts are historical.

**2026-10-22 continuation:** the user now explicitly authorizes unattended game control, driving/menu checks,
parallel agents and downloads. This supersedes the older "don't launch/drive" preference recorded in section 1.
Source has been pushed to the now-public repository `christiand0797/MidnightClub3-Port` (`github` remote); `origin`
still points at the upstream framework. The local checkout was shallow; fetching upstream history fixed its initial
empty-repository push failure. Full-project saves/configuration backups use the private companion
`christiand0797/MidnightClub3-Port-Backups`. The latest user request also includes supplied game data in the source
repository and complete playable public releases; packaging and verification are in progress.

Current native build and all **13 C++ checks pass**. Tire widths now have **38 selections, 105–755 mm**, preserving
all prior indices. Direct-to-garage preview cars bypass mcPlayerFactory; explicit selected-preview ownership now lets
them receive player-only suspension travel changes. Live maximum-low ride-height validation is ongoing. Actual
generated wheel-solver regression confirms the widened travel avoids the original clamp for contact requests.

All **7,081 eligible active texture replacements** passed the CPU audit; **5,603 categorical vinyl masks stay
excluded**. A fresh named UltraSharp pass is prepared. Detailed x4plus movies remain in progress (48/106 at initial
audit, about 48 GPU-hours remaining); no duplicate GPU job was started. The durable coordinator will finish both
passes, verify a complete backup, and only then allow new offline PBR generation. **No new PBR work is authorized
before that gate succeeds.** Existing PBR experiments and plan below are inherited, not newly enabled rendering.

Historical findings follow; prefer the active ledger and latest subsystem notes when these older counts conflict.

The requested reanalysis found a source/build integration break left by partial settings cleanup. It is repaired.
Detailed scope, validation and remaining limitations are in [REANALYSIS.md](REANALYSIS.md); subsystem evidence is in
[notes/runtime_audit.md](notes/runtime_audit.md), [notes/settings_tuning_audit.md](notes/settings_tuning_audit.md),
[notes/media_audit.md](notes/media_audit.md) and [notes/audio_envelope_lfo.md](notes/audio_envelope_lfo.md).

- Current native output remains `templates/game/out/native/mc3.exe`. Use root `Launch-MC3.ps1` or `.bat`.
  `LaunchGame.ps1` now forwards to the same launcher for the existing desktop shortcut. Root `mc3.exe` is stale.
- Removed the unused MC3Extension and disabled `graphics_page.cpp` prototype. The default shader hooks already
  declare `float4 ext`; preserve that exact matching C++/HLSL layout. Native graphics/menu research remains below.
- Fixed WORLD0..WORLD3 transfer to the threaded renderer. Pedestrian/vehicle animation still needs a user check.
- Fixed live 60 -> 30 fps switching by restoring the original requested step/substeps; preserve the caller's cdecl
  rate argument. A regression executes the actual hooks with deterministic guest clock entry points.
- Tuning visitors only register fields: values become tracked after parser `sub_0032F490` returns, not after
  registration. Guard committed pages and finite values; remove unused pending fields in parser destructor
  `sub_0032F840`. Actual-hook regression covers parser matching, reloading and stale/unavailable addresses.
- HD texture selection now invalidates cached pixels when toggled. HD movies support enabling a clip first opened
  with HD off and disabling/re-enabling during playback. Added an actual Media Foundation decoder component test.
- Removed inert LOD control/storage and stale unsupported enhancement settings from the source template. Added the
  real HD movie checkbox. FOV/LOD/native garage sliders remain future work, not functioning options.
- CMake code generation now depends on the image config and XDK configs rather than unrelated host settings.
- Existing supported texture batch finished: 4,774 valid replacements, 16 flat and 2 tiny intended skips. This is
  not every texture format. The inherited movie job continues; do not start duplicate jobs.
- Final integration: native AMD64 build passed; **8 C++ checks + 8 disc checks passed**. WIC ownership and actual Media Foundation decode/loop/toggle/data-reuse tests are included. Decoded pack images release after upload; decoder lifetime/error/row checks are hardened. See REANALYSIS for the executable hash and logs.
- No game was launched/driven in this continuation. No ISO or extracted game assets were rewritten. No commit made.

### 0.1 Work in flight (06:30, 2026-10-21, Claude lead session) — check before editing these files

Seven background agents, each owning only the files listed, none builds (the lead builds and integrates):

| Topic | Owns | Notes |
|---|---|---|
| DirectSound EG + LFO implementation | runtime/xdk/dsound.cpp, runtime/audio/* | docs/notes/audio_envelope_lfo.md |
| game.toml `= 0` symbol verification | templates/game/config/game.toml | docs/notes/symbol_audit.md |
| P8 / linear / .tex / .xbpf texture extraction | tools/texture_pack/*, textures/source | docs/notes/texture_extraction.md |
| Vertex shader slots + pretransformed IM vertices | runtime/xdk/d3d8.cpp (+ additive state.h) | docs/notes/d3d8_shader_slots.md |
| LOD / FOV reverse engineering | templates/game/src/game/lod.cpp, fov.cpp | docs/notes/lod_fov.md |
| Garage ride height menu, player-only tuning, rim/tire ranges | templates/game/src/game/tuning.cpp, garage.cpp | docs/notes/garage_tuning.md |
| Minimap black square + off-screen racer popup | runtime/gpu/d3d12.cpp, shaders.cpp/.h | docs/notes/hud_minimap_popup.md |

Verified this morning: HD cutscenes play in game from video_hd/*.mp4 (intro checked at 1920x1440, frames advance in
step with Bink; decoder serial ~= frame-2). Movie job: 102/106 done at 06:30, rerun tools/video/upscale_movies.py to
finish if interrupted. Build passes (06:25).

### 0.2 Landed since 0.1 (11:40, 2026-10-21)

- **Vertex shader slots + pretransformed IM vertices** (agent, runtime/xdk/d3d8.cpp; notes docs/notes/d3d8_shader_slots.md):
  136-slot program memory, LoadVertexShader / LoadVertexShaderProgram (placeholder symbol FFFFFE05, not in the image) /
  SelectVertexShader, SetVertexShader loads slot 0, inline program methods 0x0B00/0x1E9C/0x1EA0 and constants via
  `ApplyTransformMethod()` from both Walk() and EndPush, constant setters flush pushed commands first (ordering),
  XYZRHW immediate-mode vertices drawn through the screen-space path. Built; title renders; MC3 has not been seen
  calling the slot functions (grep log for "vertex program from slot", "selected at slot").
- **game.toml `= 0` audit** (agent; docs/notes/symbol_audit.md): all 55 genuinely absent (proved via the XBE .XTLID
  export table for 48, code evidence for the rest). Nothing to change.
- **Intermittent startup crash fixed** (lead, runtime/kernel/threads.cpp): XAPI sub_0001354A (called at boot from
  sub_001B15D0, sets the process quantum) writes [[fs:0x28]+0x44]+0x14 = KTHREAD.ApcState.Process->quantum. The
  runtime never set KTHREAD+0x44 and did not clear the KPCR/ETHREAD blocks, so leftover bytes made it write through
  a wild pointer now and then (dump CrashDumps/mc3.2026.10.21_06.44.36.dmp). Every thread now points at one zeroed
  KPROCESS (`Process()`), ApcListHeads are empty lists, and the blocks are zeroed first.
- **All 106 HD cutscenes finished** (video_hd/). Verified in game: intro plays in HD (during movies the title presents
  at 30 fps, as on the console).
- **DirectSound envelopes + LFO** (agent; runtime/audio/modulation.h new, runtime/xdk/dsound.cpp; notes
  docs/notes/audio_envelope_lfo.md "Implementation"): EG (delay/attack/hold/decay/sustain/release, xemu curves; EG0
  pitch/filter, EG1 amplitude), triangle LFOs, stepped every 32 samples; StopEx/FlushEx with release flags fade out.
  Built, title runs. Unverified by ear; first 16 SetEG / 8 SetLFO calls are logged ("dsound: ... envelope mode") in
  races. Guesses: LFO rate/delay units, depth scales. Filter cutoff still has no effect (SetFilter host no-op).
- **LOD / draw distance + FOV** (agent; templates/game/src/game/lod.cpp, fov.cpp; notes docs/notes/lod_fov.md),
  wired by the lead to `[graphics] lod` (0.5..4) and `fov_scale` (0.75..1.5), F1 Quality sliders "Level of detail" /
  "Field of view", copied into `g_lod_scale` / `g_fov_scale` each frame in hooks.cpp sub_0020B6F0. LOD scales the
  model LOD distance test (sphere test sub_002D45C0 from sub_000E62B0), mesh prefetch, prop cull_dist and the prop
  manager range, and the far clip (x min(lod,2), next race/free roam). FOV scales the camera projection setter
  sub_002D4AE0 for the camera's own callers only (mirror/HUD untouched). Built; untested in game.
- **HUD** (agent; docs/notes/hud_minimap_popup.md): minimap square = old full-channel clear, fixed by MaskedClear on
  Oct 20 21:42 (every capture showing it predates that). The off-screen "PRESS BACK" tab (last HUD draw, texture
  8363BC80) is placed by the game's own matrices; captures now log raw vertices, screen pixels and world/view/proj for
  small draws. **F4 in game = capture the next frame** (lead, runtime/app/main.cpp) — ask the user to press F4 while
  the popup shows, then read captures/<time>/frame.txt for that draw.
- 30 fps in menus/title background = a full-screen movie (1 draw, ~10.6 MB/frame upload of the HD picture); the game
  still steps 60/s (timing x1.000). Not a regression.
- **Texture extraction complete for everything offline-reproducible** (agent; tools/texture_pack/xbtex.py,
  extract_all.py, texfile.py, xbpf.py, xiso.py; notes docs/notes/texture_extraction.md): textures/source/ 4,776 ->
  12,714 PNGs (+105 .xbck P8, +286 .tex P8/P4, +546 .tex DXT, +1,378 .xbpf baked, +5,603 .xbpf gfxImage vinyls
  (may never match: game-tinted), +20 XPR read from the ISO because the loose media/*.xpr were rewritten by an old
  repack script). 739/741 runtime dumps covered (2 are movie frames). Palette hash: colours past a smaller palette's
  size hash as zero (ReadStagePalette). Lead started upscale.py on the new sources at ~12:00 (log TEMP/upscale2.log;
  rerun to resume).
- Crash dump triage: `lldb -b -c <dmp> mc3.exe -o "thread backtrace"` (scoop llvm) symbolizes with mc3.pdb.

### 0.3 Night of 2026-10-21/22 (lead session)

- **Garage crash fixed and verified in game** (garage agent; templates/game/src/game/garage.cpp): the list extension
  called Flash's set-member outside the player, where its context globals (0x5014D0.., pool at [0x5014D8]) are 0.
  Now sets them up (VmContext) and checks everything first. Verified by the lead driving the game: Rim Size 12"-28"
  (17 steps, was 6), Ride Height 29 steps (was 9; slams the Golf at rh_1300), no crash. New-step labels showed
  "!!RH_1300!!" (missing-key mode 2 creates "!!key!!" entries); fixed by matching placeholders — rebuilt, recheck.
  Player-only F1 tuning (mcPlayerFactory 0x2CBDE0) is in tuning.cpp.
- **Driving the game**: scratchpad scripts launch.sh (live pad file), press.sh (short 60 ms presses, half-size view
  at %TEMP%\view.png), step.sh. Held presses double-register in carousels. Route to the wheels:
  main menu > Go to Garage (5x DOWN, A, UP=Yes, A) > RIGHT=Customize > Customize carousel order Color, Body,
  Performance, Detail (Body = 1 LEFT of Performance) > Body Shop > LEFT=Wheels > Wheels: Rims, Tires, Dimensions >
  Dimensions: Rim Size, Tire Profile, Tire Width, Ride Height. NEVER pick Create Profile: a script mis-step created a
  "Player 2" profile (moved to scratchpad/accidental_save; user's save AE1A4832EEF5 untouched; backups on G:).
- **Backups**: full project G:\MC3_Backups\MidnightClub3-Port_2026-10-21 (robocopy, log G:\MC3_Backups\backup_2026-10-21.log);
  saves G:\MC3_Backups\saves_2026-10-21.
- **Textures**: re-upscaled with UltraSharp-4x (upscale_tools/models_extra, from Upscayl's repo) — keeps grain x4plus
  smoothed; per-axis padding: wrap only where opposite edges are continuous, reflect otherwise (skybox/decal seams);
  per-texture model tracked in textures/upscale_state.json; `upscale.py --model`. Vinyl masks (5,603 P8 in
  resources/vehicle/decal*.xbpf) excluded and their replacements moved to textures/replace_disabled (suspected
  cause of invisible vinyls; list textures/vinyl_hashes.txt).
- **Cutscenes**: second pass with realesrgan-x4plus (2.2x the fine detail of animevideov3, ~2.6 s/frame) in priority
  order (logos, intro, rewards, story, tutorials); per-movie model in video_hd/<movie>.model.txt; resumable
  (`tools/video/upscale_movies.py --model realesrgan-x4plus`, log TEMP/movies_x4.log). Logos+intro done.
- **Music overlap fixed** (audio agent; runtime/xdk/dsound.cpp): XACT prepares streams with Pause(3)
  (PAUSENOACTIVATE) and the host treated it as unpaused, so the next song / cutscene stream played on top; GetStatus
  never reported PLAYING so XACT recycled live voices. Built; needs a playtest (skip songs, cutscene transitions).
- **Replays fixed** (agent; templates/game/src/game/replay.cpp, hooks.cpp; docs/notes/replays.md): MC3 replays
  re-simulate from the recorded pad input ("RPY1" stream at 0x20E000) and check the player car's position every 30
  frames; the port's real-time catch-up made fixed steps vary (1-4 sixtieths), unrecorded, so playback desynced
  within ~0.5 s and ended. replay.cpp records each frame's step and forces it on playback; buffer 256 KB -> 1 MB
  (sub_0001DA90). Built; needs a playtest (log "replay: played to the end", never "left the recording").
- **Customization system mapped** (agent; docs/notes/customization_system.md, ~900 lines): all 51 part types,
  CarCfg fields, Flash VM hazards (fixed pools: 410 or 4000 symbols, full pool hangs; arrays max 128 from C++; GC
  only walks arrays), rim radius table 0x4B3940 (rim byte 29+ breaks), ApplyCarCfg re-runs on spawn/dirty 0x80,
  scrape sparks come from the chassis bound (0x121DE0, handler 0x1027D0) reaching the road via SuspensionOffset,
  neon = 0xFA5A0 four additive quads `fx_car_neon_glow` (palette alpha 0.3), paint/vinyl shader sources.
- **Garage verified again 02:30** (lead): Tire Width 23 steps (125-555 mm) with "NNN mm" labels; Ride Height 29 steps
  with 9"".. 16"" labels. Open: at extended low steps the garage preview sinks the wheels through the floor
  (scratchpad/garage_rh16.png) — garage agent asked to keep wheels on the floor, tuck them, and let the bound reach
  the road for sparks.
- In flight: visual fixes (neon, paint, vinyls, skybox; agent owns d3d12.cpp/shaders/app settings), garage (more tire
  widths, ground contact + scrape sparks, wheel tuck), replays (hooks.cpp, replay.cpp), PBR plan/tooling (tools/pbr,
  docs/PBR_PLAN.md), customization map (docs/notes/customization_system.md).

The six original agents in section 7 did not leave completed integration work/notes. This continuation's reviews
replace those pending assumptions. Audio EG/LFO, shader slots, pretransformed immediate vertices, full zero-symbol
verification, P8/linear/.tex/.xbpf coverage and FOV/LOD remain open. Do not mark the port fully verified.

---

## 1. What the user wants (in priority order, their words paraphrased)

1. Fully playable, no crashes, original console timing at 60 fps (races reached in prior playtests; full-game verification remains open).
2. Everything correct vs the original Xbox: text, headlights, minimap, menus, **NPC (pedestrian) animations**.
3. "Better than a remaster": HD textures for *every* texture, HD cutscenes, max texture detail, AA/sharpening,
   adjustable motion blur/glow/headlights, LOD, resolution.
4. Garage/tuning beyond limits: **ride height slider in the game's own Tuning menu**, extended wheel/tire sizes for
   all cars, extended garage limits.
5. No placeholders anywhere: every setting real or removed, every stub implemented.
6. Keep documentation very detailed (usage limits interrupt sessions).

User habits: they playtest themselves ("don't launch/drive the game" automation such as test races — never script a
race). They say "game closed" when they want a rebuild/relaunch; the lead may close the game whenever needed
(`node C:/Users/purpl/.claude/skills/game-development/game.mjs stop`). Standing permission to download/install any
tool. No vignette. They want background agents used.

---

## 2. Build, run, test

- Build: `powershell -NoProfile -ExecutionPolicy Bypass -File Build-MC3.ps1 -Jobs 8` from the repo root. Output:
  `templates/game/out/native/mc3.exe`. **Linking fails while mc3.exe runs** ("permission denied"): stop the game first.
  Compile errors still show while it runs, so a build with the game open is a valid compile check.
- Launch (lead's helper): `bash <scratchpad>/launch.sh` with env `MC3_SNAP=""` `MC3_SNAP_TEX=""`; scratchpad =
  `C:\Users\purpl\AppData\Local\Temp\claude\C--Users-purpl-Desktop-MidnightClub3-Port\a50b18e6-de59-49a8-b299-50e3c4893b51\scratchpad`.
  The game's own launcher config is `.codegpt-game.json` (env XBE_CAPTURE_SNAP_TEX etc.).
- Screenshot: `node C:/Users/purpl/.claude/skills/game-development/game.mjs shot`.
- Log: `templates/game/out/native/logs/mc3.log` (perf lines every second; "textures: N replacements" at start;
  "movies: ..." when a Bink opens; "timing: ..." every 10 s).
- Settings: `templates/game/out/native/settings.toml` ([graphics], [tuning]) and
  `templates/game/out/native/config/mc3_enhancements.toml` (the earlier agent's file; being cleaned up). Both were
  reset to defaults on 2026-10-21 at the user's request; backups `%TEMP%\settings.toml.bak`,
  `%TEMP%\mc3_enhancements.toml.bak`. F1 in game opens the settings overlay (Display / Vehicle / Quality / Picture /
  Info tabs).
- Frame capture: `XBE_CAPTURE_FRAME` / live `CAPTURE` command -> `templates/game/out/native/captures/<time>/frame.txt`
  (every draw, state, constants, shader HLSL files, BMP snapshots). Race capture with peds: `captures/211022`.

### Python tooling
`py -3.12` has Pillow, numpy, capstone. Git Bash heredocs mangle `\n` inside Python string literals (they become
real newlines): write patch scripts with the Write tool or use `chr(92)`; prefer the Edit tool for C++ edits.
Files are CRLF in places; patch scripts in the scratchpad normalise with `CR+LF` replacement.

---

## 3. Fixed this session (2026-10-21), with where

| Problem | Cause | Fix (file) |
|---|---|---|
| Title/menus invisible | opencode agent widened `DrawConstants::ext` to float[5] and emptied default `ShaderHooks::Constants()`, so the HLSL cbuffer and C++ struct disagreed (every constant after `wvp` read 16-20 bytes off) | `ext` back to `float ext[4]`, default hook returns `"    float4 ext;\n"` (runtime/gpu/shaders.{h,cpp}); MC3Extension fixed (runtime/gpu/mc3_extension.h): Constants returns the same float4, writes ext[0..3] only, no Shadow()/Output() overrides, no forced 1x MakeTarget (it rendered at 640x480) |
| game_info.cpp broken by opencode | unclosed namespace, stray lines | rewritten; installs MC3Extension from `.load` (templates/game/src/common/game_info.cpp) |
| **Stiff NPCs** | ped skinning vertex program (vs_48C366892849F012) scales bone indices (v12) by c105.w (D3D c9); c9 is written by the title straight into the push buffer (NV097_SET_TRANSFORM_CONSTANT_LOAD 0x1EA4 + SET_TRANSFORM_CONSTANT 0x0B80-0x0BFC); `Walk()` ignored those, c9 stayed 0, every vertex used bone 0 | `Walk()` in runtime/xdk/d3d8.cpp applies them to `g_state.vsc` (`g_const_load`, `g_const_part`). **Built, title verified, NOT yet confirmed in game by the user** — ask them |
| Motion blur too strong past 75 mph / glow / headlight pools fixed strength | none were adjustable | `D3D12Backend::ScaleScreenEffect` (runtime/gpu/d3d12.cpp) recognises MC3's post passes (half-size buffer laid over a full-screen target with a 4-vertex quad): SRCALPHA/INVSRCALPHA with depth test = speed blur (alpha scaled by `graphics.motion_blur`); SRCALPHA/ONE = glow if the source buffer was last filled by a full-screen copy (`Target::screen_copy`, runtime/gpu/ext.h) else headlight pools; pixel shaders apply `outc.a *= 1 + sblend.y; outc *= 1 + sblend.z` (shaders.cpp). User confirmed motion blur works |
| "Textures at max mip" | game clamps top mip (D3DTSS_MAXMIPLEVEL) and LOD bias | `graphics.texture_detail` 0 console / 1 Full (default: MinLOD 0, bias min(game,0)-0.5, 16x aniso) / 2 Max (MaxLOD 0) in `StageSampler` (d3d12.cpp); F1 Quality "Texture detail" |
| Tooltip strings with `\r` | `"textures\replace"` | escaped (settings_menu.cpp) |

Earlier-in-session fixes still valid: 60 fps fixed-step timing (hooks.cpp sub_0020BB00/sub_0020B6F0), headlights
(signed blend ops), dark text (SetPalette), FF vertex blending for skinned FF draws, texture pack runtime.

---

## 4. New systems built this session

### 4.1 HD texture pack (complete pipeline, verified bit-exact)
- Runtime: `runtime/gpu/texture_pack.{h,cpp}`; replacement looked up by `ContentHash` = Hash(shape 6×u32
  {format,w,h,depth,levels,cube}) → Hash(all mip bytes as in guest memory) → (P8 only) Hash(palette hash).
  `Hash` is in runtime/gpu/shaders.cpp (128-bit multiply mix). Replacements are any size, mips built on load.
  Textures uploaded 4+ times (video, render-to) are never replaced. F1 Quality: "HD texture pack", "Dump textures".
- Offline tools (`tools/texture_pack/`):
  - `dave.py` — reads "Dave" archives (format in §6.1). `list <archive> [glob]`, `extract <archive> <dir> [glob]`.
  - `xbtex.py` — bit-exact Python `Hash`, Xbox format table, finds D3D texture objects in `.xbck` memory images.
  - `extract_all.py` — every texture in every `.xbck` (5965 bundles, 76,051 instances, **4,776 unique**) decoded to
    `textures/source/<HASH>.png` + `textures/source_index.json` (hash → archive:file@offset fmt size). DXT/ARGB decode
    verified pixel-identical to runtime dumps (maxdiff 0); 725 of 741 runtime dumps covered by source.
  - `upscale.py` — Real-ESRGAN x4plus (`upscale_tools/realesrgan-ncnn-vulkan-v0.2.0-windows`) over `source/` + `dump/`
    → `replace/` (4x, 2x if >2048; wrap-padded edges for seamless tiling; alpha Lanczos; flat/tiny skipped;
    incremental; `--force`).
- Status at handoff: background job upscaling all 4,776 (3,347 written). If interrupted, rerun
  `py -3.12 tools/texture_pack/upscale.py` (it resumes). Pack is read at game start, so relaunch to see new ones.
- Not yet covered (agent working, §7): P8 textures (palette pairing), linear textures, cutscene `.tex`,
  `resources/vehicle/*.xbpf` page pools.

### 4.2 HD cutscenes (engine side complete, content in progress)
- Runtime `runtime/gpu/video_hd.{h,cpp}`: when the title opens a Bink, `video_hd/<path below video/, lower case>.mp4`
  beside the exe (e.g. `video_hd/story/sgameint.mp4`) is decoded by Media Foundation (Source Reader, RGB32, own
  thread per movie) in step with Bink's frame counter; `D3D12Backend::BindMovie` binds it instead of the movie
  texture. The title keeps decoding the Bink (audio/timing untouched). `[graphics] hd_movies` toggles.
  Links mfplat/mfreadwrite/mfuuid (cmake/XbeRecomp.cmake line 87).
- Game hooks `templates/game/src/game/movies.cpp`: BinkOpen 0x3AB590 (path capture), player open sub_003639C0
  (this+0 Bink, texture header [[this+4]+0x14]+8), update sub_003638A0 (frame = Bink FrameNum +0xC before update),
  close sub_00363B90. BinkCopyToBuffer 0x3AD280, BinkClose 0x3ADA70, BinkDoFrame 0x3AD2C0, BinkNextFrame 0x3AD8D0,
  BinkWait 0x3ACDF0. Movie texture is LIN_X8R8G8B8 of the movie size (640x480 or 720x480).
- Content: `tools/video/upscale_movies.py` — ffmpeg (winget Gyan.FFmpeg 9.0.2, not on PATH; script finds it under
  `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\ffmpeg-*\bin`) decodes Bink frames, Real-ESRGAN
  `realesr-animevideov3` x3 (`upscale_tools/realesrgan-20220424`, downloaded from the official v0.2.5.0 release)
  upscales in 400-frame chunks, frames stream into libx264 (crf 16, unsharp 5:5:0.5, yuv420p) → 1920x1440 (2160x1440
  for 720x480). Frame count verified equal to Bink's (rockstar: 210/210). 106 movies, ~63 min, ~113k frames;
  ~0.2 s/frame while sharing the GPU. Background job running at handoff, log `%TEMP%\movies.log`; rerun the script
  to resume (skips finished .mp4; `.part.mp4` are unfinished).
- **Not yet verified in game**: run the game with rockstar.mp4/sdlogo.mp4 present and check the logos show the HD
  file (log line "movies: ... from rockstar.mp4"). If the picture is black/garbled check RGB32 stride sign in
  video_hd.cpp Decode and that FrameNum mapping (1-based → 0-based) is right.

### 4.3 Vehicle tuning (F1 > Vehicle) — built, **untested by user**
- `templates/game/src/game/tuning.cpp`; settings `[tuning] ride_height_front/rear` (m, -0.5..0.5, + raises),
  `wheel_size_limit` (×, 1..3, default 1.6).
- vehSim visitor sub_000A3CA0 (wheel blocks at +0x54 front, +0x58 rear); wheel block visitor sub_000A6DA0:
  TravelLimit +0x40, TravelExtent +0x44, **SuspensionOffset +0x48**, SuspensionLimit +0x4C, SuspensionExtent +0x50.
  vehModel visitor sub_000FB760: SuspensionLimitF/R +0x0C/+0x10, **TireDiameterLimitF/R +0x14/+0x18**,
  TireExtentF/R +0x1C/+0x20, SuspensionClearanceF/R +0x24/+0x28, TireSteerLimit +0x2C, GroundClearance +0x30.
- Applied every frame from the time-manager hook (`ApplyVehicleTuning()` called in hooks.cpp sub_0020B6F0) to all
  tracked floats; each tracked value is fingerprinted by two neighbours so freed/reused memory is dropped, not written.
- Caveats: ride height applies to every car (traffic too) — needs the player-car pointer to restrict; whether the
  garage's rim list grows with TireDiameterLimit is unverified.
- **Still to do (user asked)**: put ride height into the game's own Tuning menu (Flash UI). Known: garage category
  switch at 0x13F5D0.. sets Flash arrays "RideHeightNames"/"RideHeightStatus" (type id 0x1F) and
  "RideHeightRearNames"/"RideHeightRearStatus"; category names list at 0x14F8A0 ("Ride Height", "Ride Height Rear",
  "Hydraulics", "Airbags"); CarCfg (save) bytes RideHeight0/1 +0x16/+0x17, TireWidth0/1 +0x18/+0x19, then RimSize,
  TireProfile; CarCfg field registration around 0x270F00 (parser call 0x32F3D0(type,name,ptr,1,0), type 6 float,
  3 byte, 8 vec3). The native options page code (`templates/game/src/game/graphics_page.cpp`, menu AddItem 0x348BE0,
  SetItemText 0x348D70, text lookup 0x0EA750) shows how to add items to the game's menus.

---

## 5. Reverse-engineering reference (addresses are guest VAs in default.xbe)

| What | Address / offset |
|---|---|
| Time manager (per frame) | sub_0020B6F0; fixed step 0x20BB00(rate, substeps); kStep 0x4B2D58; flags 0x534770/0x534776 |
| Text lookup (UI strings, UTF-16) | sub_000EA750 |
| UI object | [[0x50F148]+0xC], menu at ui+0xFC; AddItem 0x348BE0; SetItemText 0x348D70 |
| D3D SetPalette | 0x412580 (was wrongly 0 in game.toml once) |
| Vertex constant HLE | SetVertexShaderConstant1 0x414EB0, 1Fast 0x414F10, 4 0x414F60, NotInlineFast 0x415010 |
| Bink | see §4.2 |
| Tuning visitors | see §4.3 |
| Race post chain | headlight pools composite, glow (downsample 0849 + add 0850), speed blur (0868, vertex alpha = blur strength, e.g. 0.878) — capture 211022 |
| Ped skinning | vs_48C366892849F012: R2 = v12 * c[105].w; a0 = R2.z/R2.y...; bones c[a0+0..2] (palette from c-96) |
| Garage/tuning strings | RideHeightNames 0x458A9C (ref 0x13F63C), RimSizeNames 0x458DA8 (ref 0x13F28D), "Ride Height" 0x45B63C (ref 0x14F915) |

Disassembly helpers (scratchpad): `xdis.py <hexva> <bytes>` (annotates strings), `xref.py <strings...>`,
`text.pkl` (pickled full .text disassembly), `scan.py` (regenerates text.pkl), `peek.ps1` (live guest memory).

---

## 6. Game file formats

### 6.1 Dave archives (`templates/game/assets/assets_x.dat`, `_1`..`_5`, `banks.dat`)
Header {magic "Dave" (packed names) or "DAVE" (plain), u32 count, u32 dir size, u32 name size}; directory at 0x800,
16 bytes per file {name offset, data offset, size, packed size}; packed != size → raw deflate (zlib wbits -15); name
table at 0x800 + dir size. Packed names: 6-bit codes, LSB first, charset
`"\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~"` (+ code 48 unknown, 3 cutscene names); code ≥56 then next code x
= copy `(c-56) + (x-32)*8` chars of the previous entry's name (entries decoded in index order). Contents: base
archive = tune files, anims, cutscenes (.tex/.mesh/.anim/.skel), UI; `_2` = cars (resources/vehicle/vp_*/*.xbck);
`_1/_3/_4/_5` = one city each (resources/city/<city>_<time>_<weather>.xbck ~24 MB, props, peds, traffic). `.td` are
audio bank tables, not textures.

### 6.2 `.xbck` bundles
Baked memory image: header {u32 base VA, u32 version, u32 count, u32 size}, image from file offset 0x80 (= base VA).
Contains pointers (absolute, baked) and D3D resources {Common 0x0004xxxx, Data (physical), Lock 0, Format, Size};
pixels at `Data - (base & 0x0FFFFFFF) + 0x80`. MSVC debug fill 0xCDCDCDCD in padding.

### 6.3 Tune files (text, in the base archive)
`tune/vehicle/<car>_base.vehsim` (physics), `<car>.vehmodel` (garage fit limits), `<car>.vehmods` (upgrade
percentages), `customdata/<car>.carcfg` and `customdata/<car>/*.mccarcustom` (looks), `tune/camera/*.camtrackcs`
(cameras).

---

## 7. Work in flight (background agents, started 02:2x; resumed after a rate limit)

Each agent owns specific files, does not build, and writes notes to `docs/notes/<topic>.md`. **When they finish the
lead must: read each notes file, build (`Build-MC3.ps1`), fix compile errors, test, and fold results into this file.**

| Agent topic | Owns | Notes file |
|---|---|---|
| DirectSound envelope (SetEG, hit by MC3) + LFO | runtime/xdk/dsound.cpp, runtime/audio/* | docs/notes/audio_envelope_lfo.md |
| Verify every `= 0` symbol in game.toml against the XBE | templates/game/config/game.toml | docs/notes/symbol_audit.md |
| P8/linear/.tex/.xbpf texture extraction | tools/texture_pack/*, textures/source output | docs/notes/texture_extraction.md |
| LoadVertexShader/SelectVertexShader + pretransformed IM vertices | runtime/xdk/d3d8.cpp (+ state.h additive) | docs/notes/d3d8_shader_slots.md |
| Placebo settings: implement (deadzones etc.) or remove; MC3Extension dead code | runtime/app/app.h, settings.cpp, settings_menu.cpp, runtime/core/input.*, runtime/gpu/mc3_extension.*, graphics_page.cpp, config/mc3_enhancements.toml | docs/notes/settings_cleanup.md |
| LOD/draw distance + FOV reverse engineering | templates/game/src/game/lod.cpp, fov.cpp (new) | docs/notes/lod_fov.md |

Background shell jobs: texture upscale (`tools/texture_pack/upscale.py`), movie upscale
(`tools/video/upscale_movies.py`, log `%TEMP%\movies.log`). Both resume when rerun.

If an agent died: its partial edits are in the working tree (check `git diff` on its files) and its notes file.
Resume it (SendMessage to its id) or redo the task from its notes.

---

## 8. Known gaps / next steps (keep current)

1. Confirm with the user: NPC limbs animate; Vehicle tab works; HD movies show.
2. Integrate the six agents' work (§7), rebuild, test.
3. Ride height in the game's Tuning menu (§4.3) and restrict tuning to the player's car.
4. P8 & other textures into the pack (agent), then rerun `upscale.py` for new sources.
5. LOD / FOV (agent), then remove the F1 "Level of detail" Pending item and wire the setting.
6. Racer notification popup off-screen to the left (not investigated yet).
7. Minimap black square (fix landed earlier, never confirmed in game).
8. `graphics_page.cpp` native options page: disabled; needs a text page (HUD Options screen 0x08) rather than the
   Audio slider screen, and the crash the opencode agent reported needs a repro.
9. Bloom/SSAO/colour grading/UI scale: real post passes or game hooks, if kept.
10. Commit: nothing from this session is committed (`git status` is large). Commit in logical pieces when the user
    agrees; never commit the ISO, extracted assets or out/.

## 9. Pitfalls learned

- The cbuffer rule: any change to `DrawConstants` must be mirrored in the HLSL cbuffer in shaders.cpp, same order and
  16-byte packing; `ext` is one float4.
- game.toml `= 0` can be wrong; a missing XDK symbol silently drops work.
- Another tool (opencode) edited this repo concurrently once and broke the build; if files change under you, check
  `ls -la --time-style=full-iso` and ask the user to pause other agents.
- Don't run two builds at once (shared build dir). Don't run two GPU upscalers if you need speed (they share the
  RTX 3050).
- The title-screen UI is drawn into a 512×512 target and composited with blend ONE/ONE through the `kTexWide` spread
  path; it is a good canary for constant-layout bugs.
