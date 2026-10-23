# Reanalysis and continuation — 2026-10-21

This pass resumed from the previous handoff, checked the current working tree and running jobs, and repaired concrete integration/correctness defects. It did not launch or drive the game. Previous race playtests and their logs are historical evidence, not proof that the new build completes every race.

## Scope and evidence

- Inventoried source/configuration/build/launcher/documentation files and local root artifacts; saved a hash/size inventory in `.local/reanalysis-files.json`. Python, TOML and JSON parsing passed. Root PowerShell scripts also parse.
- Reviewed runtime GPU/render-thread/shader paths, Direct3D/DirectSound/Bink integration, app/settings/input, kernel/file/memory changes, game hooks, texture/video tooling and root extraction/repacking experiments. Followed the actual generated parser/timing functions where needed. Detailed source boundaries and unknowns are recorded in `docs/notes/`.
- Generated code, downloaded dependencies, large media outputs and original disc payloads were inventoried or validated by format/content checks as appropriate; this is not a claim that every translated instruction or third-party library was independently audited.
- The build verified USA title `0x54540079`, XBE size 5,099,520 bytes, SHA-1 `5bca1cf092edaadf876e8f80129900407a4a07b8`, SHA-256 `0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`. The saved source path currently selects the local copy of the original ISO. No disc or extracted game assets were modified.
- The native log recorded about 60 fps and timing near x1.000 in the prior run. Its latest crash dumps predate these changes. The root `mc3.exe` is an old copy; the canonical current executable is `templates/game/out/native/mc3.exe`.

## Repairs made

1. **Source/build integration:** removed the unused MC3 GPU extension and disabled native-Graphics-page prototype that still referenced deleted settings. The default shader cbuffer already includes the required one-float4 `ext`; no shader layout is changed by removing the extension. This also removes its small-texture type guess.
2. **Animation state transfer:** send WORLD0–WORLD3 to the render thread. The new fixed-function skinning code previously received only WORLD0. This source fix does not prove pedestrian animation is visually correct.
3. **Live frame-rate switching:** retain the original fixed step and substeps, restore them when disabling 60 fps midrace, and preserve the caller's cdecl rate argument. Previously changing to a 30 fps cap could retain a 1/60 simulation step and run at half speed.
4. **Tuning load order and stale addresses:** visitors register field addresses before parsing; activate tuning only after the actual matching parser returns. Clear unused pending fields on parser destruction, retire unavailable/fingerprint-mismatched addresses before access and reject nonfinite values. Committed-page checks are defensive; fingerprints are not complete allocation-generation tracking.
5. **Live HD selection:** changing the HD texture setting refreshes unchanged cached textures. Movies opened with HD off can enable their decoder later; disabling HD immediately selects the original picture. Original Bink audio/timing remain active.
6. **Working settings:** removed inert LOD storage/control and unsupported settings from the source enhancement template, and exposed the real HD movie toggle. Native Options/Tuning integration and FOV/LOD are still future work.
7. **Build/launch consistency:** unrelated host-settings TOML edits no longer trigger translation of the whole XBE. Batch/legacy launchers forward to the canonical PowerShell launcher. Rewrote the obsolete portable guide and ignored local ROM/media/tool/output artifacts in Git. Existing work was preserved; no commit was made.

Media ownership/decoder hardening and final validation are recorded at the end of this report.

## Tests

Standalone C++ checks exercise voice properties, Winsock negotiation, shader-stage behavior, keyboard/script mapping, actual title timing hooks and actual tuning hooks. The two new hook checks mock guest entry-point bodies and backing memory; they verify host integration logic, not all original game behavior.

Optional Windows media checks use completed local fixtures rather than starting the game. Configure `tests` with `-DMC3_HD_TEST_ROOT=<absolute templates/game/out/native directory>` to enable them. The movie test exercises actual Media Foundation decoding of `video_hd/rockstar.mp4`, 1920x1440 BGRA/opaque alpha, advancing frames and live original/HD selection. It does not prove orientation, every movie or final D3D12 composition.

Run disc-format checks with `python -m unittest discover -s tools/disc -p 'test_*.py' -v`. Build with `powershell -NoProfile -ExecutionPolicy Bypass -File Build-MC3.ps1 -Jobs 8`; test targets are separate in `tests/CMakeLists.txt`.

## Media jobs

The inherited supported texture job finished: **4,774 clean replacement PNGs**, with exactly 16 flat images and two tiny 8x8 images intentionally skipped. All final PNGs were checked. This is the supported source corpus, not all textures on disc. P8, linear, `.tex`, `.xbpf` and loose XPR coverage remains incomplete.

The inherited movie-upscale process continues and publishes completed MP4s. Do not start duplicate GPU jobs. Progress is in `%TEMP%/movies.log`. Restart the game after adding texture replacements because their directory is indexed once per process. The active runtime pipeline is under `tools/texture_pack/` and `tools/video/`; root extractor/repacker scripts are historical experiments, some with incorrect format assumptions. Do not copy their outputs over originals.

## What still prevents a fully verified port

- User playtest: main menu/world stability after this build, NPC limbs, Vehicle-tab behavior, actual tuning effects, minimap alpha, off-screen opponent popup and remaining car/window artifacts. No new gameplay result is claimed by this pass.
- Garage ride-height slider and player-only ownership. Current tuning affects loaded car data including traffic; extending garage selections and save behavior remains unverified.
- Actual audio envelope/LFO/filter/DSP behavior. The recorded run hits SetEG, which remains unimplemented. Research recovered descriptor packing and the important generator-0 filter/pitch versus generator-1 amplitude distinction; retrigger/release/scheduling behavior must be resolved before implementation.
- Shader-slot loading/selection, pretransformed immediate vertices and ordering of raw versus direct GPU constant/attribute writes. Their source gaps are documented; not every gap is observed in the latest playtest.
- Complete zero-symbol verification against the XBE, all texture formats, all HD movies and long-session GPU resource budgeting. Console-specific memory/display no-ops must be distinguished from missing game behavior.

## Research notes

- [Runtime/render/audio audit](notes/runtime_audit.md)
- [Settings/timing/tuning audit](notes/settings_tuning_audit.md)
- [Texture/video formats and job audit](notes/media_audit.md)
- [Audio envelopes/LFO evidence and unresolved semantics](notes/audio_envelope_lfo.md)

`HANDOFF.md` section 0 supersedes the old six-agent work-in-flight table; those claimed background tasks had not landed. The old technical addresses/history remain for reuse. The new build must not be described as a fully completed PC port solely because compilation and component tests pass.

## Final integration result (2026-10-21T03:06:17-06:00)

- Full native build passed: `templates/game/out/native/mc3.exe`, Windows AMD64 PE, 23,307,776 bytes, SHA-256 `0fdee8d519ad60a00669e91a8a40fb4f4272520b05be1f00d132aed68f7cc016`. Build log: `.local/reanalysis-build.log`.
- **8/8 C++ checks passed**, including actual WIC texture decode/independent ownership and Media Foundation playback/toggle/loop/data-reuse checks. Test log: `.local/reanalysis-tests.log`. **8/8 Python disc checks passed**. Python/TOML/JSON and root PowerShell parsing passed; `git diff --check` passed.
- Replacement images now have caller-owned decoded storage that is released after upload. The pack retains only its index and used-hash statistics, avoiding the observed approximately 7 GiB potential CPU image cache. GPU resource budgeting remains a separate open issue.
- Movie decoding now checks COM/MF creation, output type/dimensions/fps/stride, sample timestamps and full buffer extents before publishing. It cleans up COM on early exits, restores original-picture selection on decoder failure, retires reused data/header registrations and assigns globally fresh cache serials. The test validates normal decoding/loop/reuse; exhaustive malformed-media/stride-orientation testing remains open.
- No game session was started and no keyboard/controller input was automated. Current gameplay/visual correctness still requires user playtesting. No commit or external publication was performed.
