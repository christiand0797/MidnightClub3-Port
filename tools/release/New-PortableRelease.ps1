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
    [string]$ContentManifestUrl,
    [switch]$NoEnhance  # leave out Make HD (enhance\, Make HD.cmd, the HD first-run settings)
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
# NVIDIA DLSS (cmake/DLSS.cmake): the NGX runtime and its license, when the build has them
foreach ($name in @('nvngx_dlss.dll', 'NVIDIA_DLSS_LICENSE.txt')) {
    $file = Join-Path $exe.DirectoryName $name
    if (Test-Path -LiteralPath $file -PathType Leaf) { Copy-Item -LiteralPath $file -Destination (Join-Path $stage $name) }
}
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
# "Make HD" (tools/enhance/build_hd.py): the HD textures, PBR pack and HD movies built on the player's PC from their own
# disc. enhance\ holds a private Python with the packages the tools import, the tools as laid out in the repository,
# the Real-ESRGAN upscaler with the models the pipeline uses and the PBR networks (DeepBump, PBRify as ONNX). No game
# texture, map or movie is packaged; ffmpeg is fetched by the movie step itself.
if (-not $NoEnhance) {
    $cache = Join-Path $projectRoot 'out/release-cache'
    [void][IO.Directory]::CreateDirectory($cache)
    $enhance = Join-Path $stage 'enhance'
    $pythonZip = Join-Path $cache 'python-3.12.10-embed-amd64.zip'
    if (-not (Test-Path -LiteralPath $pythonZip)) {
        Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip' -OutFile $pythonZip
    }
    if ((Get-FileHash -LiteralPath $pythonZip -Algorithm SHA256).Hash -ne '4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3') {
        throw "$pythonZip does not match the pinned Python 3.12.10 embeddable package."
    }
    $python = Join-Path $enhance 'python'
    Expand-Archive -LiteralPath $pythonZip -DestinationPath $python
    # the embeddable package ignores site-packages unless its ._pth file names it
    [IO.File]::WriteAllText((Join-Path $python 'python312._pth'), "python312.zip`r`n.`r`nLib\site-packages`r`nimport site`r`n", [Text.UTF8Encoding]::new($false))
    $sitePackages = Join-Path $python 'Lib/site-packages'
    & py -3.12 -m pip install --quiet --disable-pip-version-check --target $sitePackages --only-binary=:all: `
        --platform win_amd64 --python-version 3.12 --implementation cp `
        numpy==2.4.3 pillow==12.1.1 scipy==1.17.1 onnxruntime-directml==1.24.4
    if ($LASTEXITCODE) { throw 'pip could not fetch the Make HD packages.' }
    Get-ChildItem -LiteralPath $sitePackages -Recurse -Directory -Filter '__pycache__' | Remove-Item -Recurse -Force
    Get-ChildItem -LiteralPath $sitePackages -Directory | Where-Object Name -In 'bin' | Remove-Item -Recurse -Force
    $toolFiles = @(
        'tools/enhance/build_hd.py',
        'tools/texture_pack/extract_all.py', 'tools/texture_pack/upscale.py', 'tools/texture_pack/xiso.py',
        'tools/texture_pack/dave.py', 'tools/texture_pack/texfile.py', 'tools/texture_pack/xbpf.py', 'tools/texture_pack/xbtex.py',
        'tools/pbr/pbrlib.py', 'tools/pbr/generate.py', 'tools/pbr/test_pack.py', 'tools/pbr/sets/hd_release.json',
        'tools/video/upscale_movies.py')
    $models = 'tools/pbr/models'
    $upscaler = 'upscale_tools/realesrgan-20220424'
    $toolFiles += @("$models/deepbump256.onnx", "$models/pbrify/1x-PBRify_NormalV3.onnx", "$models/pbrify/1x-PBRify_RoughnessV2.onnx",
        "$models/pbrify/1x-PBRify_Height.onnx", "$models/pbrify/LICENSE",
        "$upscaler/realesrgan-ncnn-vulkan.exe", "$upscaler/vcomp140.dll",
        'upscale_tools/models_extra/ultrasharp-4x.param', 'upscale_tools/models_extra/ultrasharp-4x.bin',
        'upscale_tools/models_extra/remacri-4x.param', 'upscale_tools/models_extra/remacri-4x.bin',
        'upscale_tools/texconv.exe')  # the PBR pack's BC5 normal and BC7 ORM maps (tools/pbr/test_pack.py)
    foreach ($relative in $toolFiles) {
        $source = Join-Path $projectRoot $relative
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Make HD needs $relative (models: tools/pbr/generate.py --download-models then --export-onnx; upscaler and texconv.exe from DirectXTex: upscale_tools)."
        }
        $target = Join-Path $enhance $relative
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
        Copy-Item -LiteralPath $source -Destination $target
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DirectXTex-LICENSE.txt') -Destination (Join-Path $enhance 'upscale_tools/DirectXTex-LICENSE.txt')
    [void][IO.Directory]::CreateDirectory((Join-Path $enhance 'tools/pbr/overrides'))
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Make HD.cmd') -Destination (Join-Path $stage 'Make HD.cmd')
    [void][IO.Directory]::CreateDirectory((Join-Path $stage 'config'))
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'default-settings.toml') -Destination (Join-Path $stage 'config/default-settings.toml')
}
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
    "3. The first start downloads the game content. Keep the internet on and wait."
} else {
    "3. When it asks for your game, pick your Midnight Club 3 DUB Edition Remix (USA) .iso file.`n   (Tip: put the .iso in this folder and it finds it by itself.)"
}
$hdInstructions = if ($NoEnhance) { '' } else {
    "5. When it asks ""Make the game HD?"", click YES. A black window makes HD textures,`n   shiny realistic materials and HD movies from your own disc. It takes a few hours.`n   It pauses while you play, so leave it on overnight. When it says ALL DONE!,`n   start the game again. (Closed it? Double-click ""Make HD"" in this folder.)"
}
[IO.File]::WriteAllText((Join-Path $stage 'START HERE.txt'), @"
MIDNIGHT CLUB 3: DUB EDITION REMIX - PC VERSION
===============================================

HOW TO PLAY
1. Make sure you EXTRACTED this ZIP (right-click it, Extract All). Don't play inside the ZIP.
2. Double-click mc3.exe.
   If Windows says "Windows protected your PC": click More info, then Run anyway.
$setupInstructions
4. The first start builds the game for your PC. Wait a few minutes (only the first time).
$hdInstructions
6. Play! Next time use the "Midnight Club 3 Remix (Native)" shortcut on your desktop.

KEYS
  Arrow keys = move in menus      Space = choose      Escape = back      Enter = start/pause
  F1 = PC settings (graphics, cars, more)              F2 = car tuning
  Controller: plug it in before starting. Back + Y = PC settings.

PROBLEMS?
  Slow or choppy?  Press F1, Display, and lower Render scale (try 100%).
  Don't delete or move anything in this folder (the DLLs, compiler and enhance folders are needed).
  More help: INSTALL.md and PLAYER_GUIDE.md in this folder.

Version: $Version   Source revision: $SourceRevision
Your saves and settings stay on this PC. Nothing from the game is downloaded: the HD look is made from your disc.
This is a fan-made development build; the GitHub release notes list what was tested.
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
