# Detailed video pass and safe continuation

Updated 2026-10-22, Codex video agent. Scope: `tools/video/*` only; no runtime,
original Bink, game save or published movie was replaced by this investigation.

## Current production job

The inherited Python worker PID 2268 is running
`tools/video/upscale_movies.py --model realesrgan-x4plus`, with its log at
`%TEMP%/movies_x4.log`. At this investigation's snapshot it had published
48 of 106 movies with x4plus markers and was processing `story/saponint.bik`
(1,600 of 2,136 frames reported). Marker inventory leaves 66,188 source frames,
including that partially processed movie. At the inherited estimate of 2.6 seconds
per frame, that is approximately 47.8 GPU hours, before game/GPU contention and
validation. This is an estimate, not a completion promise.

The already-running interpreter loaded the old script before the hardening below;
editing the file does not retrofit its running functions. It does not honor a
stop-file or OS lock. Do not launch a second movie worker while it is present.

## Model comparison

The existing second pass already changes the first pass's anime-video model to
the general-image `realesrgan-x4plus` model. The developers describe x4plus as a
general-image model and animevideov3 as an anime-video model in their
[model zoo](https://github.com/xinntao/Real-ESRGAN/blob/master/docs/model_zoo.md).
Their MSE-trained RealESRNet variant is explicitly described as producing
over-smooth effects, so it is a poor first response to the user's detail complaint.

A bounded comparison processed original `mc3intro.bik` frame index 1000 with
x4plus, UltraSharp and Remacri. All three inferred at 4x and were reduced to the
same 1920x1440 output size; a source Lanczos enlargement is included for context.
The comparison PNG, full frames, hashes and timings are in
`tools/video/quality_samples/`. The original Bink SHA256 is
`cb22217b41f94c35dbf006425ce7bc4c49b00d25625db1ff1d78a70cdb906fa9`.

On this frame, x4plus keeps headlight housing edges and grille structure visibly
sharper than UltraSharp or Remacri. The sample supports continuing the existing
x4plus pass, rather than discarding roughly half a completed pass to change models
again. It does not prove a ranking across every scene, temporal stability or that
every inferred detail existed in the original. Single-image networks process
frames independently; no motion model or frame interpolation is applied.

The comparison took about 11 seconds per model including one-frame process and
network startup while the inherited job was running. Those timings are not batch
throughput measurements. No further GPU benchmarks were started.

The [official NCNN documentation](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan)
describes its model selection, tile sizing and Vulkan implementation. Upscayl
lists [UltraSharp and Remacri among its models](https://github.com/upscayl/upscayl/blob/main/common/models-list.ts).
These sources establish what the models are; the local visual comparison informs
the choice for this movie.

## Hardened pipeline

`tools/video/upscale_movies.py` now:

- Starts exclusively from `templates/game/assets/video/*.bik`, preserving frame
  index, original rational FPS, and the game's original audio/timing path.
- Reads the Bink header's declared frame count and checks its dimensions and rate
  against ffprobe. Bink's frame count is exposed as stream duration rather than
  `nb_frames`; treating the latter as zero would produce a false completion/ETA.
  Header offsets follow [FFmpeg's Bink demuxer](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/bink.c).
- Verifies all source frames decoded and every requested inference frame returned
  at the selected network's expected size. Incorrect output is rejected before
  publication.
- Encodes to a unique sibling `.part.mp4`. Encoder processes and temporary PNGs
  are cleaned up after success/failure; failed work does not replace the old MP4.
- Decodes every completed candidate, checks exact frame count/rate, 3x dimensions,
  H.264 and yuv420p before the same-directory atomic replacement. Windows files
  held open by the game are retried at publication.
- Records source SHA256, source geometry/count/rate, model parameter/weight and
  executable SHA256, recipe version, encoder settings and completed output SHA256
  in `<movie>.upscale.json`. Resume checks recipe and complete output hash.
- Keeps `<movie>.model.txt` for older inventory tools. An inherited matching
  marker is accepted only after full decode/geometry/count/rate validation and is
  annotated `historical_source_and_weights_verified: false`. A historical text
  marker cannot prove which weight file or original source produced its MP4.
- Uses an OS-held output-directory lock and detects older Python workers through
  process inventory. A crashed process automatically releases its lock; a leftover
  lock file is not evidence that a process remains active.
- Writes optional atomic JSON coverage reports after each completed/skipped movie
  or audit item. Reports distinguish filtered coverage from all-source coverage.
- Supports a stop-file at movie boundaries. A stop request never publishes a
  half-encoded movie. Resume is at movie granularity, not within the current
  movie's unfinished PNG/encoder chunk.

The ongoing inherited job still uses its older publication rules until it exits.
Its already-published MP4s should be audited before backup/completion claims.

## Commands and coordinator contract

Use the Python with Pillow installed:

```powershell
$VideoPython = 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe'
```

Quick, read-only marker inventory (does not run a network or validate outputs):

```powershell
& $VideoPython tools/video/upscale_movies.py --model realesrgan-x4plus --status --report .local/video-status.json
```

Resume after the legacy worker has exited, with durable progress:

```powershell
& $VideoPython tools/video/upscale_movies.py --model realesrgan-x4plus --stop-file .local/stop-video-upscale --report .local/video-pass.json
```

Create the named stop-file to finish the current movie and exit. Remove it before
resuming. `--max-movies 1` provides a bounded single-new-movie run. No output or
model-marker files need to be removed to resume. Do not use `--force` for ordinary
continuation: it intentionally recomputes already-current validated movies.

After producers are stopped, validate the entire published pack:

```powershell
& $VideoPython tools/video/upscale_movies.py --model realesrgan-x4plus --audit-only --report .local/video-audit.json
```

`--audit-only` does not publish, adopt, or modify movie/sidecar files. Its explicitly
requested report is written as progress. A completed all-source gate requires
`mode: audit`, `movies: 106`, `covers_all_sources: true`,
`all_source_movies_validated: true`, `all_requested_model_validated: true`, and
`failed: 0`. A `--status` report always sets both validation booleans false.
A filtered `--only` audit can validate that subset, but its all-source boolean
stays false. The coordinator must use an unfiltered final audit before the full
project backup and must still check its own texture, backup and PBR gates.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | All selected originals have a validated requested-model output; for `--status`, inventory completed only. |
| 1 | A movie failed, originals did not match, worker conflict, or tool/I/O error. |
| 2 | Clean checkpoint/limit exit with pending movies, or a valid audit with some movies still on another/unrecorded model. |

Reports include requested model, source movie count, verification/failed totals,
remaining source frames and per-movie `movie`, `frames`, `status`,
`output_validated`, and `recorded_model`. Audit rows include decoded geometry,
frame rate/count and historical attribution. `status` is `pending`, `verified`,
`model-pending` or `failed`; error rows include `error`.

To hand off legacy PID 2268 safely, observe the completion of its current movie
(the final MP4 and matching model marker, not merely `1600/N` in the log). Verify
the process identity again, then terminate that old worker and its descendants at
the checkpoint before starting the next GPU phase. The old process immediately
starts decoding its next movie, so a small amount of next-movie temporary work may
be lost. Terminating only its Python parent can leave a GPU child running; verify
the entire known process tree has exited. Parent/coordinator owns this transition.

## Validation performed

13 tests pass under Python 3.12. The tests include actual ffmpeg encoding/decoding
of three deterministic CPU-generated frames across chunk boundaries, exact rational
FPS and frame count, GPU failure/missing-frame preservation, metadata/output hash
resume, damaged decode rejection, atomic publication failures, legacy worker
refusal, Bink header frame count, all-source gate semantics and stop-file exit.
No neural network is needed for the tests.

```powershell
& $VideoPython -m unittest discover -s tools/video -p 'test_*.py' -v
```

The actual published Rockstar logo was fully audited: 210 frames, 1920x1440,
2997/100 FPS, H.264 yuv420p, x4plus marker; this is only a one-movie validation.
An attempted bounded production invocation was correctly rejected with PID 2268
before any GPU work. No complete 106-movie audit has been claimed.
