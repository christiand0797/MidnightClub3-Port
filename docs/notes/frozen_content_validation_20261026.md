# Frozen corrected-content validation — 2026-10-26

The actual immutable cohort passed the CPU audit at **22:39:48 UTC / 16:39:48 MDT** with zero
errors and zero timing warnings. This validates the frozen files; native archive, clean installation
and source-exact runtime checks remain separate release gates.

The report is
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/validation-r1/frozen-validation.json`,
SHA-256 `ad99118fa1b5070c01010087fd0ad2c66b4374c8114bcfb0f53516465be32939`.
It binds snapshot manifest `46505b3573e724391555413efd2e09b6574574a980d2208458baeed13672faf9`.
The [snapshot note](content_snapshot_20261026.md) records the original freeze and recovery.

## Actual checks

- All 7,391 declared inventory files were rehashed before and after validation. The playable
  payload remains exactly 7,188 files. No unlisted input, link or mutation was accepted.
- All 7,081 PNGs passed chunk verification and a fresh pixel decode. Every image is RGBA and
  within the original pass's 2,048-pixel bound. All hashes join the authenticated proof ZIP:
  7,079 named-pass outputs plus the two exact reviewed San Diego corrections and their old archives.
  The frozen ledger, audit and model-identity JSON agree with their manifest/proof anchors.
- All 106 H.264/yuv420p HD outputs were fully software-decoded using two decoder threads.
  Geometry and frame counts match the frozen original Bink headers. All **105,272 decoded frames**,
  presentation timestamps, durations, average rates and total stream durations match the exact
  original frame grid. There were no decoder errors or historical timing exceptions.
- Original Binks were independently hashed and their headers checked. This audit does not claim
  a complete additional decode of every original Bink or a new whole-pack visual/neural review.
- Sidecar bytes/absence, strict recipes and any transitive identical-source aliases were checked
  without adoption, retiming or inference. Source, decoder and all recorded inputs were rehashed
  before completion; `inputs_modified` is false only after that successful final verification.

The recorded provenance remains **44 strict original/model movies, 5 marker-only and 57 unrecorded
historical movies**. Passing decoding/timing does not upgrade those 62 historical files to verified
new-pass x4plus outputs. The report correctly sets `all_outputs_have_strict_recorded_provenance`
and `release_ready` to false. The final all-106-strict pass remains unfinished.

## Reproducible scope

The validator and tests are public at source commit `c68d9641275b40bc2615ed506ef06933ea5841b8`.
Root and the independent reviewer each passed all 23 focused CPU fixtures before the real run.

| Input | SHA-256 |
| --- | --- |
| Validator | `60f6dc557d6f6c3ed58784c8db36b7dfd71f70539b50da8f004065e49cc019fd` |
| Tests | `a7a74797f56a9d7d6462c0871e2dde361d1adf9a871ed888a09f0ac87d035b2a` |
| FFprobe | `02264dfa4f56baaa31e443ece8aa16d397f6b008200e7fd184d22b9009e75fd3` |
| Authenticated proof ZIP | `21105deacc97ad2eb49b8d165c43c20d3b3a689df053a9c9a6591361bbf919bc` |

Execution used explicit Python 3.12, began at 22:26:31 UTC and finished at 22:39:48 UTC. Its new
external report directory contains an append-only progress log. The old snapshot/proof/plan and
canonical media were unchanged. The active media coordinator continued its single GPU worker.

Next, package only `manifest.payload` through the tested bundle builder with the frozen
`evidence/source-video` allowlist; cross-check every TSV entry against that payload and verify the
result through the native archive reader. An isolated clean runtime build from a sealed source
commit is required to close r11's inherited-object provenance gap, followed by bounded native and
clean offline first-launch tests. No release/upload, verified full-project backup or runtime PBR
change is implied by this audit.
