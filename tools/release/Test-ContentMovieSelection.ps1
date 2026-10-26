<#
.SYNOPSIS
CPU-only exact original-to-HD movie selection fixtures. No game, inference, real media, network or large bundle.
#>
[CmdletBinding()]
param([string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'PortableRelease.psm1') -Force
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('.local/content-movie-selection-' + [Guid]::NewGuid().ToString('N'))
}
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $root) { throw 'Fixture output exists; choose a new directory.' }
[void][IO.Directory]::CreateDirectory($root)
$utf8 = [Text.UTF8Encoding]::new($false)
$checks = 0
function Assert-Movie([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-MovieRejection([scriptblock]$Operation, [string]$Expected) {
    $message = ''
    try { & $Operation | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Movie ($message -match $Expected) "Expected '$Expected' rejection, observed '$message'."
}
function Fixture-Movie([string]$Relative, [string]$Text) {
    $path = Join-Path $root $Relative
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path, $Text, $utf8)
    return $path
}
$original = Join-Path $root 'original'
$generated = Join-Path $root 'generated'
$expected = @(foreach ($i in 0..105) {
    $relative = 'story/clip{0:000}' -f $i
    [void](Fixture-Movie ("original/$relative.bik") "original fixture $i")
    [void](Fixture-Movie ("generated/$relative.mp4") "generated fixture $i")
    'video_hd/' + $relative + '.mp4'
})
$partial = Fixture-Movie 'generated/story/.clip001.owned.part.mp4' 'do not package this in-flight work'
$unrelated = Fixture-Movie 'generated/exports/unrelated.mp4' 'do not package this unrelated export'
$extraBefore = @{}
foreach ($path in @($partial, $unrelated)) { $extraBefore[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
$selection = @(Get-PortableMovieFiles -SourceVideoDirectory $original -VideoDirectory $generated)
Assert-Movie ($selection.Count -eq 106) 'Expected exact 106-source movie selection.'
Assert-Movie (($selection.Target -join "`n") -ceq ($expected -join "`n")) 'Selected paths differ from original Bink catalogue.'
Assert-Movie ($selection.Source -notcontains $partial -and $selection.Source -notcontains $unrelated) 'Temporary/unrelated MP4 selected.'
$checks++

# Exercise the actual archive writer with only tiny selected files, and inspect its complete per-file inventory.
$bundle = New-PortableContentArchive -Files $selection -OutputDirectory (Join-Path $root 'tiny-content') -Version 'movie-selection-fixture' -ChunkSizeMiB 1
$inventory = [IO.File]::ReadAllLines($bundle.Inventory, $utf8)
Assert-Movie ($bundle.Files -eq 106 -and $bundle.Chunks -eq 1 -and $bundle.ArchiveBytes -lt 64KB) 'Unexpected fixture archive scope/size.'
$archived = @($inventory | Select-Object -Skip 1 | ForEach-Object { $_.Split("`t")[0] })
Assert-Movie (($archived -join "`n") -ceq ($expected -join "`n")) 'Archive includes missing/unrelated movie data.'
foreach ($path in @($partial, $unrelated)) {
    Assert-Movie ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $extraBefore[$path]) 'Excluded file changed.'
}
$checks++

$missing = Join-Path $generated 'story/clip105.mp4'
$retained = Join-Path $root 'retained-missing-fixture.mp4'
Move-Item -LiteralPath $missing -Destination $retained
try { Expect-MovieRejection { Get-PortableMovieFiles -SourceVideoDirectory $original -VideoDirectory $generated } 'Missing expected generated movie: story/clip105.mp4' }
finally { Move-Item -LiteralPath $retained -Destination $missing }
$checks++

Expect-MovieRejection { ConvertTo-PortableMovieTargets -SourceRelativePaths @('story/one.bik', 'STORY/ONE.BIK') -ExpectedCount 2 } 'Duplicate case-insensitive original'
$checks++
foreach ($unsafe in @('../escape.bik', '/absolute.bik', 'story//empty.bik', 'story\backslash.bik', 'story/CON.bik', 'AUX/movie.bik', 'story/trailing./one.bik', 'story/trailing /one.bik', 'C:/escape.bik', 'story/not-a-bink.mp4')) {
    Expect-MovieRejection { ConvertTo-PortableMovieTargets -SourceRelativePaths @($unsafe) -ExpectedCount 1 } 'Unsafe original movie'
}
$checks++
Expect-MovieRejection { ConvertTo-PortableMovieTargets -SourceRelativePaths @('only.bik') } 'catalogue has 1 entries; expected 106'
Expect-MovieRejection { ConvertTo-PortableMovieTargets -SourceRelativePaths @() } 'catalogue has 0 entries; expected 106'
$checks++
$normalized = @(ConvertTo-PortableMovieTargets -SourceRelativePaths @('Story/INTRO.BIK', '字幕/intro 1.bik') -ExpectedCount 2)
Assert-Movie ($normalized -contains 'story/intro.mp4' -and $normalized -contains '字幕/intro 1.mp4') 'Safe names/normalization changed.'
$checks++

# Windows normally disallows same-directory case aliases. Inject a case-sensitive filesystem's second FileInfo
# through the module's enumeration boundary, while leaving every real fixture file untouched.
$module = Get-Module PortableRelease
$duplicatePath = Join-Path $generated 'STORY/CLIP000.MP4'
& $module {
    param($OriginalRoot, $GeneratedRoot, $DuplicatePath)
    $script:movieFixtureGenerated = $GeneratedRoot
    $script:movieFixtureDuplicate = $DuplicatePath
    function script:Get-ChildItem {
        param([string]$LiteralPath, [switch]$File, [switch]$Recurse, [switch]$Force)
        Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters
        if ($LiteralPath -ieq $script:movieFixtureGenerated) { [IO.FileInfo]::new($script:movieFixtureDuplicate) }
    }
    try {
        $message = ''
        try { Get-PortableMovieFiles -SourceVideoDirectory $OriginalRoot -VideoDirectory $GeneratedRoot | Out-Null }
        catch { $message = $_.Exception.Message }
        if ($message -notmatch 'Duplicate case-insensitive generated movie') { throw "Wrong duplicate-fixture result: $message" }
    } finally {
        Remove-Item -LiteralPath Function:\Get-ChildItem
        Remove-Variable -Scope Script -Name movieFixtureGenerated, movieFixtureDuplicate
    }
} $original $generated $duplicatePath
$checks++

# A junction fixture needs no existing directory/process changes. Remove only its own directory entry afterward.
$linked = Join-Path $root 'linked-generated'
[void](New-Item -ItemType Junction -Path $linked -Target $generated)
try {
    Expect-MovieRejection { Get-PortableMovieFiles -SourceVideoDirectory $original -VideoDirectory $linked } 'Movie path crosses a link'
    # The selected root itself is a normal directory, but an ancestor is a junction.
    Expect-MovieRejection { Get-PortableMovieFiles -SourceVideoDirectory $original -VideoDirectory (Join-Path $linked 'story') } 'Movie path crosses a link'
} finally {
    $ownedLink = Get-Item -LiteralPath $linked -Force
    Assert-Movie ($ownedLink.FullName -ceq (Join-Path $root 'linked-generated') -and ($ownedLink.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Refusing to retire an unexpected fixture path.'
    [IO.Directory]::Delete($ownedLink.FullName) # Nonrecursive: remove the junction entry, never its target.
}
Assert-Movie (@(Get-ChildItem -LiteralPath $generated -File -Recurse -Force).Count -eq 108) 'Junction retirement changed generated files.'
$checks++

# Retain the uniquely owned fixture directory for inspection. No recursive cleanup or real-media changes occur.
[pscustomobject]@{ Result = 'PASS'; Checks = $checks; OutputDirectory = $root; SelectedMovies = $selection.Count; ArchiveBytes = $bundle.ArchiveBytes; ExcludedFilesUnchanged = $true }
