# Verified immutable content bundle — 2026-10-26

The new local `0.3.3-dev.20261026` content bundle completed at **22:55:59 UTC /16:55:59 MDT**.
Its7,188 payload records match the sealed content snapshot exactly. Independent chunk/whole-archive
hashing and the pinned headless native archive reader passed. **Release readiness remains false**;
no release, upload, game session or installation was performed by this assembly.

The new output and separate control directories are:

- `G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/content-bundle-r1/`.
- `G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/content-bundle-control-r1/`.

They were created exclusively. No existing output, earlier release asset, canonical game/media file,
personal setting or save was replaced. Partial/error evidence would have remained unaccepted; this
run exited successfully with no failure receipt.

## Inputs and exact scope

The [snapshot](content_snapshot_20261026.md) manifest has SHA-256
`46505b3573e724391555413efd2e09b6574574a980d2208458baeed13672faf9`.
The separate full frozen-content audit completed at22:39:48UTC, SHA-256
`ad99118fa1b5070c01010087fd0ad2c66b4374c8114bcfb0f53516465be32939`, at
`validation-r1/frozen-validation.json` under the same external release-work root.

That audit fully decoded/frame-checked all106 frozen MP4 outputs and checked their exact original
Bink-header timing, with zero errors/warnings. It verified and loaded all7,081 RGBA PNGs, joining
7,079 original-pass and two reviewed replacements to their proof, then rehashed the recorded inputs
unchanged. It does not claim full original-Bink decoding, new visual-quality acceptance or authenticated
neural history for every movie.

Movie provenance remains **44 strict original-source records,5 historical marker-only and57 historical
unrecorded outputs**. Successful playback-data validation does not upgrade those historical records.
The sealed earlier39/67 plan and the reviewed texture proof graph remain preserved.

The archive contains only the **7,188 payload files**: one ISO,7,081 replacement PNGs and106 exact
source-derived MP4 names. All `content-files.tsv` path/size/SHA-256 rows were compared against
`snapshot-manifest.json.payload`; the snapshot's **7,391-file evidence inventory was not bundled**.
Original Binks, sidecars, reviewed proof metadata and unrelated/partial movies do not become runtime
payload entries. The complete reviewed proof package remains separately authenticated.

The builder received explicit frozen paths for the ISO, replacement PNGs, MP4s and
`evidence/source-video/` catalogue. It used no mutable live-media folder or implicit live catalogue.

## Archive authentication

| Artifact | SHA-256 |
| --- | --- |
| `content-manifest.tsv` | `1c8c72b70313bf673504275127067b5508510150b24a3f9ad256a872fafdb215` |
| `content-files.tsv` | `cbf8891624798d57b95bd54a73b47e5f6ea7f9f398137cc3d8d11e2e4f387dd8` |
| Concatenated archive | `b84957a318b73ff2249a63aa16e547648872d3170f957cc707bcb6926053f9ca` |
| `content-bundle-control-r1/acceptance.json` | `92c2fdde59450ff472820f383aa47cb4349530a542ac2d9a77158241cc378726` |

The archive is exactly **14,124,573,292 bytes in eight chunks**. The first seven are1,887,436,800 bytes
(1800MiB) each; the last is912,515,692 bytes. Full chunk names use the prefix
`mc3-content-0.3.3-dev.20261026` and suffixes `.001.mc3c` through `.008.mc3c`:

| Chunk | SHA-256 |
| --- | --- |
| 001 | `69ca22df936a33ef4f4c1e4c5b1b49034c8392c46b648ab70c52e004cd00b28d` |
| 002 | `663e964d0e3d56d58e5c8ead143539e6a4c6af785b44b13884a1f950b2fbafc1` |
| 003 | `228f486d9c8a906c224cffaf3f0a7e6647c7e03a81ef16b96a01887659382ae4` |
| 004 | `dadc1d0cb8e4224e5b7068b1ee89fcc9885d9435cc8702545538454178daa472` |
| 005 | `6f99543408f82481ceeef0732edf43b800dbaaadea21d3c5625f4e95fdf35c53` |
| 006 | `ae28b56c6012f073c992bb1c2fb719024bea80a5d96759524b61bb53c0950b6c` |
| 007 | `bb29d3f9f305ac04d378f45ac4ff34ad3b5b477f93f7220d7c464a2a3ca4a0f8` |
| 008 | `78400b9d6d68db2a865c390118b982b81469f9d61d28df855968727bee97c58d` |

`New-ContentBundle.ps1` and `PortableRelease.psm1` were pinned before/after assembly to
`be2e705b9b4d3cdf3073d80cc364b780070928fd2444837fba33e0bb1d157621` and
`746e6fd2de192bdc287d0472e1598c552cb8a29e3ae5083edb75a11d5b74feee` respectively. The streaming
builder exited0 at22:53:52UTC. The independent per-chunk, concatenated-archive and complete payload-row
checks passed before invoking the native reader.

## Headless native reader

The preserved read-only checker at
`G:/MC3_Backups/release-work/runtime-hotfix-0.3.2-dev.20261025-r1/control/headless/Check-ContentArchive.exe`
has SHA-256 `288dd8d0bc47f5b645519e41e566514a5d5062d54bbf7383315a60b5ead820c2`.
Its checker/reader/header hashes match its original source proof and exact
`08bcab9687e55db8245e149d39fd92db78944dff` source; those files show no Git diff from that revision.
No old build script or prior release evidence was overwritten.

The checker exited **0**, with empty stderr, and reported:

```text
Native archive reader PASS: 7188 files, 8 chunks, 14124573292 bytes
```

It authenticated actual chunk, whole-archive, entry/path and per-file bytes against the packaging
inventory. This console-only reader creates no setup window, downloads nothing, installs no files
and does not launch the game or initialize its renderer. It is not clean-install or gameplay QA.

## Durable evidence and next gates

The control directory preserves `preflight.json`, exact `build-command.json` and generated PowerShell
script, builder stdout/stderr/exit, `payload-comparison.json`, `archive-hashes.json`, native checker
command/stdout/stderr/exit, immutable phase receipts and the complete acceptance receipt. Its owned
one-shot script SHA-256 is `c6bbfbe9988a9735544494722077a4130f8e2ad363040a5d4b1f9cb419eda4f9`.

Root still owns the exact new runtime build/source proof, offline clean-install and native gameplay
checks, runtime ZIP pinned to this fixed-tag content manifest, and release/public-download gates.
Older release assets and the immutable snapshot/proof graph remain unchanged. Live strict-media
processing is separate and was not stopped or controlled by this build. All106 strict media,
final media audits and a verified full-project backup remain separate prerequisites for new offline
PBR generation; runtime PBR is unchanged.
