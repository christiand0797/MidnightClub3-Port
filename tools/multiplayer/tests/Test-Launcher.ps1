# Real script execution with fake child processes. No relay, game, capture, browser or network is launched.
$ErrorActionPreference='Stop'
$launcher=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../Launch-Multiplayer.ps1'))
$tokens=$null;$errors=$null
[void][Management.Automation.Language.Parser]::ParseFile($launcher,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$p=(& $launcher -PlanOnly -Instance 7|ConvertFrom-Json)
if($p.TunnelPort-ne30748 -or $p.Relay-ne'wss://mc3.top/ws' -or $p.Browser-ne'https://mc3.top/' -or $p.Capture-ne'off' -or $p.ControllerSharing -or $p.GameLaunched -or $p.AdministratorRequired){throw 'Unsafe default plan.'}
$l=(& $launcher -PlanOnly -Instance 7 -Local|ConvertFrom-Json)
if($l.TunnelPort-ne30748 -or $l.Relay-ne'ws://127.0.0.1:8080/ws' -or $l.Browser-ne'http://127.0.0.1:8080/'){throw 'Local plan differs.'}
$s=(& $launcher -PlanOnly -Instance 7 -Local -StartStream -AllowControl|ConvertFrom-Json)
if($s.Capture-ne'game-window-only' -or -not $s.ControllerSharing -or $s.Browser-ne'http://127.0.0.1:8080/'){throw 'Explicit stream plan differs.'}
$rejected=$false
try{& $launcher -PlanOnly -AllowControl}catch{$rejected=$true}
if(-not $rejected){throw 'Control without stream was accepted.'}
foreach($invalid in @(@{Instance=9},@{StartStream=$true;RelayPort=8766},@{StartStream=$true;Server='ws://localhost:8766/ws'},@{Server='ws://example.com/ws'})){
    $rejected=$false
    try{& $launcher -PlanOnly @invalid}catch{$rejected=$true}
    if(-not $rejected){throw 'Invalid instance, colliding port or insecure remote was accepted.'}
}
$remote=(& $launcher -PlanOnly -Server 'wss://example.com/ws'|ConvertFrom-Json)
if($remote.Relay-ne'wss://example.com/ws' -or $remote.Browser-ne'https://example.com/'){throw 'Remote relay routing differs.'}
$remote=(& $launcher -PlanOnly -Server 'wss://example.com'|ConvertFrom-Json)
if($remote.Relay-ne'wss://example.com/ws'){throw 'Missing relay path was not normalized.'}

$global:mc3TestChildren=@{};$global:mc3TestStopped=@();$global:mc3TestSpawned=@();$global:mc3TestNextId=990000
function Start-Process {
    param($FilePath,$ArgumentList,$WorkingDirectory,$WindowStyle,[switch]$PassThru,$RedirectStandardOutput,$RedirectStandardError)
    if($WindowStyle-ne'Hidden'){throw 'Child was not hidden.'}
    $global:mc3TestNextId++
    $child=[pscustomobject]@{Id=$global:mc3TestNextId;StartTime=[DateTime]::UtcNow;HasExited=$false}
    $child|Add-Member ScriptMethod Refresh { $this.HasExited=$true }
    $child|Add-Member ScriptMethod Dispose { }
    $child|Add-Member ScriptMethod Kill { $global:mc3TestStopped+= $this.Id; $this.HasExited=$true }
    $global:mc3TestChildren[$child.Id]=$child
    $global:mc3TestSpawned+=@{Exe=$FilePath;Args=$ArgumentList;Directory=$WorkingDirectory;RelayHost=$env:RELAY_HOST;Token=$env:MC3_STREAM_PAIRING_TOKEN}
    return $child
}
function Invoke-RestMethod {param($Uri,$TimeoutSec) return @{service='mc3-relay';protocol=1;status='ok'}}
function Get-Process {throw 'Cleanup must use its retained process object, not look up a reused PID.'}
function Stop-Process {throw 'Cleanup must use its retained process object, not look up a reused PID.'}
function Start-Sleep {param($Milliseconds) }
function Test-Path {
    param($LiteralPath,$PathType='Any')
    if(([string]$LiteralPath).Replace('\','/') -like '*relay/node_modules/express/package.json'){return $true}
    return Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath -PathType $PathType
}
$beforeRelayHost=$env:RELAY_HOST;$beforeToken=$env:MC3_STREAM_PAIRING_TOKEN
$beforeLocal=$env:LOCALAPPDATA
try {
    $env:LOCALAPPDATA=Join-Path ([IO.Path]::GetTempPath()) ('mc3-launcher-fixture-'+[guid]::NewGuid().ToString('N'))
    # Reserve no ports; select a currently unused high TCP port for the mocked child plan.
    $listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$listener.Start()
    $port=$listener.LocalEndpoint.Port;$listener.Stop()
    try{& $launcher -Instance 8 -RelayPort $port -NoBrowser}catch{if($_.Exception.Message-notmatch 'relay exited'){throw}}
    if($global:mc3TestSpawned.Count-ne2 -or $global:mc3TestStopped.Count-ne1){throw 'Owned-child startup/cleanup count differs.'}
    if($global:mc3TestStopped[0]-ne$global:mc3TestNextId){throw 'Cleanup affected a child that had already exited.'}
    if($global:mc3TestSpawned[0].RelayHost-ne'127.0.0.1' -or $global:mc3TestSpawned[1].Args-notmatch '"--instance" "8"'){throw 'Host or instance routing differs.'}
    if($env:RELAY_HOST-ne$beforeRelayHost -or $env:MC3_STREAM_PAIRING_TOKEN-ne$beforeToken){throw 'Launcher leaked child environment into its caller.'}
    if($global:mc3TestSpawned[1].Args-notmatch '[A-Za-z]:.*proxy[\\/]proxy.py'){throw 'Proxy path is not absolute.'}
}finally{$env:LOCALAPPDATA=$beforeLocal}
@{status='passed';checks=15;real_services_started=0;real_capture_started=0}|ConvertTo-Json
