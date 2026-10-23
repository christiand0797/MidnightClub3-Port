<# .SYNOPSIS
Build Midnight Club 3's native Windows executable. Pass -DiscImage to verify an original USA disc image.
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

$forwardParameters = @{} + $PSBoundParameters
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot 'templates/game/out/native' }
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory }
$forwardParameters.BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
& (Join-Path $PSScriptRoot 'templates/game/build_mc3.ps1') @forwardParameters
exit $LASTEXITCODE
