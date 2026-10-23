# Portable release maintenance

Run these commands from the repository root in PowerShell 7 on Windows x64. The packaging scripts do not start the game. Use the current native output at `templates/game/out/native/mc3.exe`; the repository-root `mc3.exe` may be an older build.

## Verify the packager

Build the console archive checker from a Visual Studio x64 developer shell with LLVM available:

```powershell
New-Item -ItemType Directory -Force .\out\release-check | Out-Null
clang++ -std=c++20 -O2 -I.\runtime .\tools\release\Check-ContentArchive.cpp -luser32 -lwinhttp -lbcrypt -o .\out\release-check\Check-ContentArchive.exe
if ($LASTEXITCODE) { throw 'Archive checker build failed' }
```

The checker uses the native installer's actual manifest parser, chunk locking, archive reader, path validation and SHA-256 implementation. It reads content without creating a setup window, contacting HTTPS or installing files.

After building the native executable, run:

```powershell
$revision = (git rev-parse HEAD).Trim()
.\tools\release\Test-PortableRelease.ps1 `
    -Executable .\templates\game\out\native\mc3.exe `
    -NativeArchiveVerifier .\out\release-check\Check-ContentArchive.exe `
    -SourceRevision $revision
```

Every run creates a unique inspectable folder below `out/release-tests`; it does not recursively delete temporary files. Omit `-Executable` to check content writing alone. The fixtures are synthetic and must never be published as a game-content release.

The test writes four files into three 1 MiB parts. A 44-byte record header crosses the first part boundary; another file spans a later boundary. Empty media and a nested Unicode movie name exercise length and UTF-8 handling. Both an independent PowerShell reader and the native reader verify every part, the concatenated archive and every file against their recorded sizes and SHA-256 values. A damaged part must be rejected. Unsafe/save/settings targets and duplicate destinations must be rejected.

With `-Executable`, the test also builds and extracts a complete-content runtime ZIP. It checks every manifest entry, the exact input executable hash, dependency closure for every PE file, the pinned content descriptor and the ZIP checksum sidecar. The ZIP may contain only the declared files at its root; saves, settings, logs, crash dumps, game data and media must be absent. A second own-disc ZIP verifies that omitting the content descriptor leaves that flavor independent of a content release.

On 2026-10-23, the checks passed against the native executable built at 01:47:10 local time: 23,578,112 bytes, SHA-256 `ebc7508a81e43be90c4d248b24bb2e31f5ee8a582491670fb1e5ba176ce0de1f`. The initial interoperability fixture contained 2,097,777 archive bytes across three parts. This identifies the tested build; each subsequent release needs a fresh run.

## Prepare complete content

Freeze the ISO, active texture replacements and HD movie tree before packaging. The writer hashes each source, streams it, and rejects a size or modification-time change during the write. Do not point it at an active upscale output that is still changing.

For the `v0.3.0-dev.20261023` release, the frozen snapshot and intended output are:

```powershell
$version = '0.3.0-dev.20261023'
$tag = "v$version"
$snapshot = 'G:\MC3_Backups\MidnightClub3-Port_2026-10-23_004252_github_76e97c'
$releaseRoot = 'G:\MC3_Backups\release-work\complete-0.3.0-dev.20261023'
$disc = Join-Path $snapshot 'Midnight Club 3 - DUB Edition Remix (USA).xiso.iso'
.\tools\release\New-ContentBundle.ps1 `
    -DiscImage $disc `
    -TextureDirectory (Join-Path $snapshot 'templates/game/out/native/textures/replace') `
    -VideoDirectory (Join-Path $snapshot 'templates/game/out/native/video_hd') `
    -OutputDirectory $releaseRoot `
    -Version $version `
    -ChunkSizeMiB 1800
if ($LASTEXITCODE) { throw 'Content bundle preparation failed' }
.\out\release-check\Check-ContentArchive.exe $releaseRoot
if ($LASTEXITCODE) { throw 'Native archive verification failed' }
```

Adjust the snapshot/output locations for another release. Existing manifests or first parts cause packaging to stop; use a new version/output directory instead of mixing parts from separate runs. Read the returned object for the exact file count, part count, archive size and manifest SHA-256.

The bundle includes the supported ISO as `game/game.iso`, top-level active texture PNGs named with 16 hexadecimal characters, and recursive `.mp4` movies with their relative paths. The ISO's `default.xbe` SHA-1 must be `5bca1cf092edaadf876e8f80129900407a4a07b8`. The writer emits uncompressed `.mc3c` streaming parts, `content-manifest.tsv` and the diagnostic per-file `content-files.tsv`; these are unrelated to the source snapshot's split archives.

## Build the downloadable runtime ZIP

Build from the intended source revision, run the runtime checks, and pass that exact revision into the manifest. The timestamp check rejects native sources newer than the EXE; it does not establish that an arbitrary EXE was built from a particular commit.

```powershell
.\templates\game\build_mc3.ps1 -DiscImage $disc -Configuration RelWithDebInfo -Jobs 2
if ($LASTEXITCODE) { throw 'Native build failed' }
$revision = (git rev-parse HEAD).Trim()
$contentUrl = "https://github.com/christiand0797/MidnightClub3-Port/releases/download/$tag/content-manifest.tsv"
.\tools\release\New-PortableRelease.ps1 `
    -Executable .\templates\game\out\native\mc3.exe `
    -Version $version `
    -SourceRevision $revision `
    -OutputDirectory $releaseRoot `
    -ContentManifest (Join-Path $releaseRoot 'content-manifest.tsv') `
    -ContentManifestUrl $contentUrl
```

For an own-disc release, omit both `-ContentManifest` and `-ContentManifestUrl`. `-CRTDirectory` can explicitly select an installed x64 Visual C++ redistributable folder; otherwise the newest installed Visual Studio x64 CRT is selected. Every shipped binary is checked as AMD64 PE32+, and its imports must resolve to another shipped DLL or an explicit Windows 10/11 system dependency.

The output contains a staging directory, `MidnightClub3-Native-<version>-win64.zip`, and its `.sha256` sidecar. `release-manifest.json` records the version, declared source revision, supported disc hash, and every shipped file's size/SHA-256. The manifest does not list itself. The packaged files are the selected EXE, app-local CRT DLLs, licenses and startup instructions, plus the optional two content metadata files.

Publish the runtime ZIP, its checksum, `content-manifest.tsv` and every `.mc3c` part under the fixed release tag named in `content-source.txt`. The per-file inventory can also be attached for diagnostics. Retain a published content release when newer runtime ZIPs still reference it. Use a new content tag when payload bytes change.

## Limits of these checks

The synthetic test performs no game launch, GPU initialization, HTTPS request or download-resumption exercise. The native checker authenticates and parses the archive without installing it. Actual first-launch installation, cancellation/resume, offline repair and gameplay checks remain separate release checks. The complete payload must pass the native checker after its final part is written; a synthetic fixture result does not certify a partially written full bundle.

The installer currently authenticates the installed inventory on later launches, so verifying a large installation can take time even while offline. Initial installation also retains downloaded parts until files authenticate; allow about twice the installed content size in free space. Only one installer may run against a folder. Media playback depends on Windows Media Foundation, and Windows N editions need the Media Feature Pack.
