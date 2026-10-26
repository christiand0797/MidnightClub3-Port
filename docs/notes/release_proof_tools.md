# Authenticated reviewed-texture proof package

The portable content format carries ISO/PNG/MP4. A corrected-content release therefore needs a
separate proof archive to preserve the installed San Diego correction's auditable history.

`package_reviewed_proof.py` starts from an explicit trusted ledger SHA-256 and verifies its
required source/model/tool/composite/inference/split/review dependencies. Format-specific fields
expand immutable native review documents, contexts, closed logs, raw captures and photo metadata.
Unknown proof formats, changed/missing required files, conflicting hashes, unsafe/case-colliding
paths, links and private payload references reject packaging. A new output directory is required.

Historical observations remain exact JSON metadata. For example, the old handshake's canonical
S hashes describe the pre-install files, while today's installed canonical paths hold the accepted
new bytes. Those observations are not conflicting live payload requirements. The package lists
these exclusions explicitly; it does not repin history or place an old archive under a new path.
Actual settings, HDD/save files, shortcut/token-marker files, credentials, binaries and arbitrary
environment/command paths are not expanded into payload. Historical context JSON still contains
its original consumed one-shot gate value; it is preserved as exact evidence, not a new credential.

Each required file is independently copied and hash-checked. The deterministic ZIP includes
`reviewed-proof-inventory.json` describing every payload hash, reference edge, metadata-only
observation and opaque receipt. Every archived member is reread and checked. Only then does the
external complete manifest record whole-ZIP and embedded-inventory hashes. Checksums authenticate
the pinned recorded bytes; they do not establish reviewer identity or rerun neural inference.

## Actual package

Root independently passed all15 tiny fixture groups and the real read-only graph check, then
built and verified a local package at
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/proof-r1`.

- Required graph:110 files,218,622,339 bytes;14 metadata-only observations and28 opaque receipts.
- ZIP:111 members including its inventory;157,905,338 bytes, SHA-256
  `21105deacc97ad2eb49b8d165c43c20d3b3a689df053a9c9a6591361bbf919bc`.
- External complete manifest SHA-256:
  `143b02627bd57b5b81e63f3b1c7d00b5d39651771e336e644368e5efef1ae75e`.
- Explicit archive-upload plan `upload-assets.json` SHA-256:
  `47675b97cacbb184d4e41138130a50d196c69da632e9395fc1da3ef3e7859606`.

No full playable-content snapshot, content bundle or new public release is implied by this package.
The original sealed development-release plan remains unchanged.

## Publisher support

`publish_portable.py --auxiliary-plan <path>` accepts an opt-in JSON object with format
`mc3-release-auxiliary-assets-v1` and explicit archive records containing `name`, `path`, `size`
and lowercase `sha256`. Paths are relative to the plan's own directory. It registers the exact
validated plan in an exclusive immutable control receipt and includes approved archives in
`SHA256SUMS.txt`. Omitting/changing a registered plan on resume rejects the operation.

Local bytes are rechecked even when a matching remote archive would otherwise be skipped.
Case/control/runtime collisions, traversal, linked sources, changed bytes and foreign/stale
remote assets reject upload. An empty failed upload can be retried only with the matching owned
attempt and exact size/hash anchors. The tool still operates on owned draft releases and has no
publish action. Root independently passed all21 mocked-network tests; no actual GitHub draft,
upload, deletion or publication occurred during this implementation.
