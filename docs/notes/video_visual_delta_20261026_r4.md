# Six additional strict movie visual reviews

The frozen progress snapshot contains43 strict completed movies. Only the six absent from the
previous37-movie visual coverage were sampled: `sgameout`, `sqtuner`, `sqperfom`, `sqluxury`,
`swcrace` and `stokmout`, all under `story/`. Previously reviewed clips were not resampled.

The unchanged CPU sampler produced18 exact-time source/output pairs,36 PNG captures and12
comparison sheets, with zero failures. All6,808 packet timestamps/durations matched. Root viewed
six sheets for Sqtuner/Sqluxury/Swcrace; an independent reviewer viewed the other six sheets.
Source/output/sidecar, sample and sheet hashes were checked again after viewing.

No large spatial corruption or framing/silhouette mismatch was observed at the reviewed scale.
Clearer contours coexist with limitations that remain relevant to the user's realism goal:

| Clip | Observed limits |
| --- | --- |
| Game Out | Faces/gloves/toolbox contours sharpen; faces become smooth/waxy and small toolbox script remains reconstructed. |
| Tuner | Skin/chest/cloth grain smooths strongly; beard edges and tiny lettering harden. Metallic patina simplifies into broader clean shapes. |
| Performance | Seams/flame edges sharpen; shirt-badge interiors acquire synthetic-looking strokes and fabric grain smooths. |
| Luxury | Hand/finger placement and pose remain coherent; rings/tattoos and small lettering remain ambiguous. Skin and cloth grain smooths. |
| WCRace | Large car/collar/brick outlines sharpen; grille mesh becomes dense reconstructed-looking strokes. Lip/teeth and sideburn edges harden. |
| Tokyo Out | Letterbox/layout remains intact; tiny faces, grille/spokes and badges remain model interpretations. |

These are relative source/current still assessments. There was no comparison with an older
upscale pass, inference rerun or live NPC/game validation. Three samples per clip cannot certify
complete motion, transitions, flicker or every frame. Sharper detail does not establish recovered
original high-resolution texture or authentic small mechanical geometry.

Evidence: `.local/video-visual-review-20261026-r4/visual-review.json`, SHA-256
`05e39f5f8898a757367f8c2e1a495f08ff9ecd17f636e1ce927fa80b025719c7`.
Independent complementary review SHA-256:
`dd9c09527dbb95677590217bfab412a87d5c6c556973388b575defa2b2d68d70`.
Frozen progress SHA-256:
`350474f6e3ec1d24ecb7c0edd1b49b1d1f6cf52c53e4624f838476b23b945759`;
sampler manifest SHA-256:
`2cfa3fc5d8cc4406c7002b721116cdbf68e60adeb49a358bf2165bb6b081ea72`.

No media recipe/output, GPU inference job, scheduling marker or runtime PBR setting was changed
by this review. All43 currently completed strict movies now have bounded still-review coverage;
the full106-movie pass remains incomplete.
