<#
.SYNOPSIS
Packages a synthetic local runtime fixture with -NoEnhance and checks updater delivery/manifest coverage.
.DESCRIPTION
No game is launched, no network request is made and no real release is published. The tiny PE headers below
exist only to exercise the packager's parser. They contain no executable code and are not playable binaries.
Test artifacts are retained in a new exclusive output directory.
#>
[CmdletBinding()]
param([string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('.local/update-packaging-tests-' + [Guid]::NewGuid().ToString('N'))
}
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Choose a new exclusive test output directory.' }
[void][IO.Directory]::CreateDirectory($root)
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$checks = 0
function Check-Packaging([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Write-FixtureText([string]$Relative, [string]$Text) {
    $path = Join-Path $root $Relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $Text, $utf8)
    return $path
}
function Write-FixturePE([string]$Relative, [bool]$Dll) {
    $bytes = [byte[]]::new(512)
    $stream = [IO.MemoryStream]::new($bytes, $true)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0x5A4D)
        $stream.Position = 0x3C; $writer.Write([uint32]0x80)
        $stream.Position = 0x80; $writer.Write([uint32]0x00004550)
        $writer.Write([uint16]0x8664); $writer.Write([uint16]0) # AMD64, no sections/imports
        $stream.Position = 0x94; $writer.Write([uint16]240)
        $writer.Write([uint16]$(if ($Dll) { 0x2022 } else { 0x0022 }))
        $writer.Write([uint16]0x020B)
        $writer.Flush()
    } finally { $writer.Dispose() }
    $path = Join-Path $root $Relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllBytes($path, $bytes)
    return $path
}
$exe = Write-FixturePE 'input/mc3.exe' $false
[void](Write-FixturePE 'crt/vcruntime140.dll' $true)
foreach ($relative in @('xbe_recomp.exe', 'clang.exe', 'lld-link.exe', 'mc3.lib', 'config/game.toml')) {
    [void](Write-FixtureText ('input/compiler/' + $relative) ('synthetic compiler fixture: ' + $relative))
}
[void](Write-FixtureText 'input/compiler/work/foreign.bin' 'not distributed')
[void](Write-FixtureText 'input/compiler/private.pdb' 'not distributed')
$personal = @(
    (Write-FixtureText 'input/settings.toml' 'personal preferences'),
    (Write-FixtureText 'input/hdd7/save.bin' 'personal career'),
    (Write-FixtureText 'input/assets/default.xbe' 'original content'),
    (Write-FixtureText 'input/textures/pbr/selected_n.png' 'generated maps'),
    (Write-FixtureText 'input/video_hd/story.mp4' 'generated media'),
    (Write-FixtureText 'input/mc3.dll' 'local recompiled code')
)
$before = @{}
foreach ($path in $personal) { $before[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
# This non-executable fixture represents a build newer than the source. Real packaging retains its existing
# source-age guard; no real EXE or source timestamps are modified by this test.
(Get-Item -LiteralPath $exe).LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(1)
$version = 'fixture-updater-' + [Guid]::NewGuid().ToString('N').Substring(0, 12)
$release = & (Join-Path $PSScriptRoot 'New-PortableRelease.ps1') -Executable $exe -CRTDirectory (Join-Path $root 'crt') `
    -Version $version -SourceRevision ('a' * 40) -OutputDirectory (Join-Path $root 'release') -NoEnhance
Check-Packaging ([bool]$release.Zip -and (Test-Path -LiteralPath $release.Zip)) 'Synthetic package was not produced.'
$stage = $release.UnpackedDirectory
Check-Packaging (-not (Test-Path -LiteralPath (Join-Path $stage 'enhance'))) '-NoEnhance package unexpectedly contains enhancement files.'
foreach ($relative in @('update/Update-MC3.ps1', 'update/README.md', 'Update MC3.cmd')) {
    Check-Packaging (Test-Path -LiteralPath (Join-Path $stage $relative) -PathType Leaf) "Updater file missing: $relative"
}
Check-Packaging ((Get-FileHash -LiteralPath (Join-Path $stage 'update/Update-MC3.ps1') -Algorithm SHA256).Hash -eq
    (Get-FileHash -LiteralPath (Join-Path $projectRoot 'tools/update/Update-MC3.ps1') -Algorithm SHA256).Hash) 'Packaged updater differs from source.'
$launcher = [IO.File]::ReadAllText((Join-Path $stage 'Update MC3.cmd'))
Check-Packaging ($launcher -match '-Mode Check\b' -and $launcher -notmatch '-Mode (Stage|Apply)\b' -and
    $launcher.Contains('-InstallDirectory "%~dp0."')) 'Launcher is not check-only with an explicit installation root.'
$start = [IO.File]::ReadAllText((Join-Path $stage 'START HERE.txt'))
Check-Packaging ($start.Contains('UPDATES') -and $start.Contains('Update MC3') -and $start.Contains('New source commits')) 'Start guide omits update/source distinction.'
$manifest = [IO.File]::ReadAllText((Join-Path $stage 'release-manifest.json'), $utf8) | ConvertFrom-Json
$paths = @($manifest.files | ForEach-Object { $_.path })
foreach ($relative in @('update/Update-MC3.ps1', 'update/README.md', 'Update MC3.cmd')) {
    $rows = @($manifest.files | Where-Object path -CEQ $relative)
    Check-Packaging ($rows.Count -eq 1 -and $rows[0].sha256 -eq
        (Get-FileHash -LiteralPath (Join-Path $stage $relative) -Algorithm SHA256).Hash.ToLowerInvariant()) "Updater manifest row missing or corrupt: $relative"
}
foreach ($forbidden in @('settings.toml', 'hdd7/save.bin', 'assets/default.xbe', 'textures/pbr/selected_n.png',
                         'video_hd/story.mp4', 'mc3.dll', 'compiler/work/foreign.bin', 'compiler/private.pdb')) {
    Check-Packaging ($forbidden -cnotin $paths -and -not (Test-Path -LiteralPath (Join-Path $stage $forbidden))) "Private/local input was packaged: $forbidden"
}
foreach ($path in $personal) {
    Check-Packaging ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $before[$path]) 'Packaging changed a personal fixture.'
}
$archive = [IO.Compression.ZipFile]::OpenRead($release.Zip)
try {
    Check-Packaging (@($archive.Entries | Where-Object { $_.FullName.Contains('\') }).Count -eq 0) 'ZIP contains Windows-only backslash paths.'
    foreach ($relative in @('update/Update-MC3.ps1', 'update/README.md', 'Update MC3.cmd')) {
        Check-Packaging (@($archive.Entries | Where-Object FullName -CEQ $relative).Count -eq 1) "ZIP omits updater file: $relative"
    }
} finally { $archive.Dispose() }
Check-Packaging ($release.SHA256 -eq (Get-FileHash -LiteralPath $release.Zip -Algorithm SHA256).Hash.ToLowerInvariant()) 'ZIP checksum differs.'
. (Join-Path $projectRoot 'tools/update/Update-MC3.ps1') -LibraryOnly
$verified = Read-Mc3Archive $release.Zip $release.SHA256 (Get-Item -LiteralPath $release.Zip).Length
Check-Packaging ($verified.manifest.files.Count -eq $manifest.files.Count) 'Actual updater rejected or changed the packaged manifest.'
[pscustomobject]@{ checks = $checks; output = $root; zip = $release.Zip; sha256 = $release.SHA256;
    downloads = 0; native_launched = $false; published = $false } | ConvertTo-Json -Depth 3
