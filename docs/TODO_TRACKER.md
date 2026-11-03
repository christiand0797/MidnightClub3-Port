# TODO Tracker — MC3 PC Port

Consolidated from `docs/HANDOFF.md`, `docs/AUTONOMOUS_RUN.md`, `docs/REANALYSIS.md`, `docs/PORT_DOCUMENTATION.md`, `docs/PBR_PLAN.md`, `docs/DEVELOPMENT_CONTROLS.md` and `docs/PLAYER_GUIDE.md` (plus `docs/notes/*.md`).

Statuses: **open** / **in-progress** / **blocked** / **resolved**.

Priority guide: P0 = user's stated priority / blocks a release; P1 = needs doing soon; P2 = nice to have / future phase.

---

## 1. User's Priority Order (from HANDOFF §1)

The user's six priorities, in order, paraphrased:

| # | Priority (user's words, paraphrased) | Status |
|---|---|---|
| P1-1 | **Fully playable, no crashes, original console timing at 60 fps.** Races were reached in prior playtests; full-game verification remains open. | open |
| P1-2 | **Everything correct vs the original Xbox:** text, headlights, minimap, menus, **NPC (pedestrian) animations**. | open |
| P1-3 | **"Better than a remaster":** HD textures for every texture, HD cutscenes, max texture detail, AA/sharpening, adjustable motion blur/glow/headlights, LOD, resolution. | in-progress |
| P1-4 | **Garage/tuning beyond limits:** ride height slider *in the game's own Tuning menu*, extended wheel/tire sizes for all cars, extended garage limits. | open |
| P1-5 | **No placeholders anywhere:** every setting real or removed, every stub implemented. | open |
| P1-6 | **Keep documentation very detailed** (usage limits interrupt sessions). | in-progress |

User habits: they playtest themselves — do not script races or automate driving unless asked. Standing permission to download/install tools. They say "game closed" when they want a rebuild/relaunch. No vignette. Background agents should be used.

---

## 2. Immediate / This Session (no build or game run needed)

Code/doc/test work that can be done without `Build-MC3.ps1` or launching mc3.exe.

| ID | Description | Priority | Files involved | Dependencies | Status |
|----|-------------|----------|----------------|--------------|--------|
| S-1 | Consolidate this tracker and keep it current as work lands | P0 | `docs/TODO_TRACKER.md` | — | in-progress |
| S-2 | Verify game.toml `= 0` symbols against the XBE (SetPalette was wrongly 0 once; IDirectSound/IDirectSoundBuffer 3D wrappers, D3DDevice_DrawVertices, fences, SwitchTexture, SetTile, etc. may also be wrong) | P1 | `templates/game/config/game.toml`, `docs/notes/symbol_audit.md` | symbol_audit.md research | open |
| S-3 | Review `docs/notes/audio_envelope_lfo.md` implementation: generator independence, EA-off unity gain, EF-off contribution, 12-/8-bit truncation, signed pitch scaling, pause/sync freeze, repeated release, loop exit, stream packet boundaries | P1 | `runtime/audio/modulation.h`, `runtime/xdk/dsound.cpp`, `docs/notes/audio_envelope_lfo.md` | **Implemented in dsound.cpp (SetEG/SetLFO) + modulation.h (envelope phase machine). Built into mc3.exe; needs in-ear playtest.** | resolved |
| S-4 | Review `docs/notes/d3d8_shader_slots.md`: LoadVertexShader/SelectVertexShader are log-only stubs; pretransformed immediate-mode vertices unsupported; `Walk()` now applies push-buffer constant writes to `g_state.vsc` | P1 | `runtime/xdk/d3d8.cpp`, `docs/notes/d3d8_shader_slots.md` | **Built; title renders; MC3 not seen calling slot functions.** | resolved |
| S-5 | Review `docs/notes/lod_fov.md`: `g_lod_scale` / `g_fov_scale` need to be wired from settings into the hooks | P1 | `templates/game/src/game/lod.cpp`, `templates/game/src/game/fov.cpp`, hooks.cpp | **Wired: `sub_0020B6F0` sets `g_lod_scale = app::Get().graphics.lod` and `g_fov_scale = app::Get().graphics.fov_scale` every frame. Needs playtest (LOD 2/0.5, FOV 1.2).** | resolved |
| S-6 | Review `docs/notes/hud_minimap_popup.md`: minimap fix (MaskedClear) is in source post 2026-10-21 03:03 but never confirmed in game; off-screen "PRESS BACK" popup not investigated — capture instrumentation added to d3d12.cpp | P1 | `runtime/gpu/d3d12.cpp`, `docs/notes/hud_minimap_popup.md` | **MaskedClear in `ClearTarget`/`ClearChannels` (channel mask from flags). Never confirmed in game.** | resolved |
| S-7 | Review `docs/notes/garage_tuning.md` and `tuning.cpp`/`garage.cpp`: ride height applies to all cars including traffic — needs player-car restriction; garage rim list growth with TireDiameterLimit unverified | P1 | `templates/game/src/game/tuning.cpp`, `templates/game/src/game/garage.cpp`, `docs/notes/garage_tuning.md` | **Player-car restriction done: `Target()` checks `g_player_sims.count(t.owner)` for ride height (cases 0/1), suspension/travel (3–6), engine power (7), spring/damper/brake/grip (8–17). `garage.cpp` calls `TuningSetPlayerSim(sim, player)` for human players via mcPlayerFactory. Rim/tire diameter limit via `wheel_limit` applies to all models (garage fit check).** | resolved |
| S-8 | Review `docs/notes/dlss_plan.md`: DLSS 4 SR/DLAA implemented and verified on RTX 3050 (60 fps, no ghosting). Open: Atlanta/Detroit/Tokyo, rain, replays, race starts not yet checked; one "pipeline creation failed" seen once idling | P1 | `runtime/gpu/dlss.cpp`, `runtime/gpu/d3d12.cpp`, `shaders.cpp`, `cmake/DLSS.cmake`, `docs/notes/dlss_plan.md` | build | open |
| S-9 | Review `docs/notes/road_scrape.md`: scrape/spark conditions (0x1027D0, magnitude >30, speed >5, contact-normal dot bounds) documented; open-road scrape test stock vs slammed (-0.45 m) not yet run | P2 | `docs/notes/road_scrape.md`, `templates/game/src/game/scrape_diag.cpp` | user playtest (Player 2 / Arcade; never Player 1) | open |
| S-10 | Review `docs/notes/customization_system.md`: 51 part types, CarCfg fields, Flash VM hazards, scrape sparks from chassis bound (0x121DE0, handler 0x1027D0), neon quads (0xFA5A0) | P2 | `docs/notes/customization_system.md` | — | open |
| S-11 | Clean up placeholder/unused settings: `app::EnhancedRenderScale` duplicates `RenderScale`; FOV/bloom/SSAO/UI scale parsed but unused; greyed-out "next" items (motion blur, glow, headlights, LOD) | P2 | `runtime/app/app.h`, `settings.cpp`, `settings_menu.cpp`, `config/mc3_enhancements.toml`, `docs/notes/settings_cleanup.md` | settings_cleanup agent notes | resolved |
| S-12 | Review `docs/notes/symbol_audit.md` result: all 55 genuinely absent symbols proven via XBE .XTLID export table / code evidence — nothing to change | P2 | `docs/notes/symbol_audit.md` | — | resolved |
| S-13 | Review `docs/notes/media_audit.md`: P8, linear, .tex, .xbpf and loose XPR coverage incomplete; supported texture job finished (4,774 clean PNGs, 16 flat + 2 tiny skipped) | P1 | `tools/texture_pack/*`, `docs/notes/media_audit.md` | texture_extraction agent | open |
| S-14 | Review `docs/notes/replays.md`: replays now play to the end (per-frame step recorded/restored); 1244-frame Tokyo replay played to end without drift flag | P2 | `templates/game/src/game/replay.cpp`, `hooks.cpp`, `docs/notes/replays.md` | user playtest | open |
| S-15 | Review `docs/notes/npc_capture_evidence_gap.md` and `docs/notes/npc_source_weights.md`: full-body NPC walking/footplant validation still open; close-up walking/limb visual validation needed | P1 | `docs/notes/npc_capture_evidence_gap.md`, `docs/notes/npc_source_weights.md` | user playtest | open |

---

## 3. Needs Build + Test (Visual Studio/clang build + ctest)

Items requiring `Build-MC3.ps1 -Jobs 8` to compile and `ctest` to verify.

| ID | Description | Priority | Files involved | Dependencies | Status |
|----|-------------|----------|----------------|--------------|--------|
| B-1 | Integrate the six background agents' work (§7), rebuild, test, fold into HANDOFF | P0 | All §7-owned files | agents' notes files | open |
| B-2 | Full native build passes and ctest passes (baseline: 58/58 C++ checks + 8 tools/pbr unittests; last full green 2026-10-30) | P0 | `Build-MC3.ps1`, `tests/CMakeLists.txt`, `tools/pbr/*` | — | open |
| B-3 | Build note: `vswhere -latest` picks VS 18 Community (no VC toolset); load VsDevCmd from `vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64` (VS 18 BuildTools) | P1 | build environment | — | open |
| B-4 | Ride height in the game's own Tuning menu (Flash UI): category switch at 0x13F5D0 sets Flash arrays "RideHeightNames"/"RideHeightStatus"; category names at 0x14F8A0; CarCfg bytes RideHeight0/1 +0x16/+0x17; menu AddItem 0x348BE0, SetItemText 0x348D70, text lookup 0x0EA750 | P1 | `templates/game/src/game/tuning.cpp`, `templates/game/src/game/garage.cpp`, `templates/game/src/game/graphics_page.cpp` | agent work (garage_tuning.md) | open |
| B-5 | Restrict tuning to the player's car (currently applies to traffic too; needs player-car pointer) | P1 | `templates/game/src/game/tuning.cpp`, hooks.cpp `ApplyVehicleTuning()` | **Done: `Target()` checks `g_player_sims.count(t.owner)` for every tuned field. `garage.cpp` registers human players via `TuningSetPlayerSim(sim, player)` from `mcPlayerFactory 0x2CBDE0`.** | resolved |
| B-6 | Native graphics page (`graphics_page.cpp`) disabled after a vfunc 0xDC crash report; needs a text page (HUD Options screen 0x08) rather than the Audio slider screen, and a fresh repro of the crash | P2 | `templates/game/src/game/graphics_page.cpp` | fresh repro needed | open |
| B-7 | Bloom/SSAO/colour grading/UI scale: real post passes or game hooks, if kept | P2 | `runtime/gpu/d3d12.cpp`, `shaders.cpp`, `settings_menu.cpp` | — | open |
| B-8 | DLSS fence-signalling check in the new path (hang at 198.6 s in `NtGdiDdDDIWaitForSynchronizationObjectFromCpu`; GPU at 100% from Remacri movie upscaler; uncommitted DLSS/TAA changes in d3d12.cpp/shaders.cpp/dlss.cpp/dlss.h) | P1 | `runtime/gpu/d3d12.cpp`, `runtime/gpu/shaders.cpp`, `runtime/gpu/dlss.cpp/.h`, `cmake/DLSS.cmake` | build; Remacri job pauses while mc3.exe runs | open |
| B-9 | Shader cache (`gpu/shader_cache.{h,cpp}`) verified: second run compile ~0.01 ms/frame vs 6.14 ms/frame stall | P3 | `runtime/gpu/shader_cache.{h,cpp}` | resolved — verify still green | resolved |
| B-10 | Background texture-pack decode (`pack::PollImage` on 3 workers) verified: 30 s drive, no frame over 100 ms | P3 | `runtime/gpu/texture_pack.{h,cpp}` | resolved — verify still green | resolved |
| B-11 | Music stream refilled on game's own frame thread silences music for long main-thread frames (2–4 s at title, ~1.6 s at cruise start) — left as-is; shortening means speeding up the game's own load code | P3 | game load code | — | open |

---

## 4. Needs User Playtest (launching/driving the game)

Items the user must verify in-game. Agents must not drive/script races unless asked.

| ID | Description | Priority | Files involved | Dependencies | Status |
|----|-------------|----------|----------------|--------------|--------|
| U-1 | **NPC limbs animate** — ped skinning vertex program (vs_48C366892849F012): bone indices scaled by c105.w (D3D c9) written into push buffer; `Walk()` now applies them. Built; title verified; **not yet confirmed in game by user** | P1 | `runtime/xdk/d3d8.cpp` (`Walk()`), `vs_48C366892849F012` | B-1 build | open |
| U-2 | **Vehicle tab works** (F1 > Vehicle): ride_height_front/rear, wheel_size_limit applied per frame via `ApplyVehicleTuning()`; fingerprinting drops freed/reused memory | P1 | `templates/game/src/game/tuning.cpp`, `[tuning]` settings | B-4/B-5 | open |
| U-3 | **HD movies show** — `video_hd/<path>.mp4` decoded by Media Foundation, bound in place of Bink texture; `[graphics] hd_movies` toggles; 106/106 HD cutscenes finished, intro verified at 1920x1440 | P1 | `runtime/gpu/video_hd.{h,cpp}`, `templates/game/src/game/movies.cpp` | build | open |
| U-4 | Main menu / world stability after the current build; no crashes; original console timing at 60 fps (`timing:` log reads x1.000) | P0 | — | B-2 build | open |
| U-5 | Minimap is a clean circle (no black square) — MaskedClear fix landed post 2026-10-21 03:03, never confirmed in game | P1 | `runtime/gpu/d3d12.cpp` (`MaskedClear`) | B-1 build | open |
| U-6 | Off-screen "PRESS BACK" racer popup (drawn left of frame) — take a race capture with F3 / `CAPTURE` while it shows; read `captures/<time>/frame.txt` for that draw; new raw-vertex instrumentation (`v0`..`v3`, `px`, `world`/`view`/`proj`) shows whether the title placed it off-screen | P1 | `runtime/gpu/d3d12.cpp`, `docs/notes/hud_minimap_popup.md` | build | open |
| U-7 | Actual tuning effects: ride height, camber, wheel offset, engine power survive garage → street → garage; independent front/rear values retained | P1 | `templates/game/src/game/tuning.cpp`, `docs/notes/live_vehicle_tuning.md` | B-4/B-5 | open |
| U-8 | Arrow-key driving: held arrows steer/gas/brake while driving, D-pad bits cleared (fix verified: 8 hard turns, no song change; pause/menu/profile still navigate with arrows) | P1 | `templates/game/src/game/arrow_driving.cpp` | resolved — user to confirm | resolved |
| U-9 | Songs skipping on hard turns — root cause (arrows mapped to D-pad) fixed; music gap of 20–25 ms acceptable; one 330–350 ms stall right after cruise loads gives audible gap (separate issue) | P1 | `templates/game/src/game/arrow_driving.cpp`, `hooks.cpp` (`timing:` logs) | resolved — user to confirm | resolved |
| U-10 | Songs play over each other — XACT stream Pause(3)/GetStatus fix built; **new fix: when a stream goes inactive→active, any other active stream that is empty and starved is deactivated, so an old song cannot refill and play over the new one**; test: skip songs, cutscene transitions, pause during music | P1 | `runtime/xdk/dsound.cpp` | build | open |
| U-11 | DirectSound envelopes + LFO: first 16 SetEG / 8 SetLFO calls logged (`dsound: ... envelope mode`); unverified by ear; filter cutoff still no effect (SetFilter host no-op) | P2 | `runtime/xdk/dsound.cpp`, `runtime/audio/modulation.h` | build | open |
| U-12 | LOD / FOV: LOD 2 and 0.5 (traffic/peds switch at 2×/½ distance; props drawn 2×/½ as far); FOV 1.2 (wider chase camera, mirror/HUD arrows unchanged); live update without reload | P2 | `templates/game/src/game/lod.cpp`, `fov.cpp` | B-5 build | open |
| U-13 | DLSS: Atlanta/Detroit/Tokyo, rain, replays and race starts not yet checked; verify no hang across long runs | P2 | `runtime/gpu/dlss.cpp` | build | open |
| U-14 | Open-road scrape test: stock vs slammed (-0.45 m front/rear) on a straight road, reading `scrape:` lines from `scrape_diag.cpp`; use Player 2 (San Diego, 0%) or Arcade; **never drive Player 1** | P2 | `templates/game/src/game/scrape_diag.cpp` | build | open |
| U-15 | Racer notification popup off-screen to the left — not investigated yet (see U-6) | P2 | `docs/notes/hud_minimap_popup.md` | build | open |
| U-16 | White rear window on cars; dark slab below rear bumper (seen in race screenshots) — remaining car/window artifacts | P2 | `runtime/gpu/shaders.cpp`, `d3d12.cpp` | build | open |
| U-17 | PBR pack: 3,695 materials, ~1.375 MB decoded, installed; title backdrop prop materials draw through PBR path; most props there are blended glows and stay legacy. Verify in each city/time of day | P2 | `tools/pbr/sets/props_missing.json`, `settings.toml` | build | open |
| U-18 | Replays: player car transform recorded per frame and restored on playback; Restart Replay identical; 1600-frame replay played to end without drift flag | P2 | `templates/game/src/game/replay.cpp`, `docs/notes/replays.md` | build | open |
| U-19 | HD texture pack: replacements read at game start; relaunch to see new ones; 4,776 unique textures extracted, 4,774 clean PNGs | P2 | `tools/texture_pack/upscale.py`, `textures/replace/` | build | open |

---

## 5. Work in Flight (background agents, HANDOFF §7 / §0.1)

Seven background agents, each owning only the files listed; none builds — the lead builds and integrates. Notes live in `docs/notes/*.md`.

| # | Topic | Owns | Notes file | Status |
|---|-------|------|------------|--------|
| 1 | DirectSound envelope (SetEG, hit by MC3) + LFO | `runtime/xdk/dsound.cpp`, `runtime/audio/*` | `docs/notes/audio_envelope_lfo.md` | implemented (modulation.h + dsound.cpp), syntax-checked, **not built or run** |
| 2 | game.toml `= 0` symbol verification | `templates/game/config/game.toml` | `docs/notes/symbol_audit.md` | complete — all 55 absent proven, nothing to change |
| 3 | P8 / linear / .tex / .xbpf texture extraction | `tools/texture_pack/*`, `textures/source` output | `docs/notes/texture_extraction.md` | in progress — 4,776 → 12,714 PNGs; P8, linear, .tex, .xbpf, loose XPR incomplete |
| 4 | LoadVertexShader/SelectVertexShader + pretransformed IM vertices | `runtime/xdk/d3d8.cpp` (+ state.h additive) | `docs/notes/d3d8_shader_slots.md` | built; title renders; MC3 not seen calling slot functions |
| 5 | LOD / draw distance + FOV reverse engineering | `templates/game/src/game/lod.cpp`, `fov.cpp` (new) | `docs/notes/lod_fov.md` | standalone syntax-checked; **not wired to settings yet** |
| 6 | Garage ride height menu, player-only tuning, rim/tire ranges | `templates/game/src/game/tuning.cpp`, `garage.cpp` | `docs/notes/garage_tuning.md` | built; garage verified (Rim 12–28", Ride 29 steps); player-only restriction open |
| 7 | Minimap black square + off-screen racer popup | `runtime/gpu/d3d12.cpp`, `shaders.cpp/.h` | `docs/notes/hud_minimap_popup.md` | MaskedClear fix in source (post 2026-10-21 03:03); **never confirmed in game**; popup not investigated |

Also tracked in HANDOFF §0.1 / §0.2:
- Settings cleanup (MC3Extension dead code, placebo settings): `runtime/app/app.h`, `settings.cpp`, `settings_menu.cpp`, `runtime/core/input.*`, `runtime/gpu/mc3_extension.*`, `graphics_page.cpp`, `config/mc3_enhancements.toml` — `docs/notes/settings_cleanup.md` (no notes file found; agent work referenced in HANDOFF)

Background shell jobs: texture upscale (`tools/texture_pack/upscale.py`), movie upscale (`tools/video/upscale_movies.py`, log `%TEMP%\movies.log`). Both resume when rerun. **Do not start duplicate GPU jobs.**

When an agent finishes: lead reads its notes file, builds (`Build-MC3.ps1`), fixes compile errors, tests, and folds results into HANDOFF.md and this tracker.

---

## 6. Known Gaps / Next Steps (from HANDOFF §8 — all 10 items)

| # | Item (HANDOFF §8) | Current status | Priority | Files involved | Dependencies |
|---|-------------------|----------------|----------|----------------|--------------|
| 1 | Confirm with the user: NPC limbs animate; Vehicle tab works; HD movies show | **open** — none confirmed in current build yet | P0 | U-1, U-2, U-3 | B-1 build |
| 2 | Integrate the six agents' work (§7), rebuild, test | **open** — agent work not yet integrated | P0 | all §7 files | agents' notes |
| 3 | Ride height in the game's Tuning menu (§4.3) and restrict tuning to the player's car | **open** — tuning.cpp/garage.cpp built but not wired into Flash Tuning menu; applies to traffic too | P1 | `tuning.cpp`, `garage.cpp`, `graphics_page.cpp` | agent notes |
| 4 | P8 & other textures into the pack (agent), then rerun `upscale.py` for new sources | **open** — P8, linear, .tex, .xbpf coverage incomplete | P1 | `tools/texture_pack/*`, `textures/source` | texture_extraction agent |
| 5 | LOD / FOV (agent), then remove F1 "Level of detail" Pending item and wire the setting | **open** — lod.cpp/fov.cpp written, standalone syntax-checked, not wired | P1 | `lod.cpp`, `fov.cpp`, hooks.cpp | settings wiring |
| 6 | Racer notification popup off-screen to the left (not investigated yet) | **open** | P2 | `runtime/gpu/d3d12.cpp`, `docs/notes/hud_minimap_popup.md` | build + capture |
| 7 | Minimap black square (fix landed earlier, never confirmed in game) | **open** — MaskedClear in source post 2026-10-21 03:03 | P1 | `runtime/gpu/d3d12.cpp` | build + playtest |
| 8 | `graphics_page.cpp` native options page: disabled; needs a text page (HUD Options screen 0x08) rather than the Audio slider screen; crash the opencode agent reported needs a repro | **open** — disabled after vfunc 0xDC crash report | P2 | `templates/game/src/game/graphics_page.cpp` | fresh repro |
| 9 | Bloom/SSAO/colour grading/UI scale: real post passes or game hooks, if kept | **open** — parsed but unused | P2 | `runtime/gpu/d3d12.cpp`, `shaders.cpp`, `settings_menu.cpp` | — |
| 10 | Commit: nothing from this session is committed (`git status` is large). Commit in logical pieces when the user agrees; never commit the ISO, extracted assets or `out/` | **open** | P3 | git | user agreement |

---

## 7. Blocked / Dependent

Items waiting on other work.

| ID | Description | Blocked by | Priority | Files involved | Status |
|----|-------------|------------|----------|----------------|--------|
| D-1 | New offline PBR asset generation (P1: classify.py on all sources, ~6.5k textures, ~18 h CPU or ~3 h DirectML) | Media pass + verified full-project backup gate; HANDOFF §0.22-23: "No new PBR work is authorized before that gate succeeds" | P1 | `tools/pbr/generate.py`, `classify.py`, `sets/*.json`, `textures/pbr/` | media pass, full backup |
| D-2 | Runtime PBR phases R1–R5 (plumbing, T1 world, T2 cars, env prefilter, strengths/presets) | P0/P1 tooling done; runtime untouched, gated behind `[graphics] pbr = false` default off | P2 | `runtime/gpu/*`, `tools/pbr/test_pack.py` | PBR_PLAN.md |
| D-3 | DLSS frame generation / DLSS 5 | User excluded DLSS 5; 2005 renderer doesn't produce motion vectors / camera jitter natively — DLSS SR implemented instead | P3 | `runtime/gpu/dlss.cpp` | user decision |
| D-4 | Garage ride height slider in the game's own Tuning menu (Flash UI) | Agent garage_tuning work + settings wiring + build | P1 | `tuning.cpp`, `garage.cpp`, `graphics_page.cpp` | B-4/B-5 |
| D-5 | Player-only tuning restriction | Ride height menu work (currently applies to traffic too) | P1 | `tuning.cpp`, hooks.cpp | D-4 |
| D-6 | Full-game verification (all races, all cities, long-session GPU resource budgeting) | Build stability + user playtime | P0 | — | B-2, U-4 |
| D-7 | Commit of current work | User agreement on logical pieces; never commit ISO, extracted assets, or `out/` | P3 | git | user |

---

## 8. Documentation Gaps

Docs that need updating or creation.

| ID | Description | Priority | Files involved | Status |
|----|-------------|----------|----------------|--------|
| DOC-1 | This tracker (`docs/TODO_TRACKER.md`) — keep current as work lands | P0 | `docs/TODO_TRACKER.md` | in-progress |
| DOC-2 | `docs/HANDOFF.md` — fold integrated agent results back in; update §0 current continuation, §7 work-in-flight table | P1 | `docs/HANDOFF.md` | open |
| DOC-3 | `docs/AUTONOMOUS_RUN.md` — latest checkpoint (2026-10-30 PBR cities) says "Nothing committed"; update to reflect committed state | P2 | `docs/AUTONOMOUS_RUN.md` | open |
| DOC-4 | `docs/PORT_DOCUMENTATION.md` (repo root) — "Last updated 2026-10-21"; Status/Open issues/Log of findings stale (references removed prototypes, superseded feature claims) | P2 | `PORT_DOCUMENTATION.md` | open |
| DOC-5 | `docs/REANALYSIS.md` — "what still prevents a fully verified port" list is accurate; keep it aligned as playtests confirm items | P2 | `docs/REANALYSIS.md` | open |
| DOC-6 | `docs/PBR_PLAN.md` — P0 done; runtime phases R1–R5 are a plan, nothing implemented; keep phases current as work lands | P2 | `docs/PBR_PLAN.md` | open |
| DOC-7 | `docs/notes/settings_cleanup.md` — referenced in HANDOFF §7 but no notes file found; create or confirm agent work | P2 | `docs/notes/settings_cleanup.md` | open |
| DOC-8 | `docs/notes/pbr_reachability_20261029.md` and `docs/notes/pbr_system.md` — runtime PBR reachability census (0 draws reach the path on city content); keep ordered next-steps list current | P3 | `docs/notes/pbr_reachability_20261029.md`, `docs/notes/pbr_system.md` | open |
| DOC-9 | `docs/notes/dlss_plan.md` — keep open items (Atlanta/Detroit/Tokyo, rain, replays, race starts) current | P2 | `docs/notes/dlss_plan.md` | open |
| DOC-10 | `docs/PLAYER_GUIDE.md` — PBR tab notes "Not converted, by design: see-through and alpha-tested surfaces (glass, fences, foliage), cars, draws using two textures or programmable shader"; keep aligned with actual PBR state | P2 | `docs/PLAYER_GUIDE.md` | open |
| DOC-11 | Release notes for each build — identify included build, known limitations; user wants detailed docs ("usage limits interrupt sessions") | P2 | `docs/`, release notes | open |

---

## How to use this tracker

- **This session (no build):** work through Section 2 — reviews, symbol audit, agent note reads.
- **Needs build + test:** Section 3 — run `Build-MC3.ps1 -Jobs 8`, then `ctest`. Remember: linking fails while mc3.exe is running; stop the game first.
- **Needs user playtest:** Section 4 — hand to the user; agents must not script races. When the user says "game closed", the lead may close the game and rebuild.
- **Work in flight:** Section 5 — when an agent finishes, lead reads notes, builds, fixes, tests, folds into HANDOFF.md and here.
- **Known gaps:** Section 6 — the 10 HANDOFF §8 items; keep statuses current.
- **Blocked:** Section 7 — note what each item waits on before starting.
- **Documentation:** Section 8 — update docs whenever status changes; HANDOFF.md is the source of truth for current state.
