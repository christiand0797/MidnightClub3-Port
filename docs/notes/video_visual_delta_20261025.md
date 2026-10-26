# Movie visual review: eight newly completed clips

2026-10-25 MDT. The unchanged CPU-only sampler reviewed only the eight strict original-Bink/x4plus movies
absent from the earlier 25-movie snapshot: `reward/rcharger.bik`, `rzonda.bik`, `rsport.bik`, `rdeville.bik`,
`rcobalt.bik`, `rg35.bik`, `rmagnum.bik` and `rgroupc.bik` (all under `reward/`). No old movie was resampled.

The new frozen snapshot contains 33 verified records; this delta actually sampled **8 movies / 24 matched-time
frame pairs**, not all 33 again. All 16 full-scene/native-output-detail sheets were visually inspected. Sampling
reported zero failures and matched all 4,586 source/output packet timestamps and durations in these eight clips.
Three still pairs per clip do not certify motion, flicker, every frame or every transition.

Large titles, car outlines, road markings and grille borders are generally clearer. No gross framing or silhouette
corruption was apparent in the reviewed stills. Fine paint/road/stone grain is smoother, and small reconstructed
features remain uncertain. Concrete examples:

| Movie / sample | Observation |
|---|---|
| Charger 3, ~14.366652 s | Sharper but unequal/bent small grille cells and bars. |
| G35 2, ~8.966658 s | Small grille badge and chrome streaks become thick, uneven bright shapes. |
| Magnum 2/3, ~8.966658 / 14.366652 s | Uneven square/rounded grille cells; bright rows partly fuse into hard ridges. |
| Zonda / Sport / Cobalt | Tiny logo lettering, shop glyphs and poster marks gain synthetic strokes rather than verified lost detail. |
| Group C 3, ~21.488155 s | Strong transition blur already exists in the exact-time original; this sample does not establish sharp car detail or a new output defect. |

Per-movie assessments, exact times/crop coordinates, original/output/sidecar hashes and all sheet/sample hashes
are in `.local/video-visual-review-20261025-r2/visual-review.json`, SHA-256
`5b28e83ba2330ea6f8436ce5a0acf4481b029acbc019e458e8a2703b65dfd1b2`.
All input and sample hashes were checked again after viewing. The frozen progress snapshot SHA-256 is
`af8857636376913ba927baaf65dcbf541c6953908f1856d2b63a98772f3dbfe6`; sampler manifest SHA-256 is
`51542b754773236784e674b4235159ab709f0df09da146bf7efd8149497b797b`.

Sampler SHA-256 remains `d08abb06bbcc886f83b9f93cdb24ee630028fadddad18a7f45bbf8e217d38a9c`.
The recorded directory-creation-to-final-manifest span was ~43.7 s, 04:50:37–04:51:21 UTC on October 26
(October 25 MDT); that includes hashing/I/O and is not CPU process time. Decoders used the existing two-thread
limit and single filter/PNG threads. No inference, GPU work, scheduling markers, native game, canonical outputs
or previous review artifacts were changed. No earlier upscale pass was compared or new recipe adopted.

The original [25-movie review](video_visual_review.md) remains a separate historical sample set.
