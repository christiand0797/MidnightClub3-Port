# Sqmuscle bounded still review — 2026-10-26

Only the newly completed `story/sqmuscle.bik` was sampled. The prior43 reviewed movies were
not resampled. The current HD output remains SHA-256
`d8b3fb16aad2a7db2be0a0f928b3d1ea92851eccc214489ccbb888941a9c33ad`.
Original Bink, current HD and sidecar bytes matched the independent frozen cohort before and
after sampling, and again after visual review. The unchanged CPU sampler used at most two
decoder threads. All1,497 packet timestamps/durations matched.

Three exact-time original/current pairs were sampled:

| Source/output exact time | Seconds |
| --- | ---: |
| `29900/2997` | 9.976643 |
| `74800/2997` | 24.958292 |
| `13300/333` | 39.939940 |

The standard sampler's two aggregate sheets remain preserved. Six per-pair sheets are exact
row crops, with no resizing or color change. The independent subagent actually viewed all three
overview rows; root actually viewed all three complementary detail crops. No selected sheet
remains unreviewed. Combined with prior coverage,44 distinct strict completed movies now have
bounded still review; this does not establish full106-movie completion.

The selected Sqmuscle stills preserve mechanic/car/seat composition, pose, hand placement and
broad garment/arm contours. No obvious structural tear or framing mismatch was observed at the
reviewed scales. Tank-top, brow/beard and hand edges look cleaner, while floor/dark fabric
patterns become sharper and more segmented. Root's detail review confirms aligned arm/tattoo
and garment boundaries, with smoother or reconstructed shading/ink. Skin, fingers and small
tattoo strokes remain source/model limited; sharper texture is not authenticated realism or
recovered high-resolution anatomy/lettering. Root's detail crops do not establish face quality.

This compares the original Bink with the current output, without an older-pass comparison.
Three still pairs cannot certify every frame, transitions, flicker, movement, facial animation,
audio or native playback. Overview is640x480 per side; detail enlarges selected128x96 original
regions to384x288. No recipe, media byte, inference job, runtime PBR or scheduling state changed.

Evidence is in `.local/video-visual-review-20261026-r5/`:

- `visual-review.json`: SHA-256 `9cc39032bf7c0a22e960e69c7a366d17c7e5002c280ddb5a5f4fc3549616f14c`.
- `frozen-parity.json`: SHA-256 `dedd86ac235f83ff16cf5da8f043e82f30964f9813b332b7001a8c71516ddd47`.
- `story__sqmuscle/samples.json`: SHA-256 `8d392ef9ea47222ecd4ad6f16a8f3f1a6957174d0af7a58f70cee2e349caa60a`.
- `manifest.json`: SHA-256 `35a1768937d865fee2aa650dd141e35abe329b2aa90134076d4053dd0456367f`.
- `progress-snapshot.json`: SHA-256 `24a54fa2ab9c145e30756b04c44b50460fbe2efdcb16ac9f9b39dcc4b8500d35`;44 strict movies, zero failures.

The previous43-movie coverage reference remains r4 `visual-review.json`, SHA-256
`05e39f5f8898a757367f8c2e1a495f08ff9ecd17f636e1ce927fa80b025719c7`.
