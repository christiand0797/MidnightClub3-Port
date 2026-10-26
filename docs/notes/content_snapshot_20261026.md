# Completed corrected-content snapshot — 2026-10-26

The independent content snapshot completed at **19:01:20 UTC / 13:01:20 MDT**. Its exact
manifest is `G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/content-snapshot-r1/snapshot-manifest.json`,
SHA-256 `46505b3573e724391555413efd2e09b6574574a980d2208458baeed13672faf9`.
The completed byte freeze is preserved. Frozen-content decode/timing validation and later release
checks remain separate; this is not the post-media full-project backup or completion of all 106 strict movies.

## Frozen scope and provenance

The payload has exactly **7,188 files / 14,123,992,085 bytes**: one original ISO,7,081 active
replacement PNGs and 106 exact original-source-relative MP4s. There are no partial movies,
unrelated exports, personal settings, HDD/save files or runtime EXE in that payload.

The manifest inventory contains **7,391 files / 15,849,081,423 bytes**, excluding the manifest itself:

| Category | Files |
| --- | ---: |
| ISO + PNG + MP4 payload | 7,188 |
| Independent original Binks in `evidence/source-video/` | 106 |
| Original movie provenance JSON sidecars | 44 |
| Preserved model text markers | 49 |
| Reviewed texture ledger and texture audit | 2 |
| Current model-identity record | 1 |
| Snapshot-started receipt | 1 |

The 106 frozen movies comprise **44 strict original-source records, 5 historical marker-only
outputs and 57 historical unrecorded outputs**. The latter 62 are not authenticated new-pass x4plus
outputs. Missing sidecars are recorded as absence; no metadata adoption, retiming or inference was
performed by the freezer. Its `decode_and_full_timing_validation` remains explicitly `PENDING`.

The accepted San Diego PNG hashes are present in the frozen payload:

- `4D145117D9DD0F63.png`: `e028c07f530d6037e1b276323793e297eb7e28942f9d28a99923bc4bebbcd576`.
- `A21582D8BE650E3F.png`: `38dbda2b1ec675049720151c5aa61d493cd0193bbf188411259b9ff78d1f43ff`.

The snapshot records source revision `0afc7739377f841473a9ca9892600568fc301e22` and freezer SHA-256
`e2bf896e16e3524db8830d9297d9c6910a343ac18f2b8b348fde48e26d5f0c57`. Its frozen reviewed ledger
hash is `f8ed40b44fbdeabfb4586e630e795a6481fa63e28905293b7d51e6fcce08d9cc`; its texture-audit hash
is `eac9bf8a0184b229994a08e64dd5702bb6cfab2c4046369ffe0af274ab620296`. The complete copied model
identity is recorded separately, without relabeling historical movies.

The sealed earlier 39/67 [release plan](release_movie_selection.md) remains an immutable historical
plan. The new 44/62 cohort comes from the actual frozen files. Reviewed texture history, independent
base archives, accepted native evidence and their proof dependencies remain preserved; this snapshot
does not replace or repin that proof graph.

## Publication boundary and stopped-worker copy

The local one-shot helper watched the append-only movie log, then authenticated the 1497-frame
`story/sqmuscle.bik` publication against its source, exact model/recipe, output and report. The published
MP4 hash is `d8b3fb16aad2a7db2be0a0f928b3d1ea92851eccc214489ccbb888941a9c33ad`.
Original coordinator17636 and video child11568 were identified by their exact recorded process rows.
They exited cooperatively; no game/media process was killed.

| Event | UTC on 2026-10-26 | MDT |
| --- | --- | --- |
| Exact owned stop requested after publication | 18:31:12 | 12:31:12 |
| All observed workers drained; copy starting | 18:31:41 | 12:31:41 |
| Freezer's byte-copy/validation interval | 18:31:45–19:01:19 | 12:31:45–13:01:19 |
| Completed manifest receipted | 19:01:20 | 13:01:20 |
| Automatic stop retirement failed | 19:01:21 | 13:01:21 |
| First recovery retry refused before retirement | 22:25:46 | 16:25:46 |
| Same-volume retirement and independent evidence copy completed | 22:25:55 | 16:25:55 |
| One new coordinator started | 22:25:56 | 16:25:56 |
| New movie child started | 22:26:36 | 16:26:36 |

Quiescent execution held the coordinator, reviewed-install and exact video-output byte locks in
that order. It checked actual workers, the unchanged owned stop and user/transaction gates throughout
copying. Files were independently copied with source identities, two source reads, destination hashes,
stable sidecar cohorts and final inventory checks. The actual concurrent replacement probe remained
unsupported (`WinError5`); no concurrent-copy guarantee was substituted for the stopped-worker window.

`Svincint` had started after the published boundary and was stopped cooperatively. Its old-worker
log reports no completed-batch frame count before cancellation, so the amount of discarded in-flight
work is unreported. This was **not a proven zero-work-loss stop**. All 44 published strict outputs,
including the complete 1497-frame SqMuscle, were retained; only the owned in-flight temporary work
needed to restart.

## Retirement failure, outage and recovery

The automatic restart failed because `os.rename` attempted to move the C: stop directly to G:,
returning **WinError17**. The complete snapshot was unaffected. The exact owned stop remained at
its original C: path, SHA-256 `21c08a5b6efb6705e0b519a982a134c9b8434e4b26bc8a8048a6715faf0a269b`,
inode1407374884388090, one link. No foreign/user stop was cleared.

An account-usage outage delayed recovery from approximately **13:01 to 16:26 MDT**, about 3 h 25 m
after copying finished. Media remained stopped during that delay; this interval is not continuous
processing or necessary copy time. The planned copy itself took about 29 m 34 s.

The first recovery retry resolved `python` from PATH to Python 3.15 without Pillow. It failed while
importing the quiescent helpers, before flag retirement, and preserved the unchanged request. The
successful r2 recovery used explicit `C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe`;
its helper SHA-256 is `8cd6c9ed752d5ef09e86bbeb55017dc46929609cdb7be8e295d7fe35a97911f0`.

Under all three locks, r2 rechecked source/model/original/ledger/snapshot pins, exact stop bytes,
inode and payload, plus actual worker/user/transaction gates. It retired only that unchanged stop
to the new C: path `.local/snapshot-resume-recovery-20261026-r2/retired-owned-stop.json`, then made
a separate verified independent G: evidence copy. Both copies retain the exact stop SHA above;
the local retirement retains its inode and each file has one link. Final worker/marker checks
preceded starting coordinator **13372**. Child **18468** subsequently started the pending
`story/svincint.bik` again and logged 8/1603 frames. The coordinator's reviewed-texture startup
preflight therefore completed before its movie launch. Older checkpoint progress fields can lag
this restart; they do not change the frozen 44/5/57 cohort.

## Evidence and remaining checks

External immutable receipts are below
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/`:

- `snapshot-maintenance-r1/`: boundary, durable stop intent, owned-stop identity, completed snapshot
  and failed automatic restart phases. The completed phase pins the manifest hash above.
- `snapshot-resume-recovery-r1/failed-before-retirement.json`: Python 3.15/Pillow refusal.
- `snapshot-resume-recovery-r2/`: before/after retirement evidence, independent stop copy and
  `coordinator-started.json` (`c1538e657bf25a0d42fc97fe02798d2074f64b251200d4c842dc4895a4a1afb7`).
- `validation-r1/`: the separately running CPU frozen-content validator's progress/evidence.

The independent manifest review reconciled all 7,188 payload hash/size entries with the 7,391-file manifest
inventory and movie source/model/sidecar links, without rereading the large payload. All 25 freezer
fixtures passed before the real run. The separate frozen validator passed 23 root and 23 independent
fixtures and is now checking this actual cohort; its full decode/timing result was still pending at
this checkpoint. Fixture results do not certify every frozen movie.

Next: finish and review the immutable cohort's CPU file/decode/timing audit; then build and verify
content chunks/native archive reading, clean offline first-launch installation and exact source-to-EXE
release evidence. Preserve the snapshot, original plans, proof graph and all older published assets.
No new release, upload or native game session was performed by this snapshot/recovery. Runtime PBR is
unchanged. All106 strict media, final media audits and a newly verified full-project backup remain
mandatory separate gates before new offline PBR generation.
