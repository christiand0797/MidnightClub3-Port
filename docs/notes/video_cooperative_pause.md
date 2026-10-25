# Cooperative pause inside a movie

The optional `tools/video/upscale_movies.py --pause-file <path>` protocol lets the existing movie child wait
through a game session without discarding its progress. The coordinator's ownership/scheduling side is
described in [media game pause](media_game_pause.md). The worker only reads marker presence; it never clears
a pause/stop marker, manipulates game input, or stops another process.

With no pause option, the existing 400-frame default and CLI movie-boundary stop behavior remain unchanged.
With a pause path, each NCNN invocation contains at most eight frames. The worker checks before source decode,
encoder creation, each inference invocation, each frame fed to the encoder, final validation/publication and
publication retries. Pause/stop checks use `lexists`, so even a dangling or malformed marker is a presence request.
An explicit stop is checked first on every wait iteration. A pause sleeps for one second between checks.

The already-running NCNN invocation finishes; this is not immediate GPU preemption or a wall-clock guarantee.
The bound is one current invocation of at most eight frames, plus coordinator detection/scheduling latency.
An already-started CPU decoder/encoder operation likewise reaches its next safe point. Smaller invocations
may reduce throughput because each starts NCNN again; no new GPU benchmark was run.

An ordinary pause retains the same Python worker, encoder/pipe and owned temporary decoded original frames.
After the coordinator removes only its unchanged owned pause marker, that same encoder receives every source
frame once in the original order. Explicit source frame rate/time base, rather than wall-clock arrival time,
determines the MP4 presentation timeline. The existing complete-stream timing validator remains in place.

An explicit stop, unavailable/dead owner or uncertain owner liveness raises `MovieStopped`. The owned encoder
is closed/stopped before its unique unpublished `.part.mp4` is removed, and only the owned temporary decode
directory is cleaned. Previously published MP4s, model markers and provenance stay unchanged. CLI status is
pending with exit code 2 and zero added failures. The in-flight movie starts again from the original Bink on
the next run; a game pause itself does not restart that movie.

## Owner liveness

The coordinator supplies `--pause-owner-pid <its actual parent PID>`. This requires `--pause-file`; standalone
pause use can omit an owner. On Windows, the worker checks that the supplied PID equals `os.getppid()` and opens
one pointer-width process handle with `SYNCHRONIZE` read-only access. At every cooperative safe point and wait
iteration, `WaitForSingleObject(handle, 0)` must return `WAIT_TIMEOUT`. A signaled handle, failed/uncertain wait
or unavailable owner stops cooperatively without clearing the private marker or resuming GPU work.

That held handle prevents a later PID reuse from changing which process is observed. The initial PID equality
checks do not prove against the rare case of a parent PID already reused **before** the initial handle open;
no parent/child creation-time attestation is claimed. The process-handle wait/access contract follows the
[Windows wait API](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
and [process access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/process-security-and-access-rights).
No kill, suspend, write-memory or input-control right is requested for the owner.

## Picture recipe and verification

Scheduling settings are outside `recipe_for`. Pipeline version 1, original source hashes, model/executable
identity, 4× inference followed by existing 3× Lanczos output, unsharp amount, encoder settings and exact source
timeline are unchanged. With the pause option, weights/executable identity is rehashed before every inference
invocation and before final publication, including retry waits. If a pause begins during that check, the identity
is checked again after resume. A source/model edit cannot silently receive the old pinned recipe.

Fourteen fake-process CPU tests cover pause before/during decode, one held encoder and all 19 frames in order
across 8/8/3 batches, explicit-stop priority/cleanup, final-batch and pre-publication stop, owner exit/uncertain
wait, pointer-width handle signatures, dangling marker presence, model changes during pause/hash/publication
retry, unchanged defaults and pending CLI reporting. The existing 19 validation/provenance tests also pass.
These fixtures start no GPU, real encoder or game and do not prove neural frame bit-equivalence. Independent
review reran the 14 pause cases and all 24 existing movie cases, including the existing CPU-only FFmpeg/fake
inference tests, without finding a remaining blocker.

A separate bounded real Windows parent/child probe observed the live original parent, normal parent exit 0,
then a signaled handle and child exit 2. It touched only fresh owned fixture processes, with no kill or existing
process interaction. Evidence is `.local/movie-pause-owner-review-20261025/owner-probe.json` (SHA-256
`b48e53106c92feef6224606366f841960fec82ff93d22eb5d6b1083886fdbd29`). This proves actual handle liveness;
marker retention/encoder cleanup are established by the separate worker fixtures, not that handle-only probe.

The read-only actual-record comparison is `.local/video-pause-recipe-compatibility-20261025-r1.json`: all **25**
strict original-derived records still match their current source headers, full source/output hashes and exact
picture recipe, with zero issues. No output or metadata was rewritten; no inference or new stream decode was
performed by this comparison. Existing completed movies are not forced or regenerated by the pause feature.
The compatibility record SHA-256 is `a6af840604faeeed3d4a38283201b1272538f0aaae6a3cb2fe9673ad384a1731`.
Frozen worker/test source hashes for this verification are respectively
`4693056a2615fe45a9ca008ffcae1862d565e3ecc8a1f4189be2d0838c16f4b8` and
`18676609422467d0c4089a8085d5315d494c7da2ee844267247da1b2bd79ae3e`.

Editing a script does not retrofit a previously loaded worker. Parent integration owns a safe paired
coordinator/worker restart and must preserve existing workers and user stop requests during rollout.
