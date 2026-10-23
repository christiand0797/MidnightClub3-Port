# Texture and video audit — 2026-10-21

Audit covers the canonical `tools/texture_pack/` and `tools/video/` pipeline, runtime texture/movie binding,
all root Python scripts (AST/inventory, full reads of the local extraction/repacking pipelines), archive
directories and representative unhandled file records, generated PNG integrity, and completed movie metadata.
No game launch, build, source asset rewrite, download, or upscaler start/stop was performed.

## Current output and inherited jobs

Initial output inventory at approximately 02:49 America/Denver, with the texture completion check below:

- `textures/source`: 4,776 PNGs; `textures/dump`: 741; `textures/replace`: 4,774. The texture job subsequently
  finished and PID 7252 exited. The union of source/dump is 4,792 hashes; its 18 missing replacements are exactly
  intended skips (16 flat images, two 8x8 tiny textures). There are no unexpected missing eligible replacements.
- All source/dump PNGs and, after job completion, all 4,774 replacements passed Pillow `Image.verify()`.
- `source_index.json` contains 1,879 DXT1 and 2,897 DXT5 unique hashes. It does not demonstrate coverage of other
  formats.
- 106 original Bink movies exist; 12 HD MP4s were complete at the later snapshot. MP4 count changes as the job runs.
  The movie log had begun `reward/rme412.bik`.
- Completed MP4s existing during ffprobe validation had 3x dimensions and matching original durations.
  `rockstar.mp4` is 1920x1440 / 210 frames; `sdlogo.mp4` 1920x1440 / 240 frames. Reward clips checked were
  2160x1440 / 540 frames. This validates the container/content metadata, not native Media Foundation rendering.
- Both inherited pipelines were active when discovered: Python PID 7252 (`tools/texture_pack/upscale.py`) and
  Python PID 4312 (`tools/video/upscale_movies.py`), each with its own Real-ESRGAN child. Leave the existing jobs
  running; do not start duplicates. The texture job is now complete; the movie job remains active. Movie stdout
  is `%TEMP%/movies.log`.

## Fix integrated by this audit

Parent authorized only `runtime/gpu/d3d12.cpp` and `runtime/gpu/video_hd.cpp` edits.

1. Texture pack selection now participates in the cached-texture invalidation condition. Previously an unchanged
   guest texture stayed HD after disabling the setting, or stayed original after enabling it. `Tex::pack_enabled`
   remembers the selection used for cached pixels; selection changes refresh once, restoring the original guest
   texture or selecting its replacement. Setting changes do not increment the dynamic guest-upload count.
2. Movie `Playing` and `Picture` now honor the current HD movie setting. An HD-capable movie opened while disabled
   keeps its path/header registration; `Frame` starts its decoder lazily if enabled later. Existing Bink picture,
   sound, and timing remain active. Once started, the HD decoder remains warm until the clip closes, which permits
   immediate disable/re-enable without creating another decoder.

`git diff --check` passed for both source files. Parent reports the integration build passed. Added
`tests/video_hd_test.cpp` for headless component integration of the actual Media Foundation decoder with the
completed `rockstar.mp4`, checking midclip first enable, 1920x1440/opaque/nonempty RGB, serial advance, disable,
re-enable, close, and missing-file fallback. Parent owns its CMake integration and execution; the game itself
was not launched. Remaining native texture verification: unchanged texture on/off/on repeatedly, confirming
original dimensions/mips and stable guest-upload counts.

## Coverage gaps and concrete format evidence

The canonical extractor intentionally scans only `.xbck`; it rejects linear texture headers with nonzero Size
(`xbtex.py:92`) and leaves P8 unnamed (`xbtex.py:108`). No new P8/linear/.tex/.xbpf integration from the old claimed
background agents is present. The base archive contains 4,040 `.tex` entries, archive `_1` another 376. Nine loose
`.xbpf` page pools exist. The canonical source function does not visit either type or the two loose XPRs.

Representative `.tex` data: a 64x64 record is 5,134 bytes, begins with a 14-byte dimensions/format header,
then 1,024 bytes of apparent BGRA palette and 4,096 indices. This is strong evidence for a paletted cutscene
texture, but the exact runtime mip/palette hash still needs loader tracing or matching live dumps before naming
offline replacements. Do not simply hash its file bytes as a D3D texture.

`extract_xbpf.py` at the repository root is not a working reader for these files: `parse_xbpf_toc` assumes four
u32 header fields after magic and seeks to a supposed TOC offset. Actual read-only observations across all nine:

```
0x00  'pf05'
0x04  u32 page count
0x08  u32 fixed page slot size
0x0C  packed u32 table[page count]
page offset = (table[n] & 0xFFFFF) * 2048
page stored extent = (table[n] >> 20) * 2048
```

The first page begins at 0x10000. Every actual file size equals `0x10000 + count * slot_size`. For brake, decal,
decal_g, shared_tex and tire, packed page stored extents exactly match their `.xbpmap` row extents rounded to
2048 bytes. Some model/animation pools have a few larger map extents, which should be investigated rather than
silently truncated.

Example rim `mip0` at file 0x10000: 4,352 bytes, resource header at +0x20, DXT5 64x64, 4,096 pixel bytes apparently
at +0x100. Tire `mip0` is similarly 65,792 = 0x100 header + 65,536 DXT5 bytes for 256x256. Lower detail `_b1/_b2`
are separate mip0 records, not one mip chain. This is a promising direct route to rim/tire replacement hashes,
after confirming which portions the title uploads.

`decal/decal_g` `gfxImage` records contain palettes/indices and pointers, not raw guessed DXT. The root extractor
guesses DXT format by payload length and manufactures a DDS header; do not use those guessed outputs as verified
sources. Map type counts: decal 4,257 gfxImage + 236 mip0; decal_g 1,512 gfxImage + 236 mip0; rim 881 mip0;
tire 294 mip0; rider 70 mip0; shared_tex 33 mip0; brake 40 mip0 plus a few lower mip records.

## Remaining actionable issues

- **CPU texture cache is unbounded.** `texture_pack.cpp:37,224` retains all decoded images and generated mips.
  Current replacement corpus would consume approximately 7,157 MiB as BGRA+mips if every image is requested;
  GPU resources add their own memory. A bounded cache or release-after-upload design is needed for long sessions
  traversing many areas. The audit did not change ownership/lifetime of `Find` results.
- **Texture writes are not atomic.** `upscale.py:103` saves directly into `replace/<hash>.png`; interruption can
  leave a truncated file which is then skipped merely because it exists. Runtime indexing also caches failed
  loads for the session. Save a same-directory temporary PNG, verify it, then replace the final file; validate
  existing files before treating them as complete. Current PNGs checked were sound.
- **Movie content remains incomplete.** `.part.mp4` publication is atomic, but the script skips any existing final
  `.mp4` without probing it and has no frame-count validation before publication. An exception during ESRGAN/frame
  writing also lacks `finally` cleanup of the encoder subprocess. There is no evidence of current corrupt finals;
  hardening these paths avoids future false completion/orphan encoders.
- **Decoder robustness remains unverified.** `video_hd.cpp` assumes MF creation/type calls succeed before
  dereferencing some COM results; early returns omit `CoUninitialize`. Stride orientation, type-change flags and
  source-buffer row extent handling need a standalone asymmetric-image decode test. An invalid row extent currently
  breaks the copy loop but publishes the partly copied frame. Avoid calling native playback verified until tested.
- Movie data-address reuse can replace `g_by_data[data]` without stopping a prior decoder associated with another
  header; cache serials also restart per clip. Usually the title closes the prior player, but this should be covered
  by lifecycle tests before treating generic video replacement as complete.
- Dump/replace indexes are read once at process startup. New files created by the inherited upscaler need a
  relaunch to be discovered; toggling HD pack does not rescan the filesystem.

## Root exploratory scripts

Use `tools/texture_pack/{dave,xbtex,extract_all,upscale}.py` and `tools/video/upscale_movies.py` as the current
pipeline. Root `check_*`, `search_*`, `get_xemu_swizzle.py` are mostly one-off network research scripts; do not
rerun them as tests. `test_dds*.py` manufacture exploratory DDS files, not a regression suite. `test_extract.py`
depends on an external `xpz_core`. `fix_ds.py` rewrites a hardcoded `C:/XboxXBERecomp/runtime/xdk/dsound.cpp` outside
this checkout and must not be reused here.

Root `upscale_textures.py` still has no-op XPR/XBCK extraction methods. `extract_xbck.py` assumes XBCK is an embedded
XPR container and its final extractor only prints texture metadata; the canonical baked-memory-image reader
supersedes it. `pipeline.py`, `complete_pipeline.py`, and `upscale_all.py` work on the old `extracted/upscaled/repacked`
directories and two loose XPRs, not runtime hash replacements. `repack_xpr.py` scales PNGs back to original texture
dimensions, retains old format/mip metadata while writing only top mip, and omits general resource preservation.
`extract_xpr.py` has incorrect P8/linear mappings and an undefined `index` in its XPR1 final summary. Do not copy
their repacked assets over originals. Those files are useful historical experiments, not a validated whole-disc HD
pipeline.

## Useful validation without a game launch

- Pillow verify of every source/replacement image and dimension/alpha assertions after atomic publication.
- Compare offline texture hash + decoded pixels with a captured runtime dump, including P8 palette-size variants,
  linear stride padding, separate XBPF mip0 records, and `.tex` loader conversion.
- ffprobe frame count/rate/dimensions for every completed MP4 against original Bink, then decode first/last frames.
- Standalone Media Foundation decode with colored top/bottom rows to verify orientation and stride; malformed
  input tests to verify Bink fallback and clean thread teardown.
- Native cache selection test with a static guest texture and repeated on/off/on transitions; ensure original
  resource dimensions/mip count return and guest-upload counters do not grow from setting changes.

## Integrated follow-up (supersedes the initial CPU-cache/decoder findings)

Caller-owned decoded replacement images now release after upload; the persistent pixel cache was removed. The native decoder checks COM/MF and sample/layout results, cleans COM on early returns, stops retired same-data movies and uses globally unique frame tokens. Parent built the native executable and ran both actual WIC ownership and Media Foundation lifecycle/loop/toggle tests successfully. All 8 C++ and 8 disc checks passed. This does not settle GPU resource budgets, all malformed media/orientation cases, extraction coverage or gameplay. The earlier findings above preserve the original audit snapshot.
