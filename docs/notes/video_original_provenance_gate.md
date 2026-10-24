# Complete original-derived movie pass: strict provenance gate

The requested new detailed pass covers all 106 original Bink movies. A matching historical model marker,
even after a full decode/timestamp audit, cannot establish which original source or model weights produced
an older HD picture. The default video-tool compatibility path retains that distinction and continues to
describe adopted historical pictures as unverified; it never retroactively turns that claim into proof.

`tools/video/upscale_movies.py --require-original-provenance` adds a stricter pass policy. Resume accepts only
records with `origin: original-bink`, historical source/weights verification exactly `true`, matching source/
output hashes and the current source/model/weights recipe. A proven prior encoder-clock recipe can still be
validated/reused or losslessly retimed under the existing packet/PTS checks. A legacy marker or false-provenance
record instead receives fresh inference from the original Bink. Its working HD picture stays published until
the replacement passes the existing complete decode/timeline/geometry checks and atomic publication.

Strict `--audit-only` remains read-only for movie files and sidecars. A valid historical picture is reported as
`provenance-pending`; a changed current model-weight recipe also remains pending. Exact source/output hash
disagreements still fail validation. Marker-only `--status` reports required legacy regeneration without
pretending its inventory is a complete validation result.

The durable coordinator passes this flag to both upscale and final audit. Its media gate requires policy
`original-bink-and-model-weights-v1`, strict report flags, and all 106 distinct rows with verified original
source/weights provenance. Old model-only caches are cache misses and re-enter processing/validation. An old
completed-PBR checkpoint or backup marker without the policy cannot authorize new offline PBR work. Existing
backup artifacts are preserved. Runtime PBR is unchanged.

CPU checks cover strict/default compatibility, no legacy proof mutation, matching fresh-record reuse,
changed-weight rejection, read-only CLI audits/inventory, strict worker regeneration, report coverage, old-cache
migration and backup/PBR bypass refusal. The movie tests use synthetic FFmpeg inputs and a fake inference
boundary; they do not start a GPU worker. The strict maintenance suite passes 27 checks.

A read-only check of actual local records confirmed that `rockstar`, `sdlogo` and `mc3intro` need the new
original-derived inference pass, while the newly completed `reward/rninja` remains reusable with unchanged
source/model/weights. Evidence is `.local/video-strict-resume-check-20261024.json`. This check did not modify
any published movie or sidecar. The parent controls cooperative coordinator restart and preserves user game
sessions and stop markers.
