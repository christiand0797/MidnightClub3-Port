# Four-movie still review — 2026-10-28

The four newly strict completed movies after [r6's 46-movie coverage](video_visual_delta_20261026_r6.md)
were sampled: `story/stokmech.bik`, `story/sdetcut.bik`, `story/saponint.bik` and
`story/sqsportb.bik`. The previous 46 were not resampled. Sgameint's in-flight output and all
partial/temp files were excluded. The immutable release snapshot remains its frozen
44 strict / 62 historical cohort; this review does not replace or relabel those movies.

The unchanged CPU sampler SHA-256 is
`d08abb06bbcc886f83b9f93cdb24ee630028fadddad18a7f45bbf8e217d38a9c`. It used at most two decoder
threads, one filter thread and one PNG thread. Current original-Bink/x4plus source, model,
sidecar and output hashes matched before/after sampling and again after actual visual review.
All 8,581 packet timestamps/durations across these four movies matched their originals:

| Movie | Packet/frame count | Exact selected times | Seconds |
| --- | ---: | --- | --- |
| Stokmech | 2,210 | `44200/2997`, `36800/999`, `58900/999` | 14.748081, 36.836837, 58.958959 |
| Sdetcut | 1,875 | `12500/999`, `93700/2997`, `149900/2997` | 12.512513, 31.264598, 50.016683 |
| Saponint | 2,136 | `42700/2997`, `106700/2997`, `170800/2997` | 14.247581, 35.602269, 56.990324 |
| Sqsportb | 2,360 | `47200/2997`, `13100/333`, `1700/27` | 15.749082, 39.339339, 62.962963 |

The reviewing agent actually viewed all four three-row overview sheets and all four three-row
detail sheets: 12 matched-PTS pairs, 24 raw source/output PNGs and eight aggregate sheets.
No selected sheet remained unreviewed. No independent second/root visual review is claimed.
Together with previous coverage, **50 distinct strict movies now have bounded still review**.

Stokmech preserves the mechanics' poses, visible hands, hood/car framing, zipper and hat/shirt
outlines. Edges sharpen, while fabric and trouser grain simplify into broad smooth folds.
The foreground hand has clearer boundaries, but fine skin/anatomy remains model interpretation.

Sdetcut preserves the character, wheel and car/garage layout. Grille, panel and cuff contours
are cleaner. Fine wheel spokes/yellow inserts, watch/finger details and tiny lettering remain
ambiguous or reconstructed-looking. Skin, tank-top and trouser texture smooth.

Saponint preserves the mechanic's pose and raised arm, red car, pillars and toolbox arrangement.
Toolboard holes, rails and drawers become more distinct, with reconstructed-looking fine tools,
container edges and small text. Selected detail crops emphasize the toolboard; they do not
independently establish facial-detail accuracy.

Sqsportb preserves character/car placement and hand/body silhouettes. Beam, wheel, headlight
and broad grille contours improve. Chrome/grille patina and tiny badge/sign/SUV lettering can
become stylized patterns; skin and fabric simplify. No obvious structural tear or framing
mismatch was observed in these selected comparisons.

These compare original Bink with current output, without an earlier-upscale-pass comparison.
Overview displays 640×480 per side; detail compares a 128×96 original region enlarged with
Lanczos against its 384×288 native-output crop. No exposure normalization or corrective editing
was applied. Cleaner edges do not prove recovered pores/anatomy, authentic lettering or maximum
realism. Three still pairs per movie cannot certify every frame, movement, facial animation,
transitions, flicker, audio or native playback. Packet timing comparison is not full frame decoding.

## Evidence

Local evidence is `.local/video-visual-review-20261028-r7/`:

- `visual-review.json`: `9d22c19874f2fccd6aea6f0ef1d4932d43841194a92bfea1a5716d004bff0809`.
- `manifest.json`: `ebe0fe686e300589325f50c6fd6472703b1e8b48e5bc43c3f7152642d8aa4e1c`.
- `delta-context.json`: `66f3c475e5f4c85146fc51e1c11c8502ca85ad9da44b5465727b40e2dbb92429`.
- Prior r6 review: `c49106fa2cff9a47cac496ac77ccb321827f89bdbc75a655ffe55657b0752c10`.

All 40 evidence files also have independently copied and verified bytes under
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/video-visual-review-20261028-r7/`.
Its `copy-receipt.json` SHA-256 is
`e9b7ab9ae0c794924c9ecf5af0e3d24a7d032792d3b8e8a9276c6b237a61a34d`.
The original/current media themselves were neither copied into that evidence folder nor modified.

| Current HD output | SHA-256 |
| --- | --- |
| Stokmech | `6587a7bfd3b26f76f5c6b1c0c3509bf4161c7c77861acbf875ebae44ca7ccd09` |
| Sdetcut | `3eeec76a1b3c82c6a6babb6f73b55d5a44629e5750a7a8dc816dd8c131f20b2b` |
| Saponint | `4622bf8c283959995d81cf3edf1f683a04acf06cf0a9bcf3a8822e89807597e2` |
| Sqsportb | `1087f842c3c6d72bed1fd8dd89e903d654581e795e1bc614754c3c3482f95eb3` |

No GPU inference, media replacement, game/UI control, scheduling/stop-marker change, runtime
PBR change or change to the frozen release cohort occurred in this review.
