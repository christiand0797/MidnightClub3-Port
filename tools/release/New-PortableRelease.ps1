<#
.SYNOPSIS
Packages a freshly built native EXE and its app-local runtime. Game files, saves, personal settings and media stay out.
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
foreach ($name in @('LICENSE', 'THIRD_PARTY.txt')) { Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination (Join-Path $stage $name) }
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs/INSTALL.md') -Destination (Join-Path $stage 'INSTALL.md')
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
No emulator, Python, compiler, administrator rights, or separate setup app is needed.
Use F1 (or controller Back + Y) for PC graphics and tuning settings.
Keep every DLL next to mc3.exe. Full instructions: INSTALL.md.

Version: $Version
Source revision: $SourceRevision
Saves and personal settings are never included. Full-content packages fetch separately checksummed release assets.
This is a development release; see the GitHub release notes for tested features and known limitations.
"@, $utf8)

$files = @(Get-ChildItem -LiteralPath $stage -File)
foreach ($file in $files | Where-Object Extension -In '.exe', '.dll') {
    $binary = Get-PortablePEInfo -Path $file.FullName
    foreach ($dependency in $binary.Imports) {
        if (-not (Test-PortableWindowsDll $dependency) -and -not (Test-Path -LiteralPath (Join-Path $stage $dependency) -PathType Leaf)) {
            throw "Missing app-local dependency '$dependency' imported by $($file.Name). Packaging stopped."
        }
    }
}
$manifestFiles = @($files | Sort-Object Name | ForEach-Object {
    [ordered]@{ path = $_.Name; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
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
