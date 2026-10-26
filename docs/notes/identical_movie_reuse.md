# Reusing byte-identical original movies

The original `TokyoTut` and `Tutorial` directories contain two byte-identical Bink pairs, confirmed by full
file SHA-256, byte count and headers. `Tnavsys.bik` has 1,605 frames; `TNavimap.bik` has 1,484. Reusing one
verified result per pair can avoid 3,089 duplicate inference frames without changing any picture or model.
This is exact original-data equality, not a filename, visual-similarity or approximate-frame comparison.

The private read-only source proof is `.local/identical-movie-source-audit-20261025-r1.json`, SHA-256
`e0f534efa6e2f6db63411e9aa29d42ee0d521265b22b06b390d0e779a8fffa8a`.
The complete Bink hashes are `ef8ea3c5dc5771c0e0cc50facf25dae7b77e5886851e19cc8a23fdddbf0bb072`
and `edcbfc4dc67402391dd8531c4a4eb7e4a8595b0b7947315c2d2a66bc6f8a5ebf`, respectively.

The worker indexes previously completed original-Bink results by the entire exact picture recipe: original
hash/size/header, model weights/executable, inference/output scale, postprocessing, encoder and timing.
Legacy model markers and unverified historical attributions cannot enter the reuse path. `--force` explicitly
bypasses reuse. A match must refer to distinct source and destination movie paths.

Before copying, both originals must still have the same complete bytes. The reference output and provenance
sidecar are hash-pinned; current weights/executable must match. The output is an independent file, never a
hardlink. The copied stream passes the existing full frame/dimension/decode/exact-PTS validator. Immediately
before publication, including after pause and locked-file retry waits, the worker checks all original,
picture, proof, model, prepared-copy and destination anchors again. A destination edit is preserved.

The new record retains the exact original-Bink picture recipe and output hash, and adds
`identical_source_reuse` with the reference original/output paths and complete source/output/record hashes.
It records reuse of an existing original-derived inference result; it does not claim another neural inference
was performed. All requested source files still require their own current validation/coverage entries.
An ordinary game pause keeps this operation pending; explicit stop preserves markers and existing outputs.

Thirteen focused CPU fixtures cover exact provenance, changed source/weights/picture/proof, complete decode
failure, modified prepared files/destinations, pause/stop priority, hardlink independence, CLI coverage and
force bypass. One fixture uses a real tiny CPU-encoded stream and the complete timeline validator. No GPU or
canonical movie is used by these tests. The previous picture recipe/version stays unchanged.

The live worker loaded before this source change continues its current movie normally. Editing the script
does not retrofit it. Activation requires a later safe coordinator/worker restart; preserve its in-flight
work and existing stop requests. No actual canonical movie has been published through reuse at this checkpoint.
