# A stopped-worker window is required for the content snapshot

The tested snapshot tool is ready, but no real full-content snapshot has run yet. The movie
coordinator and its single child continue their current movie. Preserve that in-flight work and
schedule a later snapshot after an authenticated movie publication and graceful maintenance stop.

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

Next: at a suitable published-movie boundary, create a new exclusive owned maintenance request,
let the existing pair exit normally, run the quiescent freeze under all three locks, verify its
manifest, retire only the unchanged owned request and resume one current coordinator. Then perform
frozen-content decode/timing, native archive and clean-install validation before publishing a new
release. Do not hold an idle game window or discard the current movie to force this window.
