# Overnight media, backup and offline PBR coordinator

Prepared 2026-10-22. **Implementation and fixture tests are complete; no production GPU job, backup or new PBR
generation was started by the texture/maintenance worker.** Parent owns review and launch. Once running, the
coordinator replaces this document with continuous phase/checkpoint details (at least once per minute for a child).

Read-only inventory:

```powershell
& 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe' tools/maintenance/overnight_pipeline.py --dry-run
```

Explicitly execute the authorized workflow:

```powershell
& 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe' tools/maintenance/overnight_pipeline.py --run
```

The default, including a command with no flags, only inventories. The actual-project dry run found 37,611 files,
28,285,791,754 bytes, 106 original Bink movies, 12,714 original texture PNGs and 7,081 current active replacements.
These counts were a point-in-time inventory while other project work continued. No links were excluded in that
inventory. The source project remains `C:\Users\purpl\Desktop\MidnightClub3-Port`; default backup root is
`G:\MC3_Backups` (over 500 GiB available when checked).

## Order and gates

1. Obtain an operating-system exclusive coordinator lock. A second launch fails before starting work. Wait for
   **all** `mc3.exe`, Real-ESRGAN workers and texture/video Python controllers to exit. This includes legacy PID2268;
   the coordinator never terminates it. Parent must stop the old worker and its children at a verified movie
   boundary. The old code does not understand the new stop flag. Starting the coordinator early is safe: it waits.
   The authorized `--run` main thread holds `SetThreadExecutionState(ES_CONTINUOUS|ES_SYSTEM_REQUIRED)` while it
   works/waits, then restores `ES_CONTINUOUS` in a `finally` block. It requests no display wakefulness and changes
   no system power configuration. Process exit/crash releases the thread request. Acquire/release appear in
   live state and `power.log`; dry-run never requests wakefulness.
2. Fresh original-derived UltraSharp texture pass: `--pass-id 2026-10-22-originals-pass --batch 16`. Never upscale
   existing replacements as inputs. Resume uses original/output/model/pipeline checksums. Vinyl masks stay excluded.
3. Hardened x4plus movie pass, sequentially after textures, resuming already validated model outputs. Original Bink
   video supplies input frames; existing good MP4s stay published until a new full movie validates. Bink audio and
   runtime playback remain unchanged. Video agent estimates roughly 48 hours of remaining inference at the earlier
   measured rate; this is not an overnight completion promise.
4. Fresh texture audit verifies all eligible outputs and the named pass's SHA-256 provenance. Full video audit
   decodes all 106 movies and verifies expected dimensions, exact frame rate/count and requested model. The gate
   requires `mode=audit`, `covers_all_sources`, `all_source_movies_validated`, `all_requested_model_validated`,
   `failed=0` and 106 unique, verified, output-validated x4plus rows. Marker inventories and filtered audits cannot
   pass. If media changes during the audits, no gate is issued.
5. Full project backup to a unique `G:\MC3_Backups\MidnightClub3-Port_PrePBR_<timestamp>` directory, using robocopy
   `/E /COPY:DAT /DCOPY:DAT /XJ /R:1 /W:1 /MT:8`. No mirroring, moving or deletion. Existing destination copies are
   preserved. Check available space before copying.
6. Compare every included relative filename and length, then SHA-256 **every source file and its copied file**.
   Reject same-length corruption, extra/missing files and any changing source snapshot. Save resumable per-file
   identities/hashes in external `G:\MC3_Backups\verification\<backup-name>.manifest.json`; unchanged source and
   backup files reuse their verified hashes on a resumed verification. This avoids repeatedly reading 28+ GiB.
7. Publish an external `.verified.json` marker only after all files match and the media gate is still unchanged.
   Before PBR, check marker, manifest checksum, every copied file's metadata and the complete source snapshot.
   A changed project gets a new backup before any new PBR attempt. Never reuse an incomplete backup as a gate.
8. Only then invoke the existing **offline** `tools/pbr/generate.py` on the curated starter set, CPU ensemble,
   original source images, 1024 maximum, four threads, fresh regeneration. Existing PBR outputs are preserved in the
   verified backup first. No native renderer, PBR setting, shader or runtime material binding changes here. A
   completed starter run records its manifest hash, so rerunning an already completed coordinator is idempotent.

## Durable state, stopping and resume

State/lock/child logs: `.local/overnight_pipeline/live/`. Static final media audits:
`.local/overnight_pipeline/texture-audit.json` and `video-audit.json`. Video in-progress report:
`.local/overnight_pipeline/video-progress.json`. State and generated documentation update atomically.

Create `.local/stop-overnight-pipeline` to request a graceful stop. The coordinator signals `.local/stop-texture-upscale`
or `.local/stop-video-upscale`; their child finishes its current batch/movie first and checkpoints its outputs.
Copying/hashing/auditing finishes its current command before honoring the coordinator flag. Remove the coordinator
flag and whichever child stop flag was set, then run the same command to resume. Existing stop flags are respected,
never silently cleared. Exit0 means complete; exit2 paused; exceptions/failing child codes leave a failed checkpoint.

A saved, complete media gate is reused only if the metadata inventory of all source/replacement/video/model inputs,
pipeline code and audit report hashes is unchanged. Otherwise the real child tools validate/resume again. This saves
unnecessary repeat full-video decode/hashing while refusing stale gates. No detached helper kills another process,
creates a new scheduled task or sends messages to anyone.

## Explicit full-backup exclusions

- `.local/overnight_pipeline/live/`: changing state, process lock and logs.
- This generated `docs/notes/overnight_pipeline.md` and its atomic publication temporaries.
- Directory junctions/symlinks and file symlinks, recorded individually without dereferencing; the real-project
  inventory found none. `/XJ` prevents loops. Every exclusion is stored in the verification manifest.

All other files, including the original ISO, extracted assets, native outputs, `.git`, untracked source, current
replacements, quarantined masks, prior PBR outputs and static audit reports, are included. No broad cache or build
directory exclusions are used.

## Validation

**Ten maintenance fixture tests pass**: default/dry-run has no writes; required robocopy flags and overlap rejection;
full SHA verification plus cached resume; equal-length corruption; extra/missing files; source changes while hashing;
strict complete-media gates; manifest/copied-file/source-snapshot checks; operating-system lock contention; and
proof that no PBR child starts with an unverified marker. Ten texture publication/resume tests also pass, including
mocked original-input inference, categorical-mask exclusion, changed-input regeneration and batch-boundary stopping.
No GPU, actual robocopy or PBR subprocess is executed by these tests.
The keep-awake lifecycle test injects a fake Windows API and checks restoration after a work exception and refusal
to start work if the wake request fails; it does not change the machine's power state.
