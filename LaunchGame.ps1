# Compatibility entry point for the existing desktop shortcut.
[CmdletBinding()]
param(
    [string]$DiscImage,
    [string]$BuildDirectory,
    [switch]$Wait
)
& (Join-Path $PSScriptRoot 'Launch-MC3.ps1') @PSBoundParameters
exit $LASTEXITCODE
