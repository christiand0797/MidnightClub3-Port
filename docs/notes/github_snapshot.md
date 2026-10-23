# Full private GitHub project checkpoint

Updated 2026-10-23. The full checkpoint is copied and SHA-256 verified: 38,043 files,
28,281,848,318 bytes. Archive/upload is running from the frozen verified copy.
State: `G:/MC3_Backups/github_checkpoint_2026-10-23.control/state.json`.

## Destination and scope

The source repository `christiand0797/MidnightClub3-Port` is now public at the user's request.
Full checkpoints containing personal saves/configuration use the separate private companion
`christiand0797/MidnightClub3-Port-Backups`, seeded with main `8bc7f42`.
The publisher defaults to that private companion and verifies its visibility before every upload/publication.
The original upstream remote remains available; the public source repository is the `github` remote.

The user's request includes the entire project, not just Git-tracked source.
The checkpoint includes `.git`, the original ISO in the project folder,
extracted assets, native outputs, texture/video replacements, model/tools,
documentation, saves and ignored local project files. It copies only the named
project tree; no unrelated profile/configuration directory is read or included.
GitHub's automatically generated source ZIP cannot represent this full scope.

The read-only plan observed 37,620 regular files totaling 28,286,107,947 bytes,
without reparse points. G: had 584,112,930,816 free bytes. The tool reserves room
for one full copy, an approximately uncompressed-sized archive, and 2 GiB scratch
(approximately 58.7 GB for this inventory). Counts can change before execution.

This checkpoint is separate from the later **post-media, pre-PBR full backup**.
The movie pass still has roughly 48 GPU hours remaining at its inherited estimate.
This GitHub checkpoint must not be used to claim the later media/PBR gate passed.
The manifest explicitly records `is_post_media_pbr_gate: false`.

## Tool and phases

`tools/maintenance/snapshot_publish.py` defaults to a read-only dry-run. Copying
requires `--execute --checkpoint-confirmed`. The parent supplies that flag only
after the game and media workers are stopped at a clean checkpoint and source
agents are quiescent. No user reconfirmation is required; this enforces the
already-agreed checkpoint sequencing.

The tool creates a unique project copy directly under `G:/MC3_Backups`. Its
state, source inventory, manifest, logs and split archives are stored in a
separate external control directory. They are not written inside the frozen
project copy, avoiding self-referential manifests and changing archive inputs.

The sequence is:

1. Verify source/backup/control paths are disjoint and the destination repository
   is still private. Check free space and required tools. Refuse reparse points
   rather than silently following data outside the project or claiming omitted
   link targets are a complete backup.
2. Copy with `robocopy /E /COPY:DAT /DCOPY:DAT /XJ /R:1 /W:1 /MT:8` and logging.
   `/MIR`, `/PURGE`, `/MOVE` and deletion are not used. Robocopy codes 0–7 are
   successful copy outcomes; codes 8+ stop the pipeline.
3. Inventory and SHA256 every copied regular file. Verify the copy's inventory
   did not change while hashing. Preserve relative paths, byte sizes and SHA256
   values for every file, and every directory including empty ones.
4. Independently compare live source contents with the copy. Record changes to
   `.local/` and native logs as volatile differences. Any difference outside
   those paths blocks archiving/upload. A changed source file is not reported
   as equal merely because it was copied successfully earlier. The manifest
   distinguishes copied-file verification from live-source comparison; this is
   not a simultaneous VSS filesystem snapshot.
5. After `phase: verified`, archive the frozen copy with 7-Zip, including its
   root folder and `.git`, to volumes no larger than 1,900 MiB (1,992,294,400 bytes).
   Use `7z t` to test the whole split archive and independently compare the
   archive's complete file/size and directory listings to the manifest. Hash
   every archive part and write `SHA256SUMS.txt`.
6. Create an owned **draft** release using the snapshot's Git HEAD. Upload each
   split archive volume, the file manifest and checksum file. Reuse completed
   matching assets on resume. Verify remote names, states and byte sizes, and
   SHA256 digests when returned by GitHub. Refuse an unexpected or mismatching
   existing asset instead of using `--clobber`.
7. Reconfirm private visibility and all expected assets, then publish the release.
   Store the URL, asset IDs, sizes, local hashes, available remote digests and
   publication result in the external state. `--draft-only` leaves the complete
   upload unpublished if the parent intentionally requests that staging mode.

The game, source writers and media jobs should remain quiescent through
`phase: verified`, not just `phase: copied`, because live-source comparison is
still running between those phases. After verification, archive/upload reads
only the frozen snapshot and control files, so normal work can resume.

## Commands

Run from `C:/Users/purpl/Desktop/MidnightClub3-Port` with Python 3.12:

```powershell
$SnapshotPython = 'C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe'
$SnapshotState = 'G:\MC3_Backups\github_checkpoint_2026-10-22.control\state.json'
```

Read-only plan (no directories, snapshot or release are created):

```powershell
& $SnapshotPython tools/maintenance/snapshot_publish.py --state $SnapshotState
```

After the parent confirms the stable checkpoint, execute and continue through
publication:

```powershell
& $SnapshotPython tools/maintenance/snapshot_publish.py --state $SnapshotState --execute --checkpoint-confirmed
```

An optional `--name MidnightClub3-Port_2026-10-22_github_UNIQUE_ID` sets an explicit
directory name; otherwise the tool uses local date/time and a random suffix.
The target must be new. The state binds the chosen source, snapshot, repository
and tag; resume uses those saved identities rather than choosing a new snapshot.

Resume an interrupted operation:

```powershell
& $SnapshotPython tools/maintenance/snapshot_publish.py --state $SnapshotState --resume --execute --checkpoint-confirmed
```

The checkpoint flag is only needed while copying is incomplete. Once a frozen
copy is verified, archive/upload can resume with `--resume --execute` while the
source project continues changing. `--stop-after verify` allows the parent to
resume game/media work after seeing the verified checkpoint, then resume the
external archive/upload separately. Other bounded phases are `copy`, `archive`
and `upload`; stopping after upload keeps the release in draft form.

Exit 0 means the release is published (or the read-only plan succeeded).
Exit 2 means an intentional phase stop or completed draft-only upload. Exit 1
means a tool, verification, visibility, path, worker-lock or upload failure.

An OS-held state lock prevents two publisher instances from racing the same
state. A crashed process releases it automatically. An interrupted Robocopy can
resume into the same incomplete copy; it still needs another stable checkpoint.
Do not resume source edits/commits during that copy/verify interval.

An incomplete multi-volume archive is retained as evidence; a fresh archive
attempt gets a new external attempt directory because split archives cannot
be safely updated in place. Already-verified assets resume per whole volume,
not per byte inside a failed upload. GitHub can leave a zero-byte `starter`
asset after a failed upload; only such an owned empty draft asset is removed on
resume. A previously uploaded valid asset is never deleted.

## Restore and verification

From the private release, download all `.7z.NNN` parts plus
`snapshot-manifest.json` and `SHA256SUMS.txt` to one directory. Verify each listed
SHA256, then run `7z t NAME.7z.001` and extract with `7z x NAME.7z.001`.
All numbered volumes are required. The archive restores the full snapshot root
folder, including the original `.git` metadata, ignored files and empty folders.
The manifest lists every restored file for independent size/SHA256 verification.

The release notes distinguish this checkpoint from the final post-media backup
and from completed PBR work. Source `main` and the immutable checkpoint archive
can diverge as continued work is committed; the release records its snapshot ID
and Git HEAD so that distinction stays explicit.

## Evidence and references

Prepared tool validation used an isolated small fixture, not the live project.
It tested hidden files, `.git/config`, a filename containing spaces, an empty
directory and volatile state. Actual 7-Zip created/tested a split archive; its
complete listing matched the fixture manifest. SHA256 source comparison caught
a stable-file modification and recorded an allowed volatile modification.
Remote asset comparison rejected a wrong digest and recursive backup paths were
rejected. Python compilation passed. No cloud mutation was used for these tests.

GitHub documents that [each release asset must be under 2 GiB](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases),
which is why the archive uses 1,900 MiB volumes. The CLI's
[draft creation](https://cli.github.com/manual/gh_release_create),
[asset upload](https://cli.github.com/manual/gh_release_upload), and
[draft publication](https://cli.github.com/manual/gh_release_edit) commands
support the staged release process. The upload documentation states that
`--clobber` deletes existing assets first; this tool does not use it.
The installed 7-Zip 26.04 help confirms the `-v{Size}b` volume switch and
`t` integrity-test command used here.
