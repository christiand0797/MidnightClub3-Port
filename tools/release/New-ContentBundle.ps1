<#
.SYNOPSIS
Builds a streaming, checksummed full game/media bundle in GitHub-release-sized chunks. Never includes saves or settings.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DiscImage,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$Version,
    [string]$TextureDirectory,
    [string]$VideoDirectory,
    [ValidateRange(1, 1900)][int]$ChunkSizeMiB = 1800
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
$disc = Get-Item -LiteralPath $DiscImage -ErrorAction Stop
if ($disc.PSIsContainer) { throw 'DiscImage must be an original Xbox ISO.' }

function Get-DiscXbeSha1([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        foreach ($partition in @([long]0, [long]0x18300000)) {
            $stream.Position = $partition + 32 * 2048
            $volume = $reader.ReadBytes(2048)
            if ($volume.Length -ne 2048 -or [Text.Encoding]::ASCII.GetString($volume, 0, 20) -ne 'MICROSOFT*XBOX*MEDIA' -or [Text.Encoding]::ASCII.GetString($volume, 0x7ec, 20) -ne 'MICROSOFT*XBOX*MEDIA') { continue }
            $rootOffset = $partition + [uint64][BitConverter]::ToUInt32($volume, 0x14) * 2048
            $rootSize = [BitConverter]::ToUInt32($volume, 0x18)
            if ($rootSize -gt 16MB -or $rootOffset + $rootSize -gt $stream.Length) { throw 'Invalid disc root directory.' }
            $stream.Position = $rootOffset
            $table = $reader.ReadBytes([int]$rootSize)
            $pending = [Collections.Generic.Stack[int]]::new()
            $seen = [Collections.Generic.HashSet[int]]::new()
            $pending.Push(0)
            while ($pending.Count) {
                $at = $pending.Pop()
                if ($at + 14 -gt $table.Length -or -not $seen.Add($at)) { continue }
                $left = [BitConverter]::ToUInt16($table, $at)
                $right = [BitConverter]::ToUInt16($table, $at + 2)
                if ($left -eq 0xffff) { continue }
                if ($left) { $pending.Push($left * 4) }
                if ($right) { $pending.Push($right * 4) }
                $length = $table[$at + 13]
                if ($at + 14 + $length -gt $table.Length) { throw 'Truncated disc directory entry.' }
                if ([Text.Encoding]::ASCII.GetString($table, $at + 14, $length) -ine 'default.xbe') { continue }
                $size = [BitConverter]::ToUInt32($table, $at + 8)
                $offset = $partition + [uint64][BitConverter]::ToUInt32($table, $at + 4) * 2048
                if ($table[$at + 12] -band 0x10 -or $size -gt 64MB -or $offset + $size -gt $stream.Length) { throw 'Invalid default.xbe in disc.' }
                $stream.Position = $offset
                $xbe = $reader.ReadBytes([int]$size)
                if ($xbe.Length -ne $size) { throw 'Truncated default.xbe in disc.' }
                $sha = [Security.Cryptography.SHA1]::Create()
                try { return ([BitConverter]::ToString($sha.ComputeHash($xbe))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose() }
            }
        }
        throw 'The ISO does not contain a readable original Xbox default.xbe.'
    } finally { $reader.Dispose() }
}

if ((Get-DiscXbeSha1 $disc.FullName) -ne '5bca1cf092edaadf876e8f80129900407a4a07b8') { throw 'The ISO is not the supported Midnight Club 3 DUB Edition Remix (USA) release.' }
$files = [Collections.Generic.List[object]]::new()
$files.Add([pscustomobject]@{ Source = $disc.FullName; Target = 'game/game.iso' })
if ($TextureDirectory) {
    $textureRoot = (Get-Item -LiteralPath $TextureDirectory).FullName
    foreach ($file in (Get-ChildItem -LiteralPath $textureRoot -File -Filter '*.png' | Sort-Object Name)) {
        if ($file.Name -notmatch '^[0-9a-fA-F]{16}\.png$' -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unexpected active texture filename: $($file.Name)" }
        $files.Add([pscustomobject]@{ Source = $file.FullName; Target = 'textures/replace/' + $file.Name })
    }
}
if ($VideoDirectory) {
    $videoRoot = (Get-Item -LiteralPath $VideoDirectory).FullName.TrimEnd('\', '/')
    foreach ($file in (Get-ChildItem -LiteralPath $videoRoot -File -Filter '*.mp4' -Recurse | Sort-Object FullName)) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked movie file cannot be bundled: $($file.Name)" }
        $relative = $file.FullName.Substring($videoRoot.Length + 1).Replace('\', '/')
        if ($relative -match '[\x00-\x1f:\\<>"|?*]' -or $relative.Split('/') -contains '..') { throw 'Unsafe movie path.' }
        $files.Add([pscustomobject]@{ Source = $file.FullName; Target = 'video_hd/' + $relative })
    }
}
Import-Module (Join-Path $PSScriptRoot 'PortableRelease.psm1') -Force
New-PortableContentArchive -Files $files.ToArray() -OutputDirectory $OutputDirectory -Version $Version -ChunkSizeMiB $ChunkSizeMiB
