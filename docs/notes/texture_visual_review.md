# Texture visual review — 2026-10-24

All **7,081 active HD replacements** received CPU pair diagnostics and visual thumbnail triage. The reviewer
opened every contact sheet, `pack_001.jpg` through `pack_071.jpg`: 70 sheets of 100 textures and one of 81.
The sheet manifest equals the active replacement filename set exactly: no omissions, duplicates, or extras.
This is complete thumbnail coverage, **not complete full-pixel or native-game certification**. Thumbnails fit
160×154 pixels and the image viewer displayed the sheets at approximately 140×135 pixels per square texture.
The review then covered all **5,635 original source/dump textures without active HD replacements** in another
57 sheets: 5,603 categorical vinyl/decal masks, 23 flat textures and 9 tiny textures. Combined coverage is
**12,716/12,716 unique original textures across128/128 sheets**, with no omitted or duplicate thumbnails.
The parent independently checked the manifests against the current source/dump union and every reviewed
sheet hash. No obvious corruption was observed in those original masks; their tint regions, UV variants,
glyph/plate flips and atlas layouts remain original game data. No GPU worker, game, active PNG, original PNG,
source index, production upscale ledger or PBR file was changed by this review.

## Evidence and limits

The read-only `tools/texture_pack/audit_visual.py` decoded every active source/replacement pair, checked PNG
integrity, compared RGB after downsampling to original size, and compared alpha against the original alpha
resized with the production Lanczos rule. Results: **0 decode errors; 0 alpha mismatching images; 0 changed
alpha pixels**. Median RGB roundtrip RMSE is 4.5158 / 255; the 99th percentile is 20.47496 / 255. There are
79 RGB candidates above 20 / 255 and 135 candidates whose opposite-edge RGB difference exceeds the source
Lanczos baseline by more than 20 / 255 on an axis. These thresholds flag review candidates, not defects.
Alpha may encode reflectivity rather than coverage; a checker composite alone cannot establish native use.

All **79 high-RMSE pairs** were additionally inspected at equal 200-pixel fit in ten source/HD comparison
sheets. Most large scores are RGB changes under very low alpha, sharpening, fonts, small logos, or source
compression. Exact alpha and recognizable glyph layouts survive. No blanket font, logo, material-color,
face-restoration, or sharpness change is justified. Flat face/hand UV atlases intentionally deform the face
to fit a mesh; their shape is not evidence of broken NPC skinning. Native UV seams, animation flicker,
shader alpha interpretation, mip behavior, and exact on-screen appearance remain outside this static review.

Local evidence lives in `.local/texture-visual-review-20261024/`: `metrics.json`, `sheets.json`, `sheets/`,
`outliers.json`, `outliers/`, and `review_coverage.json`. Four focused helper tests cover damaged PNG reporting,
single-pixel alpha drift, read-only inspection, and Windows provenance paths.

## Specific findings and candidates

1. **Garage loading portraits require neighboring-panel context.** Individual original/HD comparisons of
   San Diego `4D145117D9DD0F63` / `A21582D8BE650E3F` and Tokyo `14E9720CCE89AA66` / `711D98603F8060F0`
   show UltraSharp emphasizing coarse skin, brow, hand, and background block contours. The originals are
   already low-resolution and compressed; the San Diego character's heavy brows and beard are original art.
   These images originate in `flash/loadscreen_garage_s.xbck` and `flash/loadscreen_garage_t.xbck`.
   The loading-artwork agent subsequently proved approximately six pixels of panel overlap (pitch 250) from
   actual SFlash fill matrices, matching image registration. **The early butt-joined face comparisons in
   `faces/` assumed pitch 256 and are invalid as native composition or seam-magnitude proof.** The four
   associated CPU face candidates are explicitly marked `REJECTED_FOR_ADOPTION` in their manifest. Do not
   publish them. Use the loading agent's verified layout and joined-source candidate workflow, then review
   the exact native result. Independent reflect padding cannot supply the missing interior neighbor context.

2. **Keyboard icon `AA18732C7786396C` has neural overpainting.** Origin:
   `assets_x.dat:texture_x/ui_typing.tex#L1@0x0 DXT1 32x32 L1`. The original has bright, regular white keys;
   the active HD adds gray/brown mottling and uneven key interiors. Its RGB roundtrip RMSE is 79.2451 / 255,
   the largest in the pack, although its alpha still matches exactly. A conservative CPU-only candidate in
   `ui-fidelity-candidate/` uses premultiplied RGBA Lanczos resizing of the exact original and copies the active
   alpha unchanged. Its manifest records original/base/candidate SHA-256 and exact alpha equality. The staged
   PNG was visually inspected. It preserves the original icon and deliberately forgoes invented surface detail.
   **It remains staged, unadopted**; parent integration owns authenticated correction provenance and native QA.

3. **Dense 64-pixel signage cannot establish recovered lettering.** Examples `864D8F3D8292C592` and
   `92C0715C034EE75B` (Tokyo prop/city atlases) become sharper but retain speculative-looking small strokes
   from heavily compressed sources. The source/HD comparison does not supply ground truth for missing text.
   These remain review notes; there is no accepted rewrite or generated replacement lettering.

Keep the complete original-derived base pass intact. This helper does not modify `upscale.py`, whose exact
digest belongs to the completed 7,081-image pass context. Corrections must retain their own authenticated
provenance instead of silently changing the base records or claiming the entire pack is artifact-free.

The four separately generated joint-source CPU x4plus composites are now complete, with eight split candidates.
The agent inspected San Diego, Tokyo and Atlanta, including faces/hands and flagged tile boundaries. The parent
also inspected Detroit and the San Diego/Tokyo full outputs. These comparisons show less rectangular facial
overpainting and coherent original shapes, without an obvious new tile grid at the reviewed scale. Original
eye shadows, heavy brows and compressed small details remain; no recovered-detail or native-layout claim follows.
The complete candidate/hash/alpha chain is in [loading artwork](loading_art_audit.md). All candidates remain
isolated until native source/HD/candidate A/B and strict correction provenance are complete.

## San Diego checkpoint — 2026-10-25

The genuine native baseline is `.local/loading-native-ab-20261025/s-before.png`, 1536×864,
110,336 bytes, SHA-256 `21370866bebdb37337e1064fa91e0bd6b369212c607c449205db5da5916d82f2`.
It preserves the raw returned screenshot bytes: **the encoding is JPEG despite the `.png` filename**.
Diagonal loading-wipe overlays remain visible across the portrait. A candidate `s-after.png` has not been
captured; an accepted native comparison still needs a candidate capture with comparable wipe phase and
settings. This baseline alone does not establish candidate acceptance.

A fresh CPU-only visual comparison of the joined original Lanczos, active HD and staged x4plus candidate
shows cleaner face/hand continuity and less fuzzy texture in the candidate. No obvious regular inference
grid was visible at the reviewed scale. The original already contains solid dark eye shadows and heavy brows;
the candidate hardens some of their borders. Mottled finger/beard detail and unresolved tattoo/sign strokes
remain source/model limitations, not authenticated recovered detail. These CPU composites are not native
after images. Both canonical San Diego PNGs still matched their base hashes at the read-only check; this
checkpoint records no adoption or canonical mutation.
