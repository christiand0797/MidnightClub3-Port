# San Diego correction install plan

`tools/texture_pack/install_reviewed.py` builds and rechecks a concrete **dry-run plan** for the two San Diego
garage-loading panels. It has no apply/install operation. A multi-file transaction and crash recovery are not
claimed or implemented; parent integration owns the final controlled installation.
The separate `apply_reviewed.py` now implements the tested journaled apply/recovery sequence described in
`reviewed_texture_transaction.md`. The planner itself and its pinned digest remain unchanged; no canonical
installation has been executed during this development.

The new proposal is `.local/reviewed-san-diego-proposal-20261025-r1/`. Its `proposed-reviewed-fixes.json` describes
the future canonical ledger, while `install-plan.json` pins that file and lists the exact current, candidate,
future archive, independent archive-source, before, after, and conditional rollback bindings. This proposed
ledger is deliberately outside the active texture directory and cannot be mistaken for an installed ledger.

The builder copies old PNG bytes to `base-archive/` using exclusive new-file creation. Each archive has one link,
matches the historical pass's output SHA-256, and is a separate file from canonical/QA PNGs. It never opens an
existing PNG for writes, replaces a directory entry, creates a canonical archive, changes a pass record, or
clears a worker stop request.

Every original/model/backend/inference/comparison dependency is pinned by normalized project-relative path and
SHA-256. The future canonical current/archive paths are evaluated through four explicit read-only projections:
two original-derived candidates and two newly copied old PNGs. All other paths resolve to their real files. The
existing reviewed-chain verifier recomputes the joined input and candidate split from the original sources;
accepted-review checks also use the existing `verify_entry` semantics. These checks do not disguise a projected
pack as an audit of the currently installed files.

The whole real original pack is also audited before readiness. The actual r1 run verified **7,081/7,081** named
pass outputs, preserving the historical 7,081-texture pass and all active PNGs. Its base source integration
commit is `08bcab9687e55db8245e149d39fd92db78944dff`; the new planner is identified separately by its own digest.

## Native evidence is pending

The current readiness helper also recognizes the installed `BlackOps3.exe` client, so planning/application
must wait for that game as well as MC3, Fortnite and media workers. The helper SHA-256 is now
`43163eb2e5476ad68da4008506faea77b6803be099c4bc8844731e5c8c168c48`.
The frozen r1 proposal remains a historical pending snapshot under its original helper hash; current checks
reject that stale helper pin. Build a new r2 with the current helper after actual accepted native evidence.
The 12 planner and 22 transaction tests pass, including actual inventory selection and application refusal
for the newly observed client. No canonical image, ledger or real transaction marker was changed.

The parent observed a cleaner continuous San Diego face and hand in prior private native QA, but those displayed
UI captures were not persisted. The r1 review truthfully remains `pending`; it does not fabricate before/after
capture paths or accepted evidence. Fresh native evidence must be handed off before a **new r2 proposal** is
built. The existing r1 files and original four-city candidate proofs remain unchanged.

The accepted native record must have this shape, with actual existing PNG paths and the exact per-panel hashes
from the proposed ledger:

```json
{
  "format": "mc3-native-texture-review-v1",
  "decision": "accepted",
  "reviewer": "Codex parent native QA",
  "before_capture": ".local/loading-native-ab-20261025/s-before.png",
  "after_capture": ".local/loading-native-ab-20261025/s-after.png",
  "entries": {
    "4D145117D9DD0F63.png": {
      "source_sha256": "<exact original source PNG SHA-256>",
      "before_sha256": "<exact historical active PNG SHA-256>",
      "after_sha256": "<exact joined CPU candidate PNG SHA-256>"
    },
    "A21582D8BE650E3F.png": {
      "source_sha256": "<exact original source PNG SHA-256>",
      "before_sha256": "<exact historical active PNG SHA-256>",
      "after_sha256": "<exact joined CPU candidate PNG SHA-256>"
    }
  }
}
```

The two capture paths must be distinct readable images, at least 32 pixels per dimension. Checksums authenticate
the recorded artifacts and review declaration, not the identity of the reviewer or the origin of an image.
Extra executable/source/timing/F3 metadata can be retained; any additional artifact intended as an adoption
dependency must also be explicitly pinned.

## Readiness and eventual installation

`--check <install-plan.json>` changes no files. It returns status 2 when evidence or worker readiness is pending.
Its `install_ready` value means that the **plan's evidence and idle-worker preconditions** passed at that instant;
it does not mean that this tool implements a transaction. A live MC3/Fortnite session, texture/movie/inference
worker, or durable coordinator blocks this precondition. The actual r1 snapshot correctly reports the live
coordinator/movie/GPU children and pending native evidence. No process is stopped by the tool.

The final controlled installer must acquire coordinator/install exclusion, reverify every dependency and exact
old anchor immediately before mutation, create and verify independent canonical base archives, prepare new
same-directory copies, and atomically replace directory entries without writing through hardlinks. Publish the
exact reviewed ledger last and audit the complete mixed pack. On failure, restore an old PNG only when the
current target still matches the newly installed candidate SHA-256; preserve and report unrelated changes.
Keep partial transaction evidence and archived old bytes for recovery. The current builder intentionally does
not claim that these transaction/recovery conditions have been proven.

Eleven focused tests cover pending/accepted projected evidence, the original full-pack prerequisite, exact old
anchors, live coordinator blocking, independent archives with a simulated QA hardlink, changed dependencies,
existing future archives/canonical ledger, path bounds/unexpected bindings, and preservation of unrelated
replacement bytes. They use temporary fixtures and run no inference:

```powershell
python3.12.exe -m unittest discover -s tools/texture_pack -p test_install_reviewed.py -v
```

## Pending native evidence checkpoint — 2026-10-25

The actual baseline file `.local/loading-native-ab-20261025/s-before.png` shows the San Diego loading portrait
at 1536×864. Its original SHA-256 is `21370866bebdb37337e1064fa91e0bd6b369212c607c449205db5da5916d82f2`.
Despite the filename, its payload is **JPEG/JFIF**, with lossy capture encoding; it is preserved byte-for-byte.
The planner accepts readable images and does not require a PNG payload, so no unnecessary transcode was made.

The separate `.local/loading-native-ab-20261025/baseline-evidence.json`, SHA-256
`60eaf1fd79f8ea1a010f8c9c34d4c91dc79eb4ae248eeb0e30c59bd06fe9fbc4`, records the image/decoder/file hashes,
preflight hash and 18:02:52 UTC context, exact variant-receipt line hashes, and the later 23:26:02 UTC candidate
switch. The preflight's original private pair, r9 executable, settings, window size and the image's filesystem
18:13:04 UTC timestamp support baseline context. Neither a mutable timestamp nor that receipt log proves a
per-frame texture binding; no embedded runtime metadata or detail recovery is claimed.

The read-only graph check still passes for r1, both staged archives have one link, and the canonical pair still
matches its old anchors. An after capture and an explicit accepted paired native review remain absent, so r1
stays pending and no r2 or canonical installation is justified yet. Root preserved the active human session and
deferred a new native launch. Existing images, plans, canonical files and stop markers were not changed by this
evidence audit.
