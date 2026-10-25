# Journaled San Diego correction apply and recovery

`tools/texture_pack/apply_reviewed.py` is separate from the immutable dry-run planner. It implements a narrow
first adoption: exactly the two San Diego PNGs in an accepted plan, their independent old-byte archives, and
the exact proposed ledger. No canonical apply has been run during development of this tool. Existing planner
and r1/r2 candidate/proof bytes are not rewritten.

## Publication order and exclusion

Apply and recovery are Windows-only. They acquire the same operating-system byte lock as
`Coordinator.run()`, `.local/overnight_pipeline/live/coordinator.lock`, plus the independent
`.local/reviewed_texture_install/install.lock`. They refuse live games, texture/movie/inference workers and
the coordinator. They never kill a process, block physical input, clear a stop marker, or start inference.

Before any canonical mutation, apply atomically creates (without replacement)
`.local/reviewed_texture_install/active.json` from fully written, flushed and verified bytes. The marker pins a
random owner nonce, project root, own journal path and immutable intent SHA-256. These values and the exact
marker bytes' SHA-256 are recorded in the journal. An existing marker blocks a new apply. Recovery accepts an
absent marker or its exact own marker; a foreign/malformed/changed marker is preserved and refuses recovery.
If an explicit rollback starts after successful marker retirement, it first re-registers its own marker before
changing any canonical file, so interruption during that rollback remains gated too.

Apply reruns the immutable planner's complete proof/native-review checks and whole original-pack audit while
holding the locks. It snapshots the exact plan/ledger/old/new anchors into a new transaction journal with a
checksummed immutable intent. Every canonical mutation has an intent record flushed before it occurs.

1. Create both independent canonical old-byte archives. Each starts as an exclusive new same-directory copy,
   is flushed and SHA-256 checked, and is published without replacement permission. An existing archive is an
   error, even if its bytes happen to match.
2. Prepare independent candidate copies beside the two canonical PNGs. Recheck workers, all pinned proof
   dependencies, the exact old/current anchors and archives before replacing each directory entry. The tool
   never opens an existing canonical/QA PNG for writes; an old hardlinked QA copy keeps its original inode/bytes.
3. Publish the exact proposed ledger last, without overwriting any unexpected ledger.
4. Run the full real `reviewed_fixes.audit`. It must report the original eligible total (7,081 for the real
   project), exactly two verified reviewed corrections, exactly two accounted changed pass records, and all
   other current PNGs still matching their original named pass. Persist that report and mark the journal committed.

Every prepared archive/PNG/ledger/rollback temporary is rehashed and checked for a single independent link
immediately before its move, after mutation-boundary callbacks/checks. The earlier copy validation alone is not
sufficient. A changed or hardlinked temporary is retained as failed evidence and is never published as a PNG.

Files are flushed before same-volume Windows `MoveFileExW` directory-entry moves; no copy-across-volume or
reboot operation is requested. New archive/ledger moves omit replacement permission, while PNG/journal swaps
request replacement. This is a recoverable multi-file sequence, not a claim of one OS-wide atomic transaction
or certified hardware power-loss safety. The Windows flags follow the
[Microsoft API contract](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).
Uncooperative external edits cannot be made impossible by a process lock; exact anchors are checked at each
mutation boundary and any later unrelated bytes are preserved during recovery.

## Recovery semantics

An ordinary apply error attempts recovery while retaining both locks. A process interruption can leave an
intent without its completion event; recovery determines the actual state from the on-disk SHA-256 values.
Recovery itself journals its intentions and can resume after another interruption.

- Retire the canonical ledger into the transaction directory only when it still equals the exact newly
  installed ledger SHA-256. Preserve and report any unrelated ledger.
- Leave an already-original PNG alone. Restore old bytes only while the target still equals this transaction's
  installed candidate SHA-256. Preserve/report unexpected replacement bytes, deletion, or unsafe link paths.
- Restore through a new verified same-directory copy, preserving all original archives and candidate proofs.
  The verified canonical archive is preferred; a matching independent staged old-byte copy remains a fallback.
- If a game/media/coordinator starts, further canonical writes stop and the journal remains `recovery-pending`
  until the processes close. Nothing is killed or input-blocked.
- `rolled-back` is idempotent. `recovery-conflicts` identifies preserved external changes requiring review;
  it is not a successful complete rollback. Failed proof/journal/temp evidence is retained.

The own marker is retired by conditional SHA-256-checked move into the transaction directory only after a
committed installation or clean rollback. It stays active for pending/conflicted recovery. A crash after durable
`rolled-back` status but before retirement still permits resumed marker cleanup. A crash after `committed` but
before retirement can conservatively recover by conditional rollback. A foreign marker appearing during create
or cleanup is neither replaced nor removed.

The coordinator must not restart on an unresolved transaction. Its OS lock excludes a **live** apply process,
but a crashed process releases its lock. Parent integration now refuses any `active.json` presence, including a
malformed or foreign marker, before cached completion, any phase, GPU work, backup or PBR. Its 36 tests cover
that read-only gate without mutating the marker. Resolve the owning journal and marker before media scheduling
resumes; no timeout treats a foreign or stale marker as permission to continue.

## Tests and use

The 22 Windows temporary-fixture tests cover successful full mixed audit and explicit rollback; failure before
and after each PNG swap; failure after ledger publication; crash intent without completion; interruption during
rollback; both byte locks; a game starting at a mutation boundary; bad/pending proof and existing archive
refusal; failed post-apply audit; original QA hardlink preservation; tampered immutable intent; and unrelated
external PNG/ledger preservation; prepared-temporary byte/hardlink tampering; marker create and terminal-cleanup
crashes; and foreign marker races/preservation. No game, GPU, actual canonical pack or real stop marker is
changed by them.

```powershell
python3.12.exe -m unittest discover -s tools/texture_pack -p test_apply_reviewed.py -v
```

The parent owns final accepted native evidence and actual canonical execution. When that evidence is ready,
build a new accepted proposal with the existing planner, close the required processes, and use a **new**
transaction directory. The explicit CLI actions are `--apply <accepted-install-plan.json>
--transaction-directory <new-local-directory>` and `--recover <own-journal.json>`. No automatic apply, restart,
source-pass regeneration, backup/PBR bypass, or release publication is performed by this module.

Frozen source SHA-256 is `961679835dd7293037299d9b1844cc7e5ab87dc6f55651d8c6ee786542f59caf`; tests are
`539d01a383fdd67bc9471c21c2df7d706c8a94001c72215626cdefbc15619673`. The direct 22-test run passed in
75.072 s; independent review reran all 22 in 72.0 s and reproduced the former prepared-temporary failure case
against the fix, confirming both original PNGs survived and the own marker retired cleanly. No remaining
concrete scoped blocker was found. The private result record is `.local/apply-reviewed-tests-20261025-r1.json`.
It also confirms the actual San Diego canonical pair still matches its old anchors, with no canonical ledger
or real active transaction marker. The original planner remains
`911ed22cd6f7f8c5b84421348fe52cbe845ed256de4dda7700d1634797b80757`.
