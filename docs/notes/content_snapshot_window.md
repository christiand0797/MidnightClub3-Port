# A stopped-worker window is required for the content snapshot

The real corrected-content snapshot completed on 2026-10-26 at 19:01:20 UTC / 13:01:20 MDT after an
authenticated SqMuscle publication and graceful worker exit. It froze 7,188 payload files and
7,391 total inventory files, including 44 strict movie records, 5 marker-only and 57 unrecorded movies.
An automatic cross-volume stop-retirement failure and subsequent usage outage delayed the reviewed
recovery until 16:26 MDT. One coordinator and movie child then restarted. Exact hashes, failure/recovery
receipts and pending validation scope are in [the completed snapshot note](content_snapshot_20261026.md).

## Actual Windows finding

Delete-sharing reads alone did not make concurrent snapshotting safe on this host. The tiny
real-API fixtures opened a target with read/write/delete sharing, yet the running worker's actual
`os.replace`/`MoveFileExW(REPLACE_EXISTING)` operation returned **WinError5 while that destination
was held open**. Replacement succeeded after close. Renaming the held object to a new unused name
also succeeded. Raw Win32 handles and CRT-converted handles, narrow/read access modes, C: and G:
were isolated; CRT conversion and the read-only attribute were not the cause.

The [CreateFile sharing contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
allows later delete-access requests, including rename operations. That alone does not establish
successful replacement of an existing open destination through every publisher API. The observed
publisher operation, rather than an inferred sharing guarantee, controls this tool's readiness.
No real movie worker publication was disrupted during these synthetic tests.

## Explicit quiescent execution

`freeze_content.py` defaults to a read-only plan. Production execution requires a fresh external
G: output and an explicit mode. Concurrent mode first probes the actual held-target publisher
operation and refuses before copying when unsupported. It must not be bypassed here.

Quiescent mode requires the exact SHA-256 of an existing owned coordinator-stop request. It never
creates, clears or retires that request. After actual coordinator/media exit it holds these exact
byte locks throughout the operation, in order:

1. Coordinator lock.
2. Reviewed-texture installation lock.
3. Video output `.upscale.lock`, also used by manually launched normal movie workers.

It refuses active texture transactions, user texture/video stop requests and real live game/media/
coordinator workers. Checks continue during streamed reads, at movie boundaries and immediately
before final sealing. Root must retire only its exact unchanged maintenance request afterward and
restart one coordinator when validated scheduling permits; the snapshot tool controls no process.

The payload is exactly **7,188 entries**: one original ISO,7,081 active PNGs and106 original-source-
derived MP4 names. Independent original Binks are copied separately for the frozen source catalogue;
sidecar bytes or their explicit absence are preserved as evidence. No partial/unrelated movie,
personal settings, saves or runtime executable is selected.

Copies use pinned file identities, two source reads, independent destination hashes, stable sidecar
cohorts and bounded retries. New outputs are exclusive independent files. Failed attempts/directories
remain unaccepted evidence and cannot be silently resumed. Completion is sealed only after the exact
final payload inventory and all hashes pass. It establishes frozen-byte integrity and recorded movie
provenance, **not a new decode/timing, complete texture-quality or full-project backup audit**.

## Validation and next window

Root independently passed all25 tiny Windows fixtures, including actual unsupported concurrent
replacement, mutation/cohort failures, strict versus historical provenance, stop/transaction and
worker gates, all three lock conflicts, and lock release on success/failure. A read-only production
plan selected the expected7,188 entries and wrote no content snapshot.

Do not rewrite the sealed39/67 release plan when progress advances. Compute and seal the actual
strict-record versus historical cohort from the frozen files. Model-marker-only or unrecorded
historical movies are not authenticated x4plus outputs. The full106 strict-movie completion and
verified post-media full-project backup remain separate pre-PBR gates.

That window has now completed; do not repeat the copy or modify its sealed cohort to track newer
live movies. Finish the separately running frozen-content decode/timing audit, then verify content
chunks/native archive reading, clean-install behavior and exact release source/EXE evidence before
publication. The live coordinator continues the remaining strict movie pass. Future maintenance
still requires an authenticated publication boundary and all three locks; retire an owned request
on the same volume, preserve independent external evidence, and use the explicit validated Python
interpreter. Do not hold an idle game window or discard a current movie to force another window.
