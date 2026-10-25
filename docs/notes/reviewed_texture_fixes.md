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

Parent verification passes14/14 tests: absent-ledger parity, valid chains, partial installation, missing
archive/proof/source, malformed global/entry data, model/pipeline/proof tampering, alpha and recomputed split
checks after repinning, categorical masks, path bounds, unrelated failures, explicit review decisions and
compatibility of an actual fully audited correction fixture with the coordinator's media gate.
The actual untouched pack passes the absent-ledger path:7081/7081 with empty issue lists.

The coordinator integration now audits an existing correction ledger **before** any original texture worker
could run. A complete audited base+correction pack skips that worker; invalid/missing proofs stop the workflow
and preserve the current PNGs. The final media check uses the same reviewed wrapper, with separate base/current
counts, exact accounted names, declared CPU recipe and strict original-movie provenance still required.
Reviewed audit reports use a unique temporary JSON path and are published atomically only after success;
failed evidence remains separate and the last good report survives.

`media_signature` includes the ledger, the correction audit/compute/join/coordinator code and every pinned
dependency. Correction dependencies are SHA-256 checked on each signature, including equal-size edits with
preserved timestamps. Missing, changed, traversing or escaping dependencies fail before GPU work. The normal
base audit path and original pass/model records are unchanged. All34 coordinator tests pass, including original
game/user-stop scheduling, bad-preflight preservation, original-worker exclusion, mixed coverage, proof tampering
and unchanged-cache reuse; the14 correction tests include an actual verified-chain report accepted by this gate.

Native A/B remains required for the real Flash layout before each adoption. No active ledger/PNG is installed;
reversible installation remains outstanding. The live movie coordinator loaded its code before this integration:
let its owned movie finish and restart at a cooperative boundary before installing any correction. Do not kill
or duplicate the worker. Reverify media and full-backup gates before new offline PBR.
