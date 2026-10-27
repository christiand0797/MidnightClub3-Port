# Svincint and Satlcut still review — 2026-10-26

Only `story/svincint.bik` and `story/satlcut.bik` were sampled: the exact difference between the
current46 strict completed movies and r5's44-movie coverage. The previous44 were not resampled.
Stokmech's in-flight partial output was not sampled. The immutable release snapshot and bundle
remain unchanged at their frozen44 strict /62 historical cohort.

The unchanged CPU sampler SHA-256 is
`d08abb06bbcc886f83b9f93cdb24ee630028fadddad18a7f45bbf8e217d38a9c`. It used at most two decoder
threads, one filter thread and one PNG thread. Original Bink, current HD and provenance-sidecar
hashes matched before/after sampling and again after the actual visual review. Both current outputs
retain strict original/x4plus source/model/output records:

- Svincint HD: `75e93924b8ca43c2c07d3f9fbc46149836f195f22aa137b66f0db9aca68eb685`.
- Satlcut HD: `d7edf933b76a567f0c4a6c719d63e308dd2bca8ccdb7e6071cd9570da1b4d0a9`.

All1,603 Svincint and1,807 Satlcut packet timestamps/durations matched the corresponding originals.
Six exact-PTS original/current still pairs were selected:

| Movie | Exact time | Seconds |
| --- | --- | ---: |
| Svincint | `32000/2997` | 10.677344 |
| Svincint | `8900/333` | 26.726727 |
| Svincint | `128200/2997` | 42.776109 |
| Satlcut | `36100/2997` | 12.045379 |
| Satlcut | `30100/999` | 30.130130 |
| Satlcut | `144500/2997` | 48.214882 |

The subagent actually viewed both three-row overview sheets; root actually viewed both
complementary three-row detail sheets. All six overview and six detail comparisons were inspected,
with no selected sheet left unreviewed. There are12 raw source/output PNGs and four standard
aggregate comparison sheets. Combined with previous coverage, **46 distinct strict movies** now
have bounded still review.

Svincint preserves the mechanic's pose, green car/wheel, sign and garage framing. Brick, shirt,
wheel and body-panel edges look cleaner. Root's detail views confirm aligned green panel/arch/amber
marker and beard/collar contours; fine wheel/spoke/reflection texture is reconstructed while
shirt/skin texture is smoothed. Fine floor/fabric texture also becomes smoother or segmented.

Satlcut preserves both characters' poses, mouth/eye placement and garage/car composition. Garment,
tattoo and sign contours sharpen. Root confirms aligned hand/car/tank-top/chest outlines, with
sharper but stylized/reconstructed tattoo lettering; skin, chest hair and fine hand texture are
smoothed. Neither reviewer observed an obvious structural tear or framing mismatch at these scales.

These are original-versus-current comparisons, with no older-pass comparison. Cleaner contours
do not prove recovered anatomy, tattoo-text authenticity, tiny lettering or realism. Three still
pairs per movie cannot certify every frame, movement, transitions, flicker, facial animation, audio
or native playback. Overview displays640×480 per side; detail compares a128×96 original region
enlarged by Lanczos with its384×288 native output crop. No exposure normalization was applied.

Evidence is preserved in `.local/video-visual-review-20261026-r6/`:

- `visual-review.json`: `c49106fa2cff9a47cac496ac77ccb321827f89bdbc75a655ffe55657b0752c10`.
- `manifest.json`: `3ce7516c50e9414f716a7bdf953c1698ab68f39d9668685031d6ce6c06405ebc`.
- `delta-context.json`: `5c4fc26ac6347033b2ebbe6b4549c1e5a9ebc602f0fa6189b29009d32a2c3c91`.
- `story__svincint/` and `story__satlcut/`: exact-time samples, unchanged aggregate sheets and
  per-clip `samples.json` with input, model, decode, crop and sheet hashes.

Prior coverage remains r5 `visual-review.json`, SHA-256
`9cc39032bf7c0a22e960e69c7a366d17c7e5002c280ddb5a5f4fc3549616f14c`, and its linked earlier reviews.
No GPU inference, media replacement, scheduler/stop-marker change, code edit or runtime PBR change
was performed for r6. This review does not replace any byte in the frozen release cohort.
