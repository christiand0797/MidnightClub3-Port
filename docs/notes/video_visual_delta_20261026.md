# Movie visual review: next four completed clips

2026-10-26 MDT. The unchanged CPU sampler was run only for `reward/rgroupb.bik`, `story/sqsuv.bik`,
`story/slastour.bik` and `story/sphil.bik`: the four strict completed movies absent from the earlier 25-movie
and eight-movie review sets. None of the previously reviewed 33 movies was resampled.

The new snapshot contains 37 strict records. This delta sampled **4 movies / 12 matched-time frame pairs**;
all eight full-scene/native-output-detail sheets were actually viewed. Sampling had zero failures. Every one
of the 3,816 source/output packet timestamps and durations matched exactly. Three still pairs per movie do
not establish complete clip quality, motion consistency, flicker or every transition.

No gross framing or silhouette break was apparent. Larger edges become clearer while fine textures and
facial features remain model interpretations:

| Movie | Observed limits |
|---|---|
| Group B reward | Deck/railing grain becomes broader and smoother while board seams sharpen. |
| SUV story | Cart/cloth/skin/paint texture is simplified; wheel hub and tiny trim/letter shapes are reconstructed from compressed source marks. |
| Last Tour story | Headlamp internals sharpen without establishing authentic reflector detail. Jaw/neck and cloth texture become smooth; lip/teeth boundaries harden. |
| Phil story | Cheek/neck grain smooths strongly and facial-hair boundaries harden. Tattoos/chest lettering remain ambiguous; source shoulder/vest edge aliasing remains visible. |

These results support the fine-detail limitation already recorded in the previous reviews. They do not prove
that all removed grain was authentic texture rather than compression noise. No prior upscale pass was compared.

Local evidence: `.local/video-visual-review-20261026-r3/visual-review.json`, SHA-256
`69ce36ad0e275d7233e8ec44ce0c19047fe1362e841db3351e314c216262f219`, records per-movie assessments,
exact sample times/crop boxes and source/output/sidecar/sample/sheet hashes. All four input chains, 24 PNGs
and eight sheets were hash-checked again after viewing. Frozen progress snapshot SHA-256:
`c9a23fa738e4ee0309b6d47d828780313947abf7d60bcacdd5105b4482beb8f0`; sampler manifest SHA-256:
`8e14b0062beed367c1fef6c741e870420fab72246c338cf3a3c9e6e47393598a`.

Sampler SHA-256 stays `d08abb06bbcc886f83b9f93cdb24ee630028fadddad18a7f45bbf8e217d38a9c`.
The directory-creation-to-final-manifest span was ~21.94 s, 08:16:50–08:17:12 UTC on October 26; this includes
I/O/hashing and is not CPU process time. Existing limits of two decoder threads and single filter/PNG threads
were retained. No inference/GPU, native session, scheduling marker, media recipe, canonical output or earlier
review artifact was changed. The [25-movie review](video_visual_review.md) and
[eight-movie delta](video_visual_delta_20261025.md) remain separate historical evidence.
