<#
.SYNOPSIS
Packages a freshly built native EXE, its app-local runtime and the compiler folder that builds the game code DLL from
the player's own disc on first launch (runtime/app/game_code.cpp). The recompiled game code (mc3.dll), game files,
saves, personal settings and media stay out.
.EXAMPLE
.\tools\release\New-PortableRelease.ps1 -Executable .\templates\game\out\native\mc3.exe -Version 0.2.0-dev.1 -SourceRevision abc1234
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$Version,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$SourceRevision,
    [string]$OutputDirectory,
    [string]$CRTDirectory,
    [string]$ContentManifest,
    [string]$ContentManifestUrl
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PortableRelease.psm1') -Force
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$exe = Get-Item -LiteralPath $Executable -ErrorAction Stop
if ($exe.Name -ne 'mc3.exe') { throw 'Pass the current native build output named mc3.exe.' }
$info = Get-PortablePEInfo -Path $exe.FullName
if ($info.IsDll) { throw 'Executable points to a DLL.' }
$nativeSources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'runtime'), (Join-Path $projectRoot 'templates/game/src') -Recurse -File |
    Where-Object Extension -In '.cpp', '.h')
$newer = @($nativeSources | Where-Object LastWriteTimeUtc -GT $exe.LastWriteTimeUtc)
if ($newer.Count) { throw "Native sources are newer than mc3.exe. Rebuild before packaging. First newer source: $($newer[0].Name)" }
$crt = Find-PortableCRT -Directory $CRTDirectory
$crtDlls = @(Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'out/releases' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($output)
$stem = "MidnightClub3-Native-$Version-win64"
$stage = Join-Path $output $stem
$zip = Join-Path $output ($stem + '.zip')
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) { throw "Release $Version already exists in $output. Choose a new version or output directory." }
[void][IO.Directory]::CreateDirectory($stage)
Copy-Item -LiteralPath $exe.FullName -Destination (Join-Path $stage 'mc3.exe')
foreach ($dll in $crtDlls) { Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $stage $dll.Name) }
# the compiler folder the build put beside mc3.exe: the recompiler, clang, lld, the game's configs, XDK layouts and the
# headers the generated code includes, plus mc3.lib (the executable's exports). Never the work folder or PDBs.
$compilerSource = Join-Path $exe.DirectoryName 'compiler'
foreach ($required in @('xbe_recomp.exe', 'clang.exe', 'lld-link.exe', 'mc3.lib', 'config/game.toml')) {
    if (-not (Test-Path -LiteralPath (Join-Path $compilerSource $required) -PathType Leaf)) { throw "The build's compiler folder is missing $required. Rebuild before packaging." }
}
$compilerStage = Join-Path $stage 'compiler'
Get-ChildItem -LiteralPath $compilerSource -Recurse -File | Where-Object {
    $_.Extension -ne '.pdb' -and $_.FullName.Substring($compilerSource.Length + 1) -notmatch '^work[\\/]'
} | ForEach-Object {
    $relative = $_.FullName.Substring($compilerSource.Length + 1)
    $target = Join-Path $compilerStage $relative
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    Copy-Item -LiteralPath $_.FullName -Destination $target
}
# xbe_recomp.exe needs the C++ runtime beside it
foreach ($dll in $crtDlls) { Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $compilerStage $dll.Name) }
if (Get-ChildItem -LiteralPath $stage -Recurse -File -Filter 'mc3.dll') { throw 'mc3.dll (recompiled game code) must not be packaged.' }
foreach ($name in @('LICENSE', 'THIRD_PARTY.txt')) { Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination (Join-Path $stage $name) }
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs/INSTALL.md') -Destination (Join-Path $stage 'INSTALL.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs/PLAYER_GUIDE.md') -Destination (Join-Path $stage 'PLAYER_GUIDE.md')
$utf8 = [Text.UTF8Encoding]::new($false)
if ([bool]$ContentManifest -ne [bool]$ContentManifestUrl) { throw 'Pass both -ContentManifest and its release-pinned -ContentManifestUrl for a complete-content download.' }
if ($ContentManifest) {
    if ($ContentManifestUrl -notmatch '^https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[A-Za-z0-9._-]+/content-manifest\.tsv$') { throw 'ContentManifestUrl must identify content-manifest.tsv in an HTTPS GitHub release with a fixed tag.' }
    $content = Get-Item -LiteralPath $ContentManifest
    if ($content.Length -gt 1MB) { throw 'Content manifest is too large.' }
    $contentHash = (Get-FileHash -LiteralPath $content.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    Copy-Item -LiteralPath $content.FullName -Destination (Join-Path $stage 'content-manifest.tsv')
    $descriptor = "MC3-CONTENT-SOURCE`t1`nurl`t$ContentManifestUrl`nsha256`t$contentHash`nsize`t$($content.Length)`n"
    [IO.File]::WriteAllText((Join-Path $stage 'content-source.txt'), $descriptor, $utf8)
}
$setupInstructions = if ($ContentManifest) {
    "3. First launch automatically downloads and verifies this release's complete game/media content.`n   Keep an internet connection available and enough free disk space for installation.`n   Offline: place all matching .mc3c parts next to mc3.exe before launching."
} else {
    "3. Select your own original Xbox USA Midnight Club 3 DUB Edition Remix ISO when asked.`n   Putting the matching ISO beside mc3.exe skips that dialog automatically."
}
[IO.File]::WriteAllText((Join-Path $stage 'START HERE.txt'), @"
Midnight Club 3: DUB Edition Remix - native Windows PC development build

1. Extract the ENTIRE ZIP to a writable folder, for example Games\MidnightClub3.
2. Double-click mc3.exe.
$setupInstructions

The game checks the disc, remembers its location, creates a desktop shortcut, and starts.
The first launch builds the game's code from your disc with the bundled compiler folder (a few minutes, once).
No emulator, Python, administrator rights, or separate setup app is needed.
Use F1 (or controller Back + Y) for PC graphics and tuning settings.
Keep every DLL and the compiler folder next to mc3.exe. Full instructions: INSTALL.md.
Controls, F1/F2 menus, customization and troubleshooting: PLAYER_GUIDE.md.

Version: $Version
Source revision: $SourceRevision
Saves and personal settings are never included. Full-content packages fetch separately checksummed release assets.
This is a development release; see the GitHub release notes for tested features and known limitations.
"@, $utf8)

$files = @(Get-ChildItem -LiteralPath $stage -File)
$allFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
foreach ($file in $files | Where-Object Extension -In '.exe', '.dll') {
    $binary = Get-PortablePEInfo -Path $file.FullName
    foreach ($dependency in $binary.Imports) {
        if (-not (Test-PortableWindowsDll $dependency) -and -not (Test-Path -LiteralPath (Join-Path $stage $dependency) -PathType Leaf)) {
            throw "Missing app-local dependency '$dependency' imported by $($file.Name). Packaging stopped."
        }
    }
}
$manifestFiles = @($allFiles | ForEach-Object { $_ | Add-Member -NotePropertyName Relative -NotePropertyValue ($_.FullName.Substring($stage.Length + 1) -replace '\\', '/') -PassThru } |
    Sort-Object Relative | ForEach-Object {
    [ordered]@{ path = $_.Relative; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$manifest = [ordered]@{
    format = 1
    name = 'MidnightClub3-Native'
    version = $Version
    source_revision = $SourceRevision.ToLowerInvariant()
    architecture = 'windows-x64'
    expected_xbe_sha1 = '5bca1cf092edaadf876e8f80129900407a4a07b8'
    files = $manifestFiles
}
[IO.File]::WriteAllText((Join-Path $stage 'release-manifest.json'), ($manifest | ConvertTo-Json -Depth 6), $utf8)
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$checksum = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($zip + '.sha256'), "$checksum  $([IO.Path]::GetFileName($zip))`n", $utf8)
[pscustomobject]@{ Version = $Version; Zip = $zip; SHA256 = $checksum; UnpackedDirectory = $stage; SourceRevision = $SourceRevision }
