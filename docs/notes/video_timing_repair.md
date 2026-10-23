# Exact Bink timing and recoverable movie candidates

On 2026-10-23 the x4plus worker completed inference and encoding, then rejected
23 reward movies with `frame rate differs: 999991/33333 vs 10000000/333333`.
The original Bink header uses the latter exact rational. With the old defaults,
FFmpeg selected MP4 time base `1/999991` and frame duration 33333 ticks. The
strict equality check correctly refused that different clock. The difference
was small, but relaxing the frame-rate comparison would not establish correct
frame ordering, duration or sync.

A CPU-only 540-frame reproducer showed that explicit
`-enc_time_base 333333:10000000` and `-video_track_timescale 10000000` produce
the exact source rate, 333333 ticks per frame, 540 decoded pictures, duration
179999820 ticks (17.999982 seconds), and zero presentation error at every frame.
The current 106 Bink headers contain 77 clips at `2997/100`, 28 at
`10000000/333333`, and one at `30/1`. The encoder now derives both clock options
from each source's reduced rational. Model, output size, Lanczos resize,
sharpening, H.264 CRF/preset and pixel format remain the same.

Validation still requires exact nominal and average frame rates, source frame
count, 3x geometry, H.264 and yuv420p. It now also decodes every presentation
timestamp and frame duration, checks the complete source frame grid starting
at zero, and checks exact stream duration. Gaps, duplicates, shifted starts,
missing timestamps and wrong final duration are rejected. The saved output
record includes the checked clock, step/count/duration and timestamp hash.

Existing recipes are not invalidated merely by this source-code edit. A recipe
that differs only by the two newly explicit clock fields can be retained after
complete decode and exact timing validation. Its original model/source history
and any `historical_source_and_weights_verified: false` attribution are retained.
The original encoder recipe is preserved; a separate compatibility record states
that its actual decoded timing satisfies the newly requested recipe. A stream
copy records its container transform separately instead of claiming a fresh
encode with options the original encoder never used.

Automatic lossless timing repair is narrower: it requires an original-Bink
record proving matching source, model/weight hashes, picture settings and
current output hash. Only the observed `999991/33333`, `1/999991` case is
eligible. Every old PTS/duration must already form a complete CFR grid, and
integer rescaling must map every value to the exact source grid. FFmpeg then
stream-copies H.264 without inference or re-encoding. A full new decode/timeline
check and identical encoded-packet hashes are required before atomic publication.
Text markers and unrecorded/adopted legacy attribution cannot authorize this
repair or be promoted to verified historical inference.

Completed encodes that fail later validation or publication are retained with
their recipe, checksum and failure under `.local/video-upscale-candidates`.
They remain unvalidated and are never automatically adopted. This quarantine
is outside `video_hd`, so failed candidates are not part of active playback or
a packaged movie inventory. Incomplete inference/encode failures still clean
their partial output. A previously published picture remains in place until a
new candidate passes all checks.

The old worker deleted its failed candidates and inferred-frame temporary
directories. Those earlier 22 encodes cannot be recovered from their older
published replacements. The final RZonda candidate was captured through a
read-only shared-delete handle after its worker exited. A separate lossless
retimed copy passed the real 540-frame validation, including exact 10 MHz timing
and unchanged encoded H.264 packets. Its capture remains unrecorded and was not
adopted or published. Diagnosis artifacts and provenance limitations are in
`G:/MC3_Backups/release-work/video-rate-diagnosis-20261023`, including
`synthetic-rate-evidence.json` and `rzonda-retime-evidence.json`.

Validation used Python 3.12 and the installed FFmpeg 9.0.2:

```powershell
py -3.12 -m unittest discover -s tools/video -p test_upscale_movies.py -v
py -3.12 -m unittest discover -s tools/maintenance -p test_overnight_pipeline.py -v
```

All 18 video tests and 21 coordinator/gate tests pass. The video integration
test reproduces the old rational failure, repairs a proven fixture without
changing its encoded packets, and exercises the real encoder across a
400-frame chunk boundary. Inference is replaced with tiny deterministic images
for these tests; no GPU worker runs. The 106-movie audit, verified full backup
and PBR gate remain required. Worker restart is a separate authorized handoff
after the running game closes.
