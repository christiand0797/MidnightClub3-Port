# Exact movie selection for content bundles

The content-bundle builder previously collected every recursive `*.mp4`. The live media folder
contained106 expected movies plus an old retained temporary output and the current worker's
temporary output. Both ended in `.part.mp4`; the old builder would have included them as playable
content. They are preserved. Their presence alone does not establish corrupt published movies.

`New-ContentBundle.ps1` now derives its exact106 case-unique movie targets from the original Bink
relative paths. `Get-PortableMovieFiles` selects only those expected generated MP4 names; temporary
and unrelated files remain untouched and unselected. Missing expected outputs, case collisions,
unsafe paths and linked selected inputs/ancestors reject the build. Existing positional CLI
arguments are preserved. `-SourceVideoDirectory` is appended, defaulting to the repository's
original `templates/game/assets/video` directory.

This is path/inventory validation, not a source-content, neural-quality or timing audit. It also
does not freeze live bytes. Release assembly must still use an independently copied, hashed
immutable content cohort, with each movie's actual provenance recorded honestly.

## Verification

Root and the implementing agent independently passed `Test-ContentMovieSelection.ps1`:
nine fixture groups cover106 tiny original/generated files, extra partial/unrelated files,
missing expected output, duplicate source names, simulated case-sensitive generated collisions,
unsafe names, Unicode names, and linked roots/root ancestors. The actual archive writer produced
a9,548-byte106-file fixture archive containing only the selected paths. Excluded files retained
their hashes. Test junctions were removed by their own nonrecursive directory entries; fixture
directories remain available for inspection.

The existing portable synthetic test also passed: four files, three chunks,2,097,785 archive bytes.
Root did not request the optional native C++ archive verifier during that run. No game, inference,
real-content archive, release upload or personal-data change was performed by these tests.

Root additionally selected the actual live inventory read-only:106 expected names from108 MP4s,
excluding `.rduc1000.cgh48vfo.part.mp4` and `.sqperfom.ff7hg1qm.part.mp4`. Both files were left in place.
The active movie worker continued normally.

## Corrected-content development release

Public v0.3.2 remains unchanged. A runtime-only r11 release pinned to its old frozen content would
still install the old San Diego panels: the installer authenticates every installed file and can
replace a manual overlay with its pinned original bytes. The next corrected-content release needs
a **new complete immutable content snapshot**, new manifest/chunks and a runtime ZIP pinned to its
fixed-tag manifest. It can contain39 strict new-pass movies plus67 historical usable movies if
that cohort and its actual validation are labeled accurately; it cannot claim106 strict new-pass
completion. The final full-media/backup/PBR gates remain separate.

Preserve the exact reviewed texture ledger, independent old archives and its project-relative
source/model/tool/composite/inference/split/review dependencies in a separately authenticated
proof package. Include transitive native receipts and raw photo metadata as well. The current
native content format carries ISO/PNG/MP4, so it does not automatically transport that proof tree.
The draft publisher also needs explicit, collision-safe auxiliary-proof asset support before that
separate archive can be published through it. Do not flatten or repin frozen history to hide gaps.

Before publishing, verify frozen files/chunks/whole archives, run the native archive reader and
clean offline first-launch checks, seal exact source-to-EXE/input hashes, and preserve every prior
release asset. The caller-supplied `SourceRevision` and mtime guard alone do not prove an EXE was
built from a tag. The native-tested r11 EXE is the appropriate candidate; canonical r9 remains
installed until the new release is ready.

A new local plan selects that complete corrected-content path at
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/release-plan.json`, SHA-256
`57e707ceb01b32e40081a0a35d0f252f50347b3c38ec9b9f18fa55c28df5fb52`.
It pins source `63af77d12f55e9496879a143b9a429cbf360d492`, the tested r11 candidate and actual
review/install receipts, verifies the65 direct ledger dependencies and records all106 expected
movie names. It explicitly marks the39/67 live cohort as unfrozen. It creates no release/archive
and requires fresh final source/input proof if subsequent source changes precede publication.
