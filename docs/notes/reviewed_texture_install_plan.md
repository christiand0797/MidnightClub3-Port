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
