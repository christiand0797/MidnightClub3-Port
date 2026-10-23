<#
.SYNOPSIS
Launch the native Windows executable using an original disc image for game data.
.EXAMPLE
.\Launch-MC3.ps1 -DiscImage 'E:\Games\Midnight Club 3.xiso.iso'
#>
[CmdletBinding()]
param(
    [string]$DiscImage,
    [string]$BuildDirectory,
    [switch]$Wait
)

$ErrorActionPreference = 'Stop'
try {
    if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot 'templates/game/out/native' }
    if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory }
    $executable = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) 'mc3.exe'
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "The native executable has not been built at $executable. Run .\Build-MC3.ps1 first."
    }
    if ([string]::IsNullOrWhiteSpace($DiscImage)) {
        $savedDisc = Join-Path $PSScriptRoot '.local/source-disc.txt'
        if (Test-Path -LiteralPath $savedDisc -PathType Leaf) { $DiscImage = (Get-Content -LiteralPath $savedDisc -Raw).Trim() }
    }
    $launchOptions = @{
        FilePath = $executable
        WorkingDirectory = (Join-Path $PSScriptRoot 'templates/game')
        PassThru = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($DiscImage)) {
        if (-not (Test-Path -LiteralPath $DiscImage -PathType Leaf)) { throw "The source disc image does not exist: $DiscImage" }
        $DiscImage = (Resolve-Path -LiteralPath $DiscImage).ProviderPath
        # Windows file names cannot contain a quotation mark, so this preserves paths containing spaces.
        $launchOptions.ArgumentList = @('--disc', ('"{0}"' -f $DiscImage))
    }
    if ($Wait) { $launchOptions.Wait = $true }
    $process = Start-Process @launchOptions
    Write-Host "Started mc3.exe (PID $($process.Id))."
    if ($Wait) { exit $process.ExitCode }
}
catch {
    Write-Error -Message $_.Exception.Message -ErrorAction Continue
    exit 1
}
