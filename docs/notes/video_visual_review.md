# Movie visual review — 2026-10-25

Actually inspected three matched-time original/current frame pairs for each of the 25 completed, strictly authenticated original-Bink/x4plus movies in the frozen review snapshot: **25 movies, 75 pairs**. Both full-scene sheets and native-output-pixel detail sheets were viewed. This covers the three startup movies and 22 reward movies; the other **81 of 106** movies were outside this snapshot.

Large lettering, car/bike outlines, grille bars and wheel edges are generally clearer than the original Bink displayed with Lanczos. No gross framing or silhouette corruption was apparent in these samples. Fine road, wood, paint, seat and chrome grain often becomes smoother. That supports the user's concern about muted fine detail even where larger edges improve; it does not establish that every removed component was authentic texture rather than source compression noise.

Some newly sharp microstructure is synthetic reconstruction. These are concrete examples, not a claim that the model restored authentic lost detail:

| Movie / sample | Exact presentation time | Source crop `[left, top, right, bottom]` | Observation |
| --- | --- | --- | --- |
| `sdlogo.bik`, 1 | `800/333` s (~2.402402) | `[296,246,424,342]` | Crosshatch-like lines appear in the purple field; the tiny registered mark is irregularly redrawn. |
| `reward/r300c.bik`, 3 | `143666523/10000000` s (~14.366652) | `[251,166,379,262]` | Oblique grille gains uneven directional/crosshatched lines and unequal diamond holes. |
| `reward/rskyline.bik`, 1 | `8999991/2500000` s (~3.599996) | `[476,206,604,302]` | Fine wooden-deck grain becomes broader and smoother while principal board seams sharpen. |
| `reward/rcad16.bik`, 2 | `129666537/10000000` s (~12.966654) | `[431,166,559,262]` | Grille lines are crisper, but small holes and crest blocks are irregular reconstructions. |

Other low-confidence features include tiny badges, license-plate headers, distant shop glyphs and chrome micro-highlights. Source headlight saturation and cinematic motion blur remain source limits. No output, recipe, model, coordinator state or GPU work was changed by this review.

Evidence is local under `.local/video-visual-review-20261025/`: `manifest.json` pins the frozen selection; each movie directory contains `samples.json`, the six decoded PNGs, `overview.png` and `detail.png`; `visual-review.json` records all 25 per-movie assessments and exact concern locations. Individual source Bink, output MP4, metadata-sidecar, sampled PNG and contact-sheet hashes are recorded. Full-scene comparisons share a display size; detail crops compare the original displayed with Lanczos against the current output at its native three-times scale. The x4plus inference is downsampled to the published three-times output.

The sampler first requires strict source/model provenance. It compares every source/output packet presentation timestamp and duration as exact fractions, then verifies FFmpeg's observed decoded timestamp for each requested frame. This snapshot had no sampling failures. Decoding was CPU-only, bounded to two decoder threads and one filter/PNG thread; no inference occurred. `packet_timestamps_matched` in each `samples.json` is the count of matching packets, not a Boolean flag.

Reproducibility identifiers:

- Frozen progress snapshot SHA-256: `9234d9d4450f20717b3fe83fe7d3dcb692ea9ba011bb235054fbd5ca875209e9`.
- `tools/video/sample_visual_quality.py` SHA-256: `d08abb06bbcc886f83b9f93cdb24ee630028fadddad18a7f45bbf8e217d38a9c`; its six focused CPU tests pass.
- NCNN x4plus parameter SHA-256: `35330ececcea33b6c397a72548e788d5d53becee4734c50b7fada36e89f10a86`.
- NCNN x4plus weights SHA-256: `713ee713b0353afaa27976f0563a64a5043bd70b9bd8936c2e26e25ebcdbcddf`.
- Inference executable SHA-256: `07e49f7cbb4ede01ae4dd4c399d3a7e5846e3d2085c3128eff881e55cb7b1a0c`.

Three stills per movie cannot certify temporal stability, shimmer, flicker, all transitions, or complete clip quality. Matching timing validates frame correspondence, not motion quality. No archived previous upscale was compared, so this review makes no claim of improvement over that prior pass. The existing pass remains unchanged; any future recipe comparison should target these exact timestamps and include a short motion review before adoption.
