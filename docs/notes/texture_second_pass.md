# Texture refresh and material taxonomy audit

## Fresh pass completed — 2026-10-23

The original-derived `2026-10-22-originals-pass` finished all **7,081 eligible textures**. A new CPU audit using
`--model ultrasharp-4x --pass-id 2026-10-22-originals-pass` verified every source/output SHA-256 against the
named-pass records, exact expected dimensions and PNG integrity. All issue collections are empty: no damaged
originals, missing/invalid outputs, active vinyl masks, orphan replacements, model/context mismatches or changed
completion records. The 5,603 masks, 23 flat images and 9 tiny images retain their intentional exclusions.

Local report: `.local/texture-pass-20261023-final-audit.json`, SHA-256
`134b5c1fd407dc731f85a1dc480a349e581af378839d4231784790cab19213ba`.
This proves the fresh pass completed; the published v0.3.0 content bundle was frozen before this pass finished.
Movies and the full post-media backup remain separate gates before new PBR generation.

The following sections preserve the initial audit and implementation details.

Updated 2026-10-22 by the texture worker. Scope: `tools/texture_pack/` and this note. No native renderer, video,
original ISO, extracted source image, or PBR output was changed. Parent owns GPU scheduling and the full-project
backup gate. The prior video x4plus GPU process was still running at the initial inspection.

## Current coverage, verified on the CPU

Command (no GPU/model process):

```powershell
& 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe' tools/texture_pack/audit.py --output .local/texture-pass-audit-2026-10-22.json
```

Result: complete for every eligible original known to the pipeline.

| Item | Count |
|---|---:|
| Unique source/dump names | 12,716 |
| Original source PNGs | 12,714 |
| Palette-tinted vinyl masks excluded | 5,603 |
| Flat images intentionally skipped | 23 |
| Tiny images intentionally skipped | 9 |
| Eligible art / valid UltraSharp replacements | 7,081 / 7,081 |
| Missing, invalid, orphan replacements | 0 |
| Excluded masks still active in replace/ | 0 |

Validation decodes original images, applies the exact same eligibility rule as production, checks replacement PNG
CRCs and expected dimensions, and checks the selected model name in `upscale_state.json`. It does not prove the
historical weight bytes used by the older pass: that state only records the model name. Installed UltraSharp weights:

| File | Bytes | SHA-256 |
|---|---:|---|
| ultrasharp-4x.bin | 33,424,520 | fb3e279d40d4cddb44db4e684d59e68d0aa39852c8cc14dc3f23ccc7e6eee9c1 |
| ultrasharp-4x.param | 116,029 | 0136ca83686809a8f17f7111f11b951e8db93610e24b7f4137c9ffe4dbc4a806 |

Padding choices across the eligible set: both axes wrap 3,449; both reflect 1,819; x wrap / y reflect 1,283;
x reflect / y wrap 530. These are edge-continuity estimates, not a semantic material classification.

## Fresh pass and interruption safety

The existing completed set was made on Oct 21. Tonight's explicitly requested new pass should use a named pass,
which starts from the original console images and can resume completed textures without restarting the entire set:

```powershell
& 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe' tools/texture_pack/upscale.py --model ultrasharp-4x --pass-id 2026-10-22-originals-pass --batch 16
```

Status at this note's creation: **GPU run pending parent scheduling**. Do not start concurrently with the video job
or an interactive game session without the parent coordinating it. Re-run exactly the same command after an
interruption. `--force` deliberately ignores completion and is unnecessary for a named fresh pass.

The new `upscale_passes.json` records the exact pipeline SHA-256, model weight/parameter SHA-256, and SHA-256 of
each completed original PNG and published replacement. A changed source or output is rebuilt. Changing code or
weights while reusing an existing pass ID is rejected; use a new pass name for a new pipeline. Existing simple
`upscale_state.json` model records remain compatible. A finished PNG is flushed, checked and atomically replaced
before its completion record is published. Both state files are now flushed and atomically replaced as well.

Other hardening: verify the selected model's two files before a job; count damaged original images as failures and
return a failing exit status; preserve vinyl exclusions through the saved `vinyl_hashes.txt` if the source index is
absent; handle both loose and archive-prefixed provenance paths. `source/` takes precedence over a same-name dump,
and `replace/` is never an input. Mask imagery remains outside the neural upscale path.

Validation: **10 Python tests pass**, including a mocked full batch that excludes masks, resumes without invoking
the network, rebuilds changed original pixels, and rejects a reused pass ID after model bytes change. Publication
tests preserve previous good PNG/state files on encoding, verification and rename failures. No GPU inference is
used by these tests.

## Read-only findings for the later PBR phase

The preexisting `tools/pbr/`, `docs/PBR_PLAN.md`, and `textures/pbr/` were inspected only. No PBR maps or manifests
were regenerated. The existing taxonomy contains 12,714 entries: 8,629 classified by path, 4,081 by low-confidence
image heuristics, and four tiny skips. Its largest classes are skip 6,111; facade 2,851; UI 1,276; rim 672;
signage 549; road_asphalt 278; rider 241; concrete 234. These counts describe the existing heuristic output,
not verified physical materials.

Before automated broad conversion, resolve these gaps:

- A single texture may be used in several draw roles. The current source index preserves only a first-seen origin;
  hash-level class alone cannot identify a reflection stage, coverage alpha, emissive additive card, or HUD reuse.
  The planned draw census should store stage, shader/combiners, alpha test/blend, and all observed roles. Mixed
  roles need separate material bindings or a conservative skip.
- Car and traffic atlases mix paint, rubber, plastic, lenses, metal, shadows and labels in one PNG. Bright,
  unsaturated pixels do not reliably identify bare metal: they also identify white paint and text. Use reviewed
  region masks and existing customization/shader signals before enabling metalness.
- DXT5 alpha is sometimes reflectivity and sometimes coverage. Image statistics can suggest a role but cannot
  prove it. Keep unknown roles inert until the actual draw combiner confirms them. Cutout coverage must survive
  all mip levels; normals must not turn transparent borders into geometry ridges.
- Dark images with bright patches are not necessarily emissive windows. City/time/weather and shader/draw evidence
  should distinguish lights from signs, reflections, black trim and UI before emitting light.
- The current path classes do not explicitly distinguish road markings, painted vs bare steel, coated vs chrome
  rims, wet vs dry surfaces, clearcoat vs base paint, or glass with decals. A reviewed subset should seed these
  distinctions before any whole-pack batch.
- Source-derived relief avoids converting UltraSharp's invented grain into bumps, but luminance-based height still
  mistakes painted lettering, stains and baked shadow for geometry. Keep strength conservative and provide per-region
  overrides. The learned models' outputs remain estimates, not recovered original authoring data.
- Offline edge continuity is useful for padding, but adjacent sky panels/atlases and UV islands need explicit seam
  metadata. A texture being repeatable along an axis does not prove its normal/AO maps should wrap that axis.

The user's sequence remains the gate: complete the current upscale work, verify a new backup of the entire project,
then start new PBR work. Runtime defaults must continue to use the present appearance.
