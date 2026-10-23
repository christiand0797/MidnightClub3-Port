# Release validation — v0.3.0-dev.20261023

This records the native build and content tested locally on 2026-10-23. Consult the release notes for available
assets and publication state; this validation record does not itself assert that the tag has been published.
The target is `v0.3.0-dev.20261023` in `christiand0797/MidnightClub3-Port`.

## Tested native build

Canonical executable: `templates/game/out/native/mc3.exe`, SHA-256
`72ade3b85ea2292c57867f5ae6d635a20304d83d9830742c1c96b85eca12c5ab`.

| Check | Result and scope |
|---|---|
| Runtime CTest | **19/19 pass**, including the original generated vehicle refresh/cache instructions |
| Snapshot publisher regressions | **17/17 pass**; [fixtures](../../tools/maintenance/test_snapshot_publish.py) cover archive listing, metadata/coverage and publication safeguards |
| Portable release publisher regressions | **7/7 pass**; [fixtures](../../tools/release/test_publish_portable.py) exercise draft ownership/collisions without contacting GitHub |
| Full content authentication | All eight parts, whole archive and **7,188 per-file** size/SHA-256 records verified |
| Actual clean offline first launch | **Pass** through full installation, desktop shortcut, native D3D12 startup and automatic HD intro playback |
| Native live vehicle update/retention | **Pass on the Golf R32**, with actual physics/cache/render readbacks before and after leaving the garage |

The local test outputs are `.local/final-refresh-tests.log`, `.local/final-snapshot-tests.log` and
`.local/publisher-tests.log`. Fixture results are distinct from completed network publication or a full game
compatibility sweep.

## Full first-launch installation

The parent launched the actual canonical EXE in the new external folder
`G:/MC3_Backups/release-work/first-launch-qa-20261023`. There was no `--disc` argument, existing installed
`game`/media tree, supplied settings/config or seeded save folder. All eight matching offline parts were placed
beside the EXE, with the app-local CRT and normal pinned content metadata. The pre-launch inventory is preserved
in that folder's `prepare-manifest.json`; it describes preparation, not the later installed state.

The archive contains **14,124,869,766 bytes**, SHA-256
`3b3f311f7800430fd0b445952f2a8fffe86f58276f639bbb850e006d0848c1ab`.
Its **7,188 authenticated files** are one supported ISO, 7,081 texture PNGs and 106 movies. Installed recursive
movie and texture counts agree with those inventory records. The original XBE loaded from the new
`game/game.iso` has SHA-1 `5bca1cf092edaadf876e8f80129900407a4a07b8`.

The actual first-launch log records full content installation at `593.115 s`, loading the new ISO at
`593.315 s`, desktop shortcut creation at `593.370 s`, D3D12 adapter initialization at `594.894 s`, and loading
7,081 texture replacement entries at `596.915 s`. Without another manual launch, it played `rockstar.mp4`
(`596.963 s`), `sdlogo.mp4` (`604.239 s`) and `mc3intro.mp4` (`612.301 s`) at 1920×1440/29.97 fps. These elapsed
times describe this disk/PC, not an installation-time guarantee for other hardware.

The installer wrote the **V2 installed marker** and a **7,188-record inventory**. Pinned manifest SHA-256:
`26595f26e83354cc18b5351b12811d1e9f0564855ae8114500213f37094e3bed`.
Installed inventory SHA-256:
`2648d2e8a8ab8a04845aefbb28ff60977d929f5f155793cec99a53db4198ffcf`.
The saved disc path is relative `game\game.iso`, and the desktop shortcut target and working directory were
independently checked against this external folder. Full installation evidence is in its `logs/mc3.log`; procedure and
detailed timestamps are in [portable setup](portable_setup.md).

The fresh-install UI check establishes automatic intro startup. Windows then displayed its own network-permission
dialog, which the agent left untouched. The user handles that Windows dialog; local installation and offline
gameplay do not require allowing public inbound network connections. The test does not promise zero OS/admin
prompts on every machine, and it does not claim a separate fresh-install menu/driving playthrough after the prompt.
The first-launch proof uses the full offline part extraction path. Fetching the pinned metadata/content from the
final public HTTPS URLs is a separate check after publication.

## Defaults, saves and vehicle scope

The runtime ZIP excludes personal settings, save data, logs, crash dumps and developer environments. Initial
setup used the compiled defaults: added ride/camber/offset zero, performance multipliers ×1, HD movies and texture
replacements enabled, 60 fps cap, vsync and desktop-sized borderless output. Updates must preserve the user's
`hdd`, `settings.toml` and personal `config` state; those are not publisher defaults to replace.

The separate [Golf R32 native validation](live_vehicle_tuning.md) confirmed that a ride-only F1 change updates
the current wheel caches immediately. Purchased `+0.275 m` plus PC front `+0.204 m` produced `0.479 m` on both
front wheels; PC rear `-0.005 m` produced `0.270 m` on both rears. These survived a distinct street-car creation,
along with tested camber, wheel offset and engine-power values. The **±2 m** slider range and settings round trip
are implemented/tested; this does not establish physical clearance at those extremes on every car.

## Remaining work

The additional texture/video processing pass and its full quality review are ongoing. Archive SHA verification
and the three observed movies establish authenticated installed bytes and sampled playback, not perfect visual
quality or complete decoding of every asset. All-car extreme width/camber, tire/road intersection, chassis
scrape/spark behavior and driving-feel combinations remain bounded playtest work; see [road scrape](road_scrape.md).
PBR conversion remains future work under the [PBR plan](../PBR_PLAN.md). None of these are marked complete by this
release validation.
