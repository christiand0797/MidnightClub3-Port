# Reviewed texture correction audit

`tools/texture_pack/reviewed_fixes.py` is a read-only wrapper around the original texture audit. With no
`textures/reviewed_fixes.json`, its result is exactly the original audit result. It cannot install a candidate
or create an accepted review. There is currently no active correction ledger; all original pass records remain
unchanged.

Its first supported recipe is the eight staged, joined-source garage-photo x4plus candidates. A ledger must
pin every dependency by normalized project-relative path and SHA-256, including the original pass/context,
original sources/bundles, exact Flash layout, composite, actual CPU inference and split records, PTH/model code,
archived old PNGs, current PNGs, and explicit before/after review evidence. Path traversal and escapes through
links fail. Categorical masks cannot be excused by a ledger.

Each archived base PNG must still match the immutable original named-pass record. Current alpha must exactly
equal original Lanczos alpha. The audit recomputes the original-derived joined input and exact output split
pixels; merely repinning a modified output hash does not pass. It verifies the actual CPU architecture, device,
thread and model provenance rather than treating a planned GPU command as execution evidence.

Only a fully verified chain may account for that texture's `missing_or_changed_pass_record` issue. Every other
base audit issue remains visible and failing. A mixed result explicitly reports its original base model, the
current per-entry recipe and separate base/current/reviewed counts. It does not claim current x4plus output is
UltraSharp. Checksums authenticate the pinned recorded chain, not the identity of the reviewer; inference is
not repeated during this audit.

Parent verification passes13/13 tests: absent-ledger parity, valid chains, partial installation, missing
archive/proof/source, malformed global/entry data, model/pipeline/proof tampering, alpha and recomputed split
checks after repinning, categorical masks, path bounds, unrelated failures and explicit review decisions.
The actual untouched pack is also being audited through the absent-ledger path.

Before adoption, native A/B remains required for the real Flash layout. The coordinator must then select this
audit for approved supersessions, refuse any invalid ledger instead of rerunning the base worker over the
corrections, include all correction dependencies in its media signature, and reverify media/backup gates.
This scheduler integration and the reversible installation step are not implemented by the audit wrapper.
