# Draft portable release uploads

`tools/release/publish_portable.py` stages release assets in an owned public-repository **draft prerelease**. It has no publish operation. GitHub CLI supplies authentication from its existing credential store; the helper never asks for or prints a token.

The GitHub connector available during this release had no release mutation/upload capability, so the helper uses `gh api` and `gh release upload`. GitHub exposes a server SHA-256 digest for uploaded assets; a matching name alone is insufficient to resume. The helper compares the remote size and digest, or streams an authenticated download when a server digest is unavailable. See [GitHub's release asset API](https://docs.github.com/en/rest/releases/assets) and [`gh release upload`](https://cli.github.com/manual/gh_release_upload).

## Durable control directory

Keep `G:\MC3_Backups\release-work\public-release-control` outside the checkout. It stores:

- `release-state.json`: repository/tag identity, unique ownership marker, release ID, expected assets and verified remote IDs/digests.
- `upload.log`: append-only progress and verification events.
- `release-notes.md`: the reviewed body, without the helper's ownership marker.
- `SHA256SUMS.txt`: hashes of the currently approved asset set, excluding the checksum file itself.
- `uploader.lock`: an OS byte lock that prevents two new helper processes from using the same state concurrently. It can remain on disk after completion.

The ownership marker is also embedded as an HTML comment in the draft body. The helper refuses an unrelated existing release, an existing unrelated tag, a published release, or an existing asset with unexpected bytes. There is no blanket `--clobber`. The only replacements supported are an identified empty failed upload from this uploader, and an explicitly requested update to its own previously verified `SHA256SUMS.txt` while still a draft.

Resume occurs at **asset boundaries**. An interrupted `.mc3c` upload may need to restart that part; completed matching assets are skipped. GitHub release uploads do not provide a partial-byte resume mechanism through this helper. Never start another helper invocation while the current upload is running.

## Current content-only draft

The first draft targets source `4d95d224e46f26ed10bc9306a65ee7d86468fb26` in `christiand0797/MidnightClub3-Port`, tag `v0.3.0-dev.20261023`. The original bootstrap ZIP is held for vehicle-control/tire-profile QA and is excluded from the upload plan. The unchanged content was verified using the native reader: 7,188 files, eight parts, 14,124,869,766 archive bytes; 7,081 PNGs and 106 MP4s accompany the ISO.

From the repository root, this command creates or resumes only the content assets, metadata and content-only checksum list:

```powershell
python .\tools\release\publish_portable.py `
    --tag v0.3.0-dev.20261023 `
    --target 4d95d224e46f26ed10bc9306a65ee7d86468fb26 `
    --content 'G:\MC3_Backups\release-work\complete-0.3.0-dev.20261023' `
    --control 'G:\MC3_Backups\release-work\public-release-control' `
    --notes 'G:\MC3_Backups\release-work\public-release-control\release-notes.md' `
    --workers 2 --upload
```

Omit `--upload` for a local plan and read-only repository/commit check. The helper first confirms that the exact destination is public, that the account can push there, and that the full approved target commit exists. The asset plan is limited to manifest-listed `.mc3c` parts, the two content TSV files, optional explicitly approved runtime ZIP/sidecar, and `SHA256SUMS.txt`. It does not enumerate a whole build or backup directory.

The content manifest is 1,074 bytes, SHA-256 `26595f26e83354cc18b5351b12811d1e9f0564855ae8114500213f37094e3bed`. The archive SHA-256 is `3b3f311f7800430fd0b445952f2a8fffe86f58276f639bbb850e006d0848c1ab`.

## Add the final approved runtime

Wait for final runtime QA and the previous helper process to finish. Rebuild/package the corrected runtime, provide its exact reviewed ZIP SHA-256, and update `release-notes.md` to describe the final checked behavior and remaining limitations. Then repeat the command with the approved final source commit and these additional options:

```powershell
--runtime-zip '<absolute path to the final MidnightClub3-Native-0.3.0-dev.20261023-win64.zip>' `
--runtime-sha256 '<approved 64-character lowercase ZIP SHA-256>' `
--update-owned-target --replace-owned-checksums
```

These placeholders must come from the final QA handoff. The helper validates the ZIP name/hash and exact `.zip.sha256` sidecar before including them. The update flags may modify only the owned draft's source target and its previously verified checksum list; existing content parts continue to be verified/skipped. A changed content part requires a new reviewed content release rather than an overwrite.

Final upload completion requires the remote asset set to exactly match the plan, all remote file sizes/digests to authenticate, and the release to remain an owned draft prerelease. A content-only run expects 11 assets; a complete runtime-plus-content run expects 13. The helper records `publication_status: held-for-final-QA` even after those checks pass. Final publication remains a separate action by the release owner after the QA hold is lifted.

Before publication, check the draft target, runtime ZIP hash, pinned content manifest, final instructions/troubleshooting, and body together. Keep the matching data release available because first-launch setup fetches assets from this fixed tag. A draft's HTML URL may contain an `untagged-...` placeholder until publication; `tag_name` in the REST response is the intended fixed tag. Unauthenticated clients cannot use draft asset URLs.

## Local safety checks

```powershell
python -m py_compile .\tools\release\publish_portable.py
python .\tools\release\test_publish_portable.py
```

The tests make no GitHub calls. They check published/unowned release refusal, remote size-plus-digest matching, mismatched/older-empty-asset refusal, concurrent control-directory locking and skipping completed matching assets.
