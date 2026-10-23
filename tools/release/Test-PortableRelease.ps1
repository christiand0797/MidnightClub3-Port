<#
.SYNOPSIS
Checks synthetic split-content interoperability and, optionally, an extracted portable ZIP. Never launches the game.
.EXAMPLE
.\tools\release\Test-PortableRelease.ps1 -Executable .\templates\game\out\native\mc3.exe -NativeArchiveVerifier .\out\release-check\Check-ContentArchive.exe
#>
[CmdletBinding()]
param(
    [string]$Executable,
    [string]$NativeArchiveVerifier,
    [string]$OutputDirectory,
    [ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$SourceRevision = '0000000'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PortableRelease.psm1') -Force
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$id = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot "out/release-tests/$id" }
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Test output already exists; pass a new directory.' }
[void][IO.Directory]::CreateDirectory($root)
$utf8 = [Text.UTF8Encoding]::new($false, $true)
function Assert-Release([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Hash-Bytes([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Expect-Rejection([scriptblock]$Operation, [string]$Message) {
    $rejected = $false
    try { & $Operation | Out-Null } catch { $rejected = $true }
    Assert-Release $rejected $Message
}
function Fixture-File([string]$Name, [int]$Size, [int]$Seed) {
    $path = Join-Path $root $Name
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    $data = [byte[]]::new($Size)
    [Random]::new($Seed).NextBytes($data)
    [IO.File]::WriteAllBytes($path, $data)
    return $path
}
# The first file leaves seven bytes in chunk 1, splitting the following 44-byte record header across chunks.
$isoSize = 1MB - 12 - 44 - $utf8.GetByteCount('game/game.iso') - 7
$files = @(
    [pscustomobject]@{ Source = (Fixture-File 'inputs/game fixture.iso' $isoSize 10); Target = 'game/game.iso' },
    [pscustomobject]@{ Source = (Fixture-File 'inputs/texture.png' (1MB + 97) 20); Target = 'textures/replace/0123456789abcdef.png' },
    [pscustomobject]@{ Source = (Fixture-File 'inputs/empty.mp4' 0 30); Target = 'video_hd/empty.mp4' },
    [pscustomobject]@{ Source = (Fixture-File 'inputs/unicode intro.mp4' 321 40); Target = 'video_hd/字幕/intro 1.mp4' }
)
$bundle = New-PortableContentArchive -Files $files -OutputDirectory (Join-Path $root 'content') -Version "fixture-$id" -ChunkSizeMiB 1
$lines = [IO.File]::ReadAllLines($bundle.Manifest, $utf8)
Assert-Release ($lines[0] -ceq "MC3-CONTENT`t1") 'Wrong manifest format/BOM.'
$archiveRow = $lines[1].Split("`t")
$stream = [IO.MemoryStream]::new()
try {
    $chunks = @($lines | Select-Object -Skip 2)
    Assert-Release ($chunks.Count -eq 3) 'Fixture did not cross two chunk boundaries.'
    for ($index = 0; $index -lt $chunks.Count; ++$index) {
        $row = $chunks[$index].Split("`t")
        Assert-Release ($row.Count -eq 4 -and $row[0] -ceq 'chunk') 'Invalid chunk record.'
        $part = Get-Item -LiteralPath (Join-Path (Split-Path -LiteralPath $bundle.Manifest) $row[1])
        Assert-Release ($part.Length -eq [long]$row[2] -and $part.Length -le 1MB) 'Wrong chunk size.'
        Assert-Release ((Get-FileHash -LiteralPath $part.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $row[3]) 'Wrong chunk SHA-256.'
        if ($index -lt $chunks.Count - 1) { Assert-Release ($part.Length -eq 1MB) 'An intermediate chunk was not full.' }
        $bytes = [IO.File]::ReadAllBytes($part.FullName)
        $stream.Write($bytes, 0, $bytes.Length)
    }
    $archive = $stream.ToArray()
    Assert-Release ($archive.Length -eq [long]$archiveRow[1] -and (Hash-Bytes $archive) -ceq $archiveRow[2]) 'Wrong archive length/SHA-256.'
    $stream.Position = 0
    $reader = [IO.BinaryReader]::new($stream, $utf8, $true)
    try {
        Assert-Release ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)) -ceq 'MC3C0001') 'Wrong native archive magic.'
        Assert-Release ($reader.ReadUInt32() -eq $files.Count) 'Wrong archive file count.'
        $inventory = [IO.File]::ReadAllLines($bundle.Inventory, $utf8)
        Assert-Release ($inventory[0] -ceq "MC3-CONTENT-FILES`t1" -and $inventory.Count -eq $files.Count + 1) 'Wrong packaging inventory.'
        for ($index = 0; $index -lt $files.Count; ++$index) {
            $pathLength = $reader.ReadUInt32()
            $size = $reader.ReadUInt64()
            $hash = ([BitConverter]::ToString($reader.ReadBytes(32))).Replace('-', '').ToLowerInvariant()
            $name = $utf8.GetString($reader.ReadBytes($pathLength))
            $data = $reader.ReadBytes([int]$size)
            $source = Get-Item -LiteralPath $files[$index].Source
            Assert-Release ($name -ceq $files[$index].Target -and $size -eq $source.Length) 'Archive record differs from source.'
            Assert-Release ((Hash-Bytes $data) -ceq $hash -and (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $hash) 'Wrong per-file SHA-256.'
            Assert-Release ($inventory[$index + 1] -ceq "$name`t$size`t$hash") 'Archive differs from packaging inventory.'
        }
        Assert-Release ($stream.Position -eq $stream.Length) 'Unexpected trailing archive data.'
    } finally { $reader.Dispose() }
} finally { $stream.Dispose() }
foreach ($target in @('hdd/player.save', 'config/settings.toml', 'video_hd/../escape.mp4')) {
    Expect-Rejection { New-PortableContentArchive -Files @([pscustomobject]@{ Source = $files[0].Source; Target = $target }) -OutputDirectory (Join-Path $root 'rejected') -Version 'rejected' } "Unsafe target accepted: $target"
}
Expect-Rejection { New-PortableContentArchive -Files @($files[0], $files[0]) -OutputDirectory (Join-Path $root 'duplicate') -Version 'duplicate' } 'Duplicate target accepted.'
if ($NativeArchiveVerifier) {
    & $NativeArchiveVerifier (Split-Path -LiteralPath $bundle.Manifest)
    Assert-Release ($LASTEXITCODE -eq 0) 'Native installer archive reader rejected the PowerShell bundle.'
    $corrupt = Join-Path $root 'corrupt-content'
    [void][IO.Directory]::CreateDirectory($corrupt)
    foreach ($file in (Get-ChildItem -LiteralPath (Split-Path -LiteralPath $bundle.Manifest) -File)) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $corrupt $file.Name)
    }
    $badPart = Join-Path $corrupt $chunks[0].Split("`t")[1]
    $badBytes = [IO.File]::ReadAllBytes($badPart)
    $badBytes[$badBytes.Length - 1] = $badBytes[$badBytes.Length - 1] -bxor 1
    [IO.File]::WriteAllBytes($badPart, $badBytes)
    $rejection = @(& $NativeArchiveVerifier $corrupt 2>&1)
    Assert-Release ($LASTEXITCODE -ne 0 -and ($rejection -join ' ') -match 'checksum failed') 'Native reader accepted a damaged chunk.'
}
$release = $null
if ($Executable) {
    $url = "https://github.com/christiand0797/MidnightClub3-Port/releases/download/fixture-$id/content-manifest.tsv"
    $release = & (Join-Path $PSScriptRoot 'New-PortableRelease.ps1') -Executable $Executable -Version "fixture-$id" -SourceRevision $SourceRevision -OutputDirectory (Join-Path $root 'release') -ContentManifest $bundle.Manifest -ContentManifestUrl $url
    $extracted = Join-Path $root 'extracted'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($release.Zip, $extracted)
    $manifest = [IO.File]::ReadAllText((Join-Path $extracted 'release-manifest.json'), $utf8) | ConvertFrom-Json
    Assert-Release ($manifest.source_revision -ceq $SourceRevision.ToLowerInvariant() -and $manifest.architecture -ceq 'windows-x64') 'Wrong release identity.'
    $expected = @($manifest.files.path) + 'release-manifest.json' | Sort-Object
    $actual = @(Get-ChildItem -LiteralPath $extracted -Recurse -File | ForEach-Object { $_.FullName.Substring($extracted.Length + 1).Replace('\', '/') } | Sort-Object)
    Assert-Release (($expected -join "`n") -ceq ($actual -join "`n")) 'ZIP contains unmanifested/missing files.'
    foreach ($file in $manifest.files) {
        Assert-Release ($file.path -notmatch '[/\\]' -and $file.path -notmatch '^(settings\.toml|hdd|config|logs|CrashDumps|game|textures|video_hd)$') 'Personal or game content leaked into runtime ZIP.'
        $path = Join-Path $extracted $file.path
        Assert-Release ((Get-Item -LiteralPath $path).Length -eq $file.size -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $file.sha256) "Release file checksum mismatch: $($file.path)"
        if ([IO.Path]::GetExtension($path) -in '.exe', '.dll') {
            foreach ($dependency in (Get-PortablePEInfo $path).Imports) {
                Assert-Release ((Test-PortableWindowsDll $dependency) -or (Test-Path -LiteralPath (Join-Path $extracted $dependency) -PathType Leaf)) "Unresolved extracted dependency: $dependency"
            }
        }
    }
    Assert-Release ((Get-FileHash -LiteralPath (Join-Path $extracted 'mc3.exe') -Algorithm SHA256).Hash -ceq (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash) 'The ZIP contains a different executable.'
    $descriptor = [IO.File]::ReadAllLines((Join-Path $extracted 'content-source.txt'), $utf8)
    Assert-Release ($descriptor[0] -ceq "MC3-CONTENT-SOURCE`t1" -and $descriptor[1] -ceq "url`t$url" -and $descriptor[2] -ceq "sha256`t$($bundle.ManifestSHA256)" -and $descriptor[3] -ceq "size`t$((Get-Item -LiteralPath $bundle.Manifest).Length)") 'Wrong pinned content descriptor.'
    Assert-Release ((Get-FileHash -LiteralPath (Join-Path $extracted 'content-manifest.tsv') -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $bundle.ManifestSHA256) 'Packaged content manifest differs from bundle.'
    $zipHash = (Get-FileHash -LiteralPath $release.Zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Release ([IO.File]::ReadAllText($release.Zip + '.sha256', $utf8) -ceq "$zipHash  $([IO.Path]::GetFileName($release.Zip))`n") 'Wrong ZIP checksum sidecar.'
    $ownDisc = & (Join-Path $PSScriptRoot 'New-PortableRelease.ps1') -Executable $Executable -Version "fixture-own-$id" -SourceRevision $SourceRevision -OutputDirectory (Join-Path $root 'release')
    $ownZip = [IO.Compression.ZipFile]::OpenRead($ownDisc.Zip)
    try {
        $ownNames = @($ownZip.Entries.FullName)
        Assert-Release ($ownNames -notcontains 'content-source.txt' -and $ownNames -notcontains 'content-manifest.tsv' -and $ownNames -contains 'mc3.exe') 'Own-disc ZIP unexpectedly requires a content release.'
    } finally { $ownZip.Dispose() }
}
# Keep the unique fixture folder for inspection; no recursive cleanup or game/UI/network activity is performed.
[pscustomobject]@{ Result = 'PASS'; OutputDirectory = $root; ContentFiles = $files.Count; Chunks = $bundle.Chunks; ArchiveBytes = $bundle.ArchiveBytes; NativeReader = [bool]$NativeArchiveVerifier; PortableZip = $(if ($release) { $release.Zip } else { $null }) }
