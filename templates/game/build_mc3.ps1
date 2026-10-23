<#
.SYNOPSIS
Build the native Midnight Club 3 executable with the installed Windows tools.
.EXAMPLE
.\build_mc3.ps1 -DiscImage 'E:\Games\Midnight Club 3.xiso.iso' -Jobs 2
#>
[CmdletBinding()]
param(
    [string]$DiscImage,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 256)]
    [int]$Jobs = 2,
    [string]$BuildDirectory,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$gameRoot = $PSScriptRoot
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $gameRoot '../..'))
$expectedXbeSha1 = '5bca1cf092edaadf876e8f80129900407a4a07b8'
$rememberDisc = $PSBoundParameters.ContainsKey('DiscImage') -and -not [string]::IsNullOrWhiteSpace($DiscImage)

function Find-Application {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
        $command = Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { return $command.Source }
    }
    return $null
}

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code ${LASTEXITCODE}: $Program" }
}

function Import-VisualStudioEnvironment {
    $vswhere = Find-Application @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'),
        'vswhere.exe'
    )
    if (-not $vswhere) {
        throw 'vswhere.exe was not found. Install the Visual Studio Build Tools with Desktop development with C++ and a Windows SDK, then retry.'
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        throw 'Visual Studio C++ Build Tools were not found. Add Desktop development with C++ and a Windows SDK to an installed Visual Studio instance.'
    }
    $installation = ($installation | Select-Object -First 1).Trim()
    $devCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCommand -PathType Leaf)) { throw "Missing Visual Studio environment script: $devCommand" }
    $environmentLines = & $env:ComSpec /d /s /c "call `"$devCommand`" -no_logo -arch=amd64 -host_arch=amd64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio failed to initialize its x64 build environment.' }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    return $installation
}

try {
    if ($env:OS -ne 'Windows_NT') { throw 'The native build currently requires Windows, the Visual Studio C++ toolchain, LLVM, CMake, Ninja, and Git.' }
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $gameRoot 'out/native' }
    if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $gameRoot $BuildDirectory }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

    if ([string]::IsNullOrWhiteSpace($DiscImage)) {
        $savedDisc = Join-Path $repositoryRoot '.local/source-disc.txt'
        if (Test-Path -LiteralPath $savedDisc -PathType Leaf) { $DiscImage = (Get-Content -LiteralPath $savedDisc -Raw).Trim() }
    }
    if (-not [string]::IsNullOrWhiteSpace($DiscImage)) {
        if (-not (Test-Path -LiteralPath $DiscImage -PathType Leaf)) { throw "The source disc image does not exist: $DiscImage" }
        $DiscImage = (Resolve-Path -LiteralPath $DiscImage).ProviderPath
    }

    $xbePath = Join-Path $gameRoot 'assets/default.xbe'
    if (-not [string]::IsNullOrWhiteSpace($DiscImage)) {
        $python = Find-Application @('python.exe', 'python3.exe', 'py.exe')
        if (-not $python) { throw 'Python 3.10 or newer is required to verify the selected disc and prepare default.xbe. Install Python and retry.' }
        $pythonArguments = @()
        if ([IO.Path]::GetFileName($python) -ieq 'py.exe') { $pythonArguments += '-3' }
        $pythonArguments += @(
            (Join-Path $repositoryRoot 'tools/disc/verify_xiso.py'), $DiscImage,
            '--expected-title-id', '0x54540079', '--expected-xbe-sha1', $expectedXbeSha1,
            '--extract-xbe', $xbePath
        )
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $xbePath) -Force
        Invoke-Checked $python $pythonArguments
    }
    if (-not (Test-Path -LiteralPath $xbePath -PathType Leaf)) {
        throw 'Missing templates/game/assets/default.xbe. Pass -DiscImage with the original USA Midnight Club 3: DUB Edition Remix image to extract and verify it.'
    }
    if ((Get-FileHash -LiteralPath $xbePath -Algorithm SHA1).Hash -ine $expectedXbeSha1) {
        throw 'assets/default.xbe does not match the verified USA Midnight Club 3: DUB Edition Remix executable. Restore the matching original executable before building.'
    }
    if ($rememberDisc) {
        $null = New-Item -ItemType Directory -Path (Join-Path $repositoryRoot '.local') -Force
        Set-Content -LiteralPath (Join-Path $repositoryRoot '.local/source-disc.txt') -Value $DiscImage -Encoding UTF8
    }

    Write-Host 'Preparing the installed Windows x64 toolchain...'
    $visualStudio = Import-VisualStudioEnvironment
    $scoopRoots = @($env:SCOOP, (Join-Path $env:USERPROFILE 'scoop'), $env:SCOOP_GLOBAL) | Where-Object { $_ }
    $llvmCandidates = @($scoopRoots | ForEach-Object { Join-Path $_ 'apps/llvm/current/bin/clang++.exe' })
    $llvmCandidates += @(
        'clang++.exe',
        (Join-Path $env:ProgramFiles 'LLVM/bin/clang++.exe'),
        (Join-Path $visualStudio 'VC/Tools/Llvm/x64/bin/clang++.exe')
    )
    $clangCpp = Find-Application $llvmCandidates
    if (-not $clangCpp) { throw 'clang++.exe was not found. Install LLVM or the Visual Studio C++ Clang tools, then retry.' }
    $llvmBin = Split-Path -Parent $clangCpp
    $clang = Find-Application @((Join-Path $llvmBin 'clang.exe'), 'clang.exe')
    $lld = Find-Application @((Join-Path $llvmBin 'lld-link.exe'), 'lld-link.exe')
    $cmake = Find-Application @('cmake.exe', (Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'))
    $ninja = Find-Application @('ninja.exe', (Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'))
    $git = Find-Application @('git.exe')
    if (-not $clang -or -not $lld -or -not $cmake -or -not $ninja -or -not $git) {
        throw 'A required build tool is missing. Ensure clang, clang++, lld-link, CMake 3.25+, Ninja, and Git are installed. This script does not install software.'
    }
    $env:PATH = "$llvmBin;$(Split-Path -Parent $cmake);$(Split-Path -Parent $ninja);$env:PATH"
    Write-Host "LLVM: $clangCpp"
    Write-Host "Output: $BuildDirectory"

    $cache = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache -PathType Leaf) {
        $oldHome = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
        if ($oldHome -and [IO.Path]::GetFullPath($oldHome.Matches[0].Groups[1].Value) -ine [IO.Path]::GetFullPath($gameRoot)) {
            throw "The selected build directory belongs to another checkout. Choose an unused -BuildDirectory. Existing cache: $cache"
        }
        $oldGenerator = Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
        if ($oldGenerator -and $oldGenerator.Matches[0].Groups[1].Value -ne 'Ninja') {
            throw 'The selected build directory uses another CMake generator. Choose an unused -BuildDirectory for this Ninja build.'
        }
    }

    $configureArguments = @(
        '-S', $gameRoot, '-B', $BuildDirectory, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_C_COMPILER=$($clang.Replace('\', '/'))",
        "-DCMAKE_CXX_COMPILER=$($clangCpp.Replace('\', '/'))",
        "-DCMAKE_MAKE_PROGRAM=$($ninja.Replace('\', '/'))",
        '-DCMAKE_C_FLAGS=-march=x86-64-v2', '-DCMAKE_CXX_FLAGS=-march=x86-64-v2'
    )
    # Reuse source downloads from a migrated cache without reusing its absolute build paths.
    foreach ($dependency in @('zydis', 'tomlplusplus', 'imgui', 'sdl3')) {
        $sourceDirectory = Join-Path $gameRoot "out/build/release/_deps/$dependency-src"
        if (Test-Path -LiteralPath (Join-Path $BuildDirectory "_deps/$dependency-src") -PathType Container) { continue }
        if (Test-Path -LiteralPath $sourceDirectory -PathType Container) {
            $configureArguments += "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$($sourceDirectory.Replace('\', '/'))"
        }
    }
    Invoke-Checked $cmake $configureArguments
    if ($ConfigureOnly) { Write-Host 'CMake configuration completed.'; exit 0 }

    Write-Host "Building mc3 with $Jobs parallel jobs. The first compilation can take substantial time."
    Invoke-Checked $cmake @('--build', $BuildDirectory, '--target', 'mc3', '--parallel', [string]$Jobs)
    $executable = Join-Path $BuildDirectory 'mc3.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "The build completed without the expected executable: $executable" }
    Write-Host "Native executable built: $executable"
    Write-Host "Launch with: powershell -NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $repositoryRoot 'Launch-MC3.ps1')`""
}
catch {
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
}
