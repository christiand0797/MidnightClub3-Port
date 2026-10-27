# R12 offline package and private test preparation

The R12 runtime ZIP is assembled and independently staged with all eight corrected-content parts.
No R12 game has been launched. Native installation/play checks are deferred while the user's
Black Ops III process is active; the coordinator has retained its existing movie worker/encoder
and acknowledged its own cooperative pause.

## Pinned package and archive reader

`runtime-package-r1/MidnightClub3-Native-0.3.3-dev.20261026-win64.zip` under the corrected-content
release-work directory is 9,718,074 bytes, SHA256
`300ea5f07f6f5e3a9bfff126047b5b35cc51019d73e9d8e9c2a8fc2348728ade`.
It includes the exact fresh EXE `a08f330c...`, the sealed explicit VC145 CRT, player instructions,
and a content descriptor pinned to manifest `1c8c72b7...` at the fixed future
`v0.3.3-dev.20261026` release URL. Its source revision remains exact build commit `c68d964...`.
This package has not been uploaded or published.

The newly compiled R12 archive checker `9f80fb07...` also passed the actual 14,124,573,292-byte
cohort: 7,188 files, eight chunks, exit0 and empty stderr. It verified chunk, whole-archive and
member hashes using the actual native reader. This was a console-only check, with no game/GPU,
input or media-control action. Exact evidence is `r12-native-archive-r1/result.json`, SHA256
`39b6c4448c46c017fe3befee02a015103ea0d9bc23c7f5e8cb334185ccc2935d`.
The [build seal](runtime_clean_r12_20261026.md) and
[bundle acceptance](content_bundle_20261026.md) remain separate immutable evidence.

## Independent private staging

The new test directory is `G:/MC3_QA_r12_offline_r1`. The pinned ZIP's 19 flat safe entries were
extracted exclusively and its 18 declared runtime files rehashed. All eight content parts were
independently copied, verified while copying and rehashed afterward; they are not hard links to
the release assets. The actual native installer may make temporary cache links to these private
copies when eventually run.

At completion, there is no installed game, replacement texture tree, HD movie tree, HDD8, settings,
content cache, installed marker/inventory or saved disc path. Only
`config/desktop-shortcut.txt` is deliberately preseeded with the exact 32 UTF-8 bytes
`G:\MC3_QA_r12_offline_r1\mc3.exe`, without BOM/newline. The existing real desktop shortcut is
required and pinned. This makes the runtime skip rewriting the real shortcut during private QA;
it must be described as an explicit exception to otherwise fresh content installation.

All 53 protected canonical/previous-private settings, saves and real-shortcut hashes matched
before/after staging. Distinct old default/HDD6/HDD7 saves stay distinct. Actual user files were
neither copied into the release nor restored/overwritten. Exact staging receipt:
`r12-offline-qa-control-r1/staging-complete.json`, SHA256
`4be9b8c6c527d944c5c9ab62438023a6dc213811f5ab2b38614eb0a0905a6cd7`.

## Reviewed launch protocol; not executed

The local helper `.local/launch_r12_offline_qa_r1.py`, SHA256
`2ba811b862e4df4bf5f818f7b25a8f973fc0d69b0e890d9ae33f5b5200e5e903`, passed independent static
review. It rechecks actual coordinator/video identities, source hashes, protected files, private
shortcut bytes, clean install state and human-game exclusions. It clears inherited process-only
input/capture diagnostics and launches only the pinned EXE with `--instance 8`, without `--disc`
or `--choose-disc` shortcuts that would bypass installation.

Before GPU/input initialization, a fresh 120-second startup gate waits for a new release file.
Release requires the same coordinator/video, a new-offset cooperative pause acknowledgment,
the waiting game log, the coordinator-owned pause and no active NCNN. Token publication uses
an exclusive prepared file, exact 32 bytes, flush/fsync/close and same-directory Windows
non-replacing atomic rename. The helper never writes shared stop/pause flags, filters physical
input or terminates any process. Failed unreleased gates time out themselves.

Static review does not authorize launch against an active human game. Wait for human games to
close and the coordinator to resume normally; discover fresh identities again. Do not clear the
existing pause or rerun old fixed-PID helpers. If legitimate user files change after staging,
preserve them and record a newly reviewed baseline without overwriting earlier evidence.

The remaining gates are actual offline extraction/installed-inventory parity, correct private
HDD/ISO mounting, new-hash HD startup/input/F1 behavior, normal owned-window close and same-worker
media resume. Final release readiness remains false until those checks pass. Runtime PBR and
the final all-106-strict-media/full-project-backup prerequisites are unchanged.
