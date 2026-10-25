# Loading and garage artwork audit — 2026-10-24

The complete loading-art inventory is clean at the encoded-source and PNG levels. This does not yet establish the
cause of the reported native rendering glitch. A separate original-derived candidate pass is prepared without
changing the active pack, the 7,081-texture pass ledger, original bundles, saves, or runtime rendering.

## Inventory and source proof

`tools/texture_pack/loading_art_audit.py` discovers all `loading_*.xbck`, `loadscreen_*.xbck`, and
`loadmovie*.xbck` files directly. It does not rely on the first-origin-only `source_index.json`, which hides
textures reused by another Flash bundle.

- **205 loading bundles**, with names and every byte compared against the supplied original disc image.
- **475 distinct texture/palette hashes**: 434 DXT5, 10 DXT1, and 31 P8.
- Every existing source PNG matches a fresh original decode. Pillow's independent BCn decoder crosschecks
  the compressed formats, allowing only documented decoder rounding (RGB ≤3, alpha ≤1).
- All active replacements pass PNG decode/CRC, dimension, original-alpha Lanczos, and immutable base-pass
  source/output hash checks. The base-pass model and extraction/upscale context also match.
- **444 color-art jobs** are eligible for candidate inference. **31 constant-color structural alpha masks**
  retain their existing verified replacements; a color network would add no useful structure.
- The generic material-tiling heuristic marks **97** of these textures for at least one wrap axis. Finite loading
  artwork should use reflected outer context instead. Both Tokyo garage photo panels had wrap-Y enabled.

The read-only report is `.local/loading-art-audit-20261024-final.json`, SHA-256
`f314ad7218c61f80713c3351bcb5c4a66e4f03acc004c71f90a8dc606a49f125`.
The 444-job manifest is `.local/loading-art-candidates-20261024-final.json`, SHA-256
`8bd0860e3091653630134c5e80cb5acff17ffc4e9dbc478cdc441cc1a1c6a1f9`.

`loading_art_worker.py` prepares an isolated ledger by default. Its optional GPU run requires the existing worker
inventory to be clear, honors the existing stop requests, and acquires one candidate-worker lock across staging
directories. It always starts from original PNGs, reflects both axes, preserves original alpha, and writes only
under a dedicated project `.local` directory. Preparing a ledger does not schedule a job. The initial ledger is
`.local/loading-art-reflect-candidates-20261024-r1/loading_candidate_pass.json`; no GPU candidate job has run.

## Garage photo layout and the actual overlap

`loading_garage_join.py` follows the original Flash memory-image pointers from each photo texture's bitmap
wrapper through its fill record to its owning shape and indexed quad. It retains the exact matrices and geometry
as evidence. This matters because file order, fill order, and visual left/right order are not always the same.

| City suffix | Left texture | Right texture | Fill indices / count | Quad overlap in twips |
| --- | --- | --- | --- | ---: |
| a | `2C31319DDD40661C` | `BA7DAD34CC50D094` | 2, 1 / 2 | 18 |
| d | `11602EACA97BFCEF` | `70A5C638246CF68C` | 1, 2 / 2 | 9 |
| s | `4D145117D9DD0F63` | `A21582D8BE650E3F` | 1, 2 / 2 | 0 |
| t | `14E9720CCE89AA66` | `711D98603F8060F0` | 5, 13 / 13 | 82 |

Each photo is two 256×256 DXT5 images. Independent registration of opaque source pixels over candidate overlaps
1–16 columns selects **six columns for every city**, with mean RGB errors 2.086, 2.622, 2.119, and 1.684
respectively. A simple 512-pixel butt join duplicates these columns and is not a faithful native seam diagnostic.

The source image pitch is 250 pixels, giving a **506×256** composite. The original fill transforms are retained
unchanged: the Tokyo scales differ slightly and its geometry overlaps more than the other cities. The image
registration alone does not prove the final native UV sampling or the exact on-screen seam; original-versus-HD
native A/B remains necessary.

The prepared four-city plan is `.local/loading-garage-joined-20261024-r3/joined-plan.json`, SHA-256
`79c0965bf0d9697ccb73d9c3392311480e7b9f626eff3a97c021c3ad96e976f1`.
It pins the original bundle, fresh decoded source and alpha hashes, exact fill/quad records, base replacement
hashes, registration scores, and padded input hashes. Earlier `r1`/`r2` directories are preserved investigation
artifacts and are not active candidates.

The composite blends only the six columns that already overlap in the independently compressed originals.
All other source RGB pixels remain unchanged. A 32-pixel reflected outer border creates a 570×320 network
input. After 4× inference, the border is cropped once and the 2024×1024 result splits at x=0 and x=1000 into
two 1024×1024 panels. Their shared 24 RGB columns are identical. Each panel receives its own original alpha
resized with Lanczos; alpha is never inferred. Native texture names, geometry, and layout remain unchanged.

Eight CPU Lanczos composite splits are available in that directory's `cpu-source-fidelity/` for review. These
are conservative candidates, not an increase in recovered source detail. No arbitrary blurred seam strips or
generative alterations are applied.

## Bounded CPU inference and provenance

The four joined composites use the separately tested `cpu_x4plus.py` PyTorch backend and existing official
`RealESRGAN_x4plus.pth` weights. The backend strictly validates all 702 state keys against the official RRDBNet
architecture, maps weights and tensors to CPU, and limits intra-op and inter-op threads to two. Its tests include
an independent official-class output comparison. It makes no claim of NCNN equivalence.
That architecture comparison used the backend developer's PyTorch 2.10 CPU runtime; the staged candidate run
records its own PyTorch 2.6.0+cu124 runtime with every tensor explicitly on CPU. No bit-equivalence across these
different library versions is claimed.

`loading_garage_join.py --staging <dedicated stage> --run-cpu` runs exactly four pinned inputs serially. It uses
the shared candidate lock and checks a private `.local/stop-loading-cpu` marker between tiles. The durable GPU
coordinator retains its own scheduling and stop requests. CPU outputs are new isolated PNGs, never active
replacements. Each completed image appends actual source/output, PTH/module digests, CPU architecture,
thread/tile configuration, and timing to `inference-record.json`.

`--split-network --inference-record <record>` independently verifies input/output hashes, backend identity,
current model/module files, exact architecture/key validation, CPU threads, and all four completed records.
It then creates eight isolated candidates. A split without an inference record is explicitly marked as an
unauthenticated diagnostic; a planned NCNN command never authenticates actual CPU output.

The current 20 loading tests cover independent compressed decode, candidate isolation/stops/provenance,
reflection, original-alpha preservation, immutable active files, synthetic thirteen-style Flash bindings,
actual four-city original layouts, exact shared-column split, bad input/changed-source rejection, and a gradient
diagnostic that detects injected grid steps. Run:

```powershell
python3.12.exe -m unittest discover -s tools/texture_pack -p test_loading*.py -v
```

`loading_garage_review.py` compares completed candidates with the same registered original Lanczos and active
HD composites, retaining the exact CPU inference record. It marks the actual tile96/pad32 lines after border
crop: x=256, 640, 1024, 1408, 1792; y=256, 640. Mean RGB line jumps are compared with neighboring gradients
and saved alongside full-size, equally registered review images. These statistics guide visual inspection;
natural scene edges can coincide with tile lines. They do not establish whole-image inference equivalence.

All four serial CPU jobs completed: s=116.906 s, t=117.656 s, a=118.750 s, d=117.734 s; total inference
**471.046 s**. The authenticated split produced eight 1024×1024 RGBA candidates under `r3/neural-candidates/`.
The final integrity check confirms original-alpha Lanczos equality, identical shared 24 RGB columns, exact
pixels from the recorded padded composite output, and unchanged **205 original bundles +475 source PNGs
+475 active PNGs**. No candidate was installed.

Of 28 measured internal tile lines, 23 are below their neighboring 95th-percentile gradient. The five flagged
lines coincide with a source or existing-HD scene edge at the same position (a:y256/x1792, d:y640/x1024,
t:x1792). Full-image inspection found no obvious rectangular grid; this is not proof of whole-image inference
equivalence. Face/detail review and native A/B remain separate gates. The complete equally registered figures
are `r3/boundary-review/{a,d,s,t}/source_active_candidate.png`, ordered original Lanczos, active HD, CPU candidate.

Final proof hashes:

- `r3/inference-record.json`: `8c031f350f4b9749a1b6df19b9e19ff70a09f1560889dfc946d99a45c6377079`.
- `r3/neural-candidate-records.json`: `12d86dced46c952b48aedb3ea12474c25aed76c4d6c185424a9d8084a37e54a5`.
- `r3/candidate-integrity.json`: `041588adddcf14cfce54faf40c4dc37b1f8fa0e4cb1ad99f4ecc58da081bf4fa`.
- Frozen compute helper: `da0387f01faa984fd98ff07a364c2cf807c6dcfb2695b17547848734b80ae989`.
- Separate boundary diagnostic helper: `2441318dc293fc802cfc9af6bf7fab630571538a93d8bc8b66932fa937fe7e49`.

## Adoption and media gates

Private native comparison on2026-10-25 observed the original active San Diego loading portrait in r7 and
the joined CPU candidate in r8/r9. The candidate reduces blocky facial overpainting and keeps a continuous
brow/skin/hand across the original panel join, with no visible hard seam. The other three cities still need
their native Flash-layout comparisons. No canonical PNG was replaced. The isolated QA directory uses7073
read-only base hardlinks and eight separate candidate copies; those base hardlinks must never be overwritten.

Candidate generation and review do not supersede the original-derived 7,081-texture pass. Before any adoption,
the coordinator must require an explicit approved overlay with:

1. Original bundle/layout and source hashes, exact old/new output hashes, actual model/backend/module metadata,
   source commit, and native original/HD/candidate A/B evidence for each adopted set.
2. An unchanged active old-output anchor for every replacement. Preserve the base outputs and original pass
   records, publish only while the game is closed, and retain a reversible rollback set.
3. A strict media audit of the unchanged base plus exactly the approved supersessions. Unknown changed files
   must fail. An overlay does not overwrite or silently rewrite the historical base-pass completion records.
4. The overlay records, layout proof, worker/model context, and adopted PNG digests included in the durable
   `media_signature`, followed by a verified full-project backup before any offline PBR generation.

Parent integration owns these gates. The present work changes no active images or runtime PBR behavior. Joint
processing of additional loading pairs should follow proven layout/registration and the reviewed four-city
results; reflecting each unrelated texture is not a substitute for confirming an atlas relationship.
