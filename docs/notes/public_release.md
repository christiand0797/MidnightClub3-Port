# Public portable release and upload maintenance

## Latest runtime — v0.3.2-dev.20261025

[v0.3.2-dev.20261025](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.2-dev.20261025)
is public as of2026-10-25 12:05MDT, prerelease ID423384119. Its exact source/tag is
`08bcab9687e55db8245e149d39fd92db78944dff`; the native executable SHA-256 is
`7bb7fcd1928156db88e33c78cd5a799ed9b04d4c9c19c9a9bbd6b00e2aa9d1ad`.
The [portable Windows ZIP](https://github.com/christiand0797/MidnightClub3-Port/releases/download/v0.3.2-dev.20261025/MidnightClub3-Native-0.3.2-dev.20261025-win64.zip)
is9703711bytes, SHA-256 `0a4fb2fa3fb2f22ff46c35e658ec31a85781038578eae2259f397e0982050ed7`.

Both remote assets (ZIP and checksum sidecar) matched their approved sizes/digests before and after publication.
An unauthenticated HTTPS ZIP download returned200 and matched the exact9703711bytes/SHA-256. All19 package
entries, dependencies, source/EXE identity and fixed content-manifest metadata passed verification. A fresh
headless archive checker passed synthetic split-content interoperability and damaged-chunk rejection;28/28
native CTests passed. Existing v0.3.0/v0.3.1 release assets (15 total), both bodies and both tags remain unchanged.

This build ships bead fitting across queued draw boundaries, centered tire-width growth, the affected lowered
ride-step tuck correction, gentler garage navigation and original-XDK vertex-constant parity. Bounded Golf
native checks cover both axle profiles30%/97.5%,755mm widths, save/reload, ride steps6–8 and street retention
of step8. Later private native observation shows an articulated avoidance jump. Close-up walking/foot contact,
all-car/extreme-camber support and actual scraping/sparks remain separate checks; the release does not claim
every NPC or vehicle is certified.

First launch still downloads the immutable v0.3.0 complete content using manifest SHA-256
`26595f26e83354cc18b5351b12811d1e9f0564855ae8114500213f37094e3bed`. The newer local texture/movie pass and
staged loading corrections are not shipped in these frozen content bytes. Runtime PBR is unchanged.

External publication/download/preservation evidence:
`G:/MC3_Backups/release-work/runtime-hotfix-0.3.2-dev.20261025-r1/control/`.
Use a new version for any subsequent package change; never replace these published assets.

## Earlier runtime — v0.3.1-dev.20261023

[v0.3.1-dev.20261023](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.1-dev.20261023)
was published at **2026-10-24 05:12:27 UTC**, public prerelease ID `422531680`. Its tag resolves to exact native
source `3a0ed6832c9a17938cbb3551c606e309334b0a4f`. The
[portable Windows ZIP](https://github.com/christiand0797/MidnightClub3-Port/releases/download/v0.3.1-dev.20261023/MidnightClub3-Native-0.3.1-dev.20261023-win64.zip)
is **9,696,068 bytes**, SHA-256 `aecf34e4c56dacf3ff370b7c3ed4f18eeb0519f4f532825401877029fefc46e2`.
Its EXE SHA-256 is `b3b6bd221d22055860de276447911be25fcda81f3ef60039102145f3bd0a14b2`.

Only the runtime ZIP and its checksum sidecar were added. Both remote sizes/digests matched the frozen plan
before and after publication, and an unauthenticated public ZIP download returned HTTP 200 with that exact
size/hash. All 19 ZIP entries/dependencies/content pins passed packaging checks. The actual native archive
reader passed the four-file/three-chunk fixture and damaged-chunk rejection; both content-enabled and own-disc
package variants passed. See [native vehicle/startup QA](native_hotfix_20261023.md) for the separate runtime scope.

First launch automatically fetches the unchanged v0.3.0 full-content manifest, pinned SHA-256
`26595f26e83354cc18b5351b12811d1e9f0564855ae8114500213f37094e3bed`, and its authenticated eight chunks.
This reuses the previously verified complete playable data. The freshly completed local texture pass and still
running detailed movie pass are not retroactively included in those frozen content bytes. v0.3.0's 13 asset
IDs/sizes/digests, original tag and release-body remainder were compared unchanged; its known-issue prefix now
links this hotfix.

External evidence is in `G:\MC3_Backups\release-work\runtime-hotfix-0.3.1-dev.20261023-r1\control`, including
`publication-verification.json`, `public-download-verification.json`, `previous-release-preservation.json`,
`native-qa-handoff.json` and frozen `release-notes.md`. The state records `publication_status: published`.
Prepare a new version for subsequent changes; do not replace these assets or resume an uploader against them.

## Frozen v0.3.0 content release

[v0.3.0-dev.20261023](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.0-dev.20261023)
was published as a public prerelease at **2026-10-23 09:09:17 UTC**, release ID `421585200`, with tag target
`1428df367ded80796032fc0f6b208d5b356b278f`. All **13 assets** had their remote sizes and SHA-256 digests checked
before and after publication/tag creation. The durable control state records `publication_status: published`.

The [Windows runtime ZIP](https://github.com/christiand0797/MidnightClub3-Port/releases/download/v0.3.0-dev.20261023/MidnightClub3-Native-0.3.0-dev.20261023-win64.zip)
is **9,688,808 bytes**, SHA-256 `c18c94213c37a76aae25b90856d427e60872a7b21c93fc11798ce9ecaccb80c3`.
Its native `mc3.exe` has SHA-256 `72ade3b85ea2292c57867f5ae6d635a20304d83d9830742c1c96b85eca12c5ab`.
The release also provides the pinned [content manifest](https://github.com/christiand0797/MidnightClub3-Port/releases/download/v0.3.0-dev.20261023/content-manifest.tsv),
eight `.mc3c` parts, diagnostic per-file inventory, ZIP sidecar and
[SHA256SUMS.txt](https://github.com/christiand0797/MidnightClub3-Port/releases/download/v0.3.0-dev.20261023/SHA256SUMS.txt).

After publication, the actual native WinHTTP downloader fetched the 1,074-byte manifest and verified its pinned
SHA-256. An unauthenticated download of the runtime ZIP matched the size/hash above. All eight public part URLs
returned HTTP 200 with their expected byte lengths. The full 14.1 GB content download was not repeated over the
network; its complete local parts, archive and 7,188 files had already passed authentication and actual clean
offline first-launch installation. See [release validation](release_validation.md) for the separate scopes.

This published tag is complete. Do not resume its uploader or replace its assets; the helper refuses published
releases. Prepare a new version and a new owned draft for subsequent runtime/content changes. The maintenance
workflow below describes draft staging and the earlier preparation history, not pending work for this tag.

## Draft upload helper

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

## Historical content-only draft (superseded)

The first draft targeted source `4d95d224e46f26ed10bc9306a65ee7d86468fb26` in `christiand0797/MidnightClub3-Port`, tag `v0.3.0-dev.20261023`. Its initial bootstrap ZIP was withheld for vehicle-control/tire-profile QA and excluded from that content-only plan. After the corrected runtime passed QA, the owned draft was updated to the final source and runtime identified above, then published. The unchanged content was verified using the native reader: 7,188 files, eight parts, 14,124,869,766 archive bytes; 7,081 PNGs and 106 MP4s accompany the ISO.

For a **future, unpublished** version, use a new tag, reviewed source commit and separate control/content directories. From the repository root, the following template creates or resumes only that draft's content assets, metadata and content-only checksum list:

```powershell
python .\tools\release\publish_portable.py `
    --tag '<new-version-tag>' `
    --target '<reviewed full source commit>' `
    --content '<new immutable content directory>' `
    --control '<new external control directory>' `
    --notes '<reviewed release-notes.md>' `
    --workers 2 --upload
```

Omit `--upload` for a local plan and read-only repository/commit check. The helper first confirms that the exact destination is public, that the account can push there, and that the full approved target commit exists. The asset plan is limited to manifest-listed `.mc3c` parts, the two content TSV files, optional explicitly approved runtime ZIP/sidecar, and `SHA256SUMS.txt`. It does not enumerate a whole build or backup directory.

The content manifest is 1,074 bytes, SHA-256 `26595f26e83354cc18b5351b12811d1e9f0564855ae8114500213f37094e3bed`. The archive SHA-256 is `3b3f311f7800430fd0b445952f2a8fffe86f58276f639bbb850e006d0848c1ab`.

## Add an approved runtime to a future owned draft

For a future release, finish runtime QA and the previous helper process first. Build/package the approved runtime, provide its exact reviewed ZIP SHA-256, and update `release-notes.md` to describe the checked behavior and remaining limitations. Then repeat that future draft's command with its approved final source commit and these additional options:

```powershell
--runtime-zip '<absolute path to the approved MidnightClub3-Native-<version>-win64.zip>' `
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

The tests make no GitHub calls. The published source at `1428df367ded80796032fc0f6b208d5b356b278f` passed **7/7** publisher fixtures. Subsequent maintainer-tool hardening passes **9/9**, adding fixed-tag preservation during draft PATCH and retries for atomic state replacement on Windows. Those later changes affect the uploader only; they do not change the published runtime or package. Coverage also includes published/unowned release refusal, remote size-plus-digest matching, mismatched/older-empty-asset refusal, concurrent control-directory locking and skipping completed matching assets.
