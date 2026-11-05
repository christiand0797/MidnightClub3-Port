<#
.SYNOPSIS
Puts a running game online and opens the account page. Never launches the game.
.DESCRIPTION
With no -Room the game joins the Main Lobby, where everyone on the server is (no player cap);
the System Link matches hosted there show up in the game's Join Game list. -Room NAME joins
that room instead (made on first use, not listed), so friends sharing a name play privately.
-Server wss://host/ws (default wss://mc3.top/ws) uses a hosted relay; -Local starts or reuses a local relay.
Race points and rankings go to the account signed in on this PC for that server: -SignIn (or
-Register for a new account) asks for name and password once and keeps a 30-day device sign-in,
never the password; -SignOut forgets it; -Guest races as a guest anyway.
-StartStream shares only the game window after host pairing; -AllowControl separately
enables additional controller ports. Ctrl+C stops only children created by this launcher.
#>
[CmdletBinding()]
param(
    [ValidateRange(0,8)][int]$Instance=0,
    [ValidateRange(1024,65534)][int]$RelayPort=8080,
    [ValidateRange(1024,65533)][int]$StreamPort=8765,
    [ValidateLength(0,50)][string]$Room='',
    [string]$Server=$(if($null -ne $env:MC3_SERVER){$env:MC3_SERVER}else{'wss://mc3.top/ws'}),
    [switch]$Local,
    [string]$Python='C:\Users\purpl\AppData\Local\Programs\Python\Python312\python.exe',
    [string]$GameExecutable=(Join-Path $PSScriptRoot '../../templates/game/out/native/mc3.exe'),
    [switch]$StartStream, [switch]$AllowControl, [switch]$NoBrowser, [switch]$PlanOnly,
    [switch]$SignIn, [switch]$Register, [switch]$SignOut, [switch]$Guest
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($AllowControl -and -not $StartStream){throw '-AllowControl requires -StartStream.'}
if(@($SignIn,$Register,$SignOut,$Guest|Where-Object {$_}).Count -gt 1){throw 'Use only one of -SignIn, -Register, -SignOut or -Guest.'}
if($Room -match '[\x00-\x1f\x7f]'){throw 'Invalid room name.'}
if($Local -or ($PSBoundParameters.ContainsKey('RelayPort') -and -not $PSBoundParameters.ContainsKey('Server'))){$Server=''}
$base='http://127.0.0.1:'+ $RelayPort
$relay='ws://127.0.0.1:'+ $RelayPort+'/ws'
if($Server){
    if($Server -notmatch '^(wss?)://([A-Za-z0-9.-]+(:\d{1,5})?)(/[A-Za-z0-9/_-]*)?$'){throw 'Set -Server to a relay URL such as wss://host/ws.'}
    $relay=$Server
    $base=$(if($Matches[1] -eq 'wss'){'https://'}else{'http://'})+$Matches[2]
    if(([uri]$Server).Scheme -eq 'ws' -and -not ([uri]$Server).IsLoopback){throw 'Remote accounts and relay traffic require wss:// with HTTPS.'}
    if(([uri]$relay).AbsolutePath -eq '/'){$relay=$relay.TrimEnd('/')+'/ws'}
}
$roomLabel=$(if($Room.Trim()){'room "'+$Room.Trim()+'"'}else{'Main Lobby'})
$udpPort=30741+$Instance
if($StartStream -and ([uri]$relay).IsLoopback -and ($StreamPort+1) -eq ([uri]$relay).Port){throw 'Stream and relay cannot own the same TCP port.'}
$plan=[ordered]@{Browser=$base+'/';Relay=$relay;Room=$roomLabel;Instance=$Instance;TunnelPort=$udpPort;
    Stream=$(if($StartStream){'ws://127.0.0.1:'+($StreamPort+1)}else{$null});
    Account=$(if($Guest){'guest'}else{'signed-in account if any'});ControllerSharing=[bool]$AllowControl;Capture=$(if($StartStream){'game-window-only'}else{'off'});
    GameLaunched=$false;AdministratorRequired=$false;StopsOnlyOwnedChildren=$true}
if($PlanOnly){$plan|ConvertTo-Json;return}
if(-not (Test-Path -LiteralPath $Python -PathType Leaf)){throw 'Set -Python to an installed Python 3.12 executable.'}
$relayDir=Join-Path $PSScriptRoot 'relay'
if(-not $Server){
    $node=(Get-Command node -CommandType Application -ErrorAction Stop|Select-Object -First 1).Source
    $major=[int](([string](& $node --version)).TrimStart('v').Split('.')[0])
    if($major -lt 24 -or $major -ge 27){throw 'Install supported Node.js 24 LTS or 26.'}
    if(-not (Test-Path -LiteralPath (Join-Path $relayDir 'node_modules/express/package.json'))){throw 'Run npm ci --omit=dev --ignore-scripts in tools/multiplayer/relay first.'}
}
& $Python -c 'import websockets; assert 14 <= int(websockets.__version__.split(chr(46))[0]) < 18'
if($LASTEXITCODE){throw 'Install websockets >=14,<18 with the selected Python.'}
$proxyScript=Join-Path $PSScriptRoot 'proxy/proxy.py'
if($SignOut){& $Python $proxyScript --relay $relay --sign-out;return}
if($SignIn -or $Register){
    & $Python $proxyScript --relay $relay $(if($Register){'--register'}else{'--sign-in'})
    if($LASTEXITCODE){throw 'Sign-in did not complete; nothing was started.'}
}elseif(-not $Guest){& $Python $proxyScript --relay $relay --whoami}
if($StartStream){
    & $Python -c 'import PIL'
    if($LASTEXITCODE){throw 'Install stream/requirements.txt with the selected Python.'}
    $GameExecutable=[IO.Path]::GetFullPath($GameExecutable)
    if(-not (Test-Path -LiteralPath $GameExecutable -PathType Leaf)){throw 'Choose the exact installed mc3.exe with -GameExecutable.'}
}
function Quote-Native([string]$Value){
    if($Value.Contains([char]0)){throw 'Invalid native argument.'}
    return '"'+[regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"'
}
function Port-InUse([int]$Port,[bool]$Udp){
    $ip=[Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties()
    $rows=if($Udp){$ip.GetActiveUdpListeners()}else{$ip.GetActiveTcpListeners()}
    return @($rows|Where-Object Port -eq $Port).Count -gt 0
}
function Relay-Healthy {
    try {$r=Invoke-RestMethod -Uri ($base+'/healthz') -TimeoutSec 2;return $r.status -eq 'ok' -and $r.service -eq 'mc3-relay' -and $r.protocol -eq 1}
    catch{return $false}
}
if(Port-InUse $udpPort $true){throw "Tunnel port $udpPort is already owned. Reuse that host or choose another instance."}
if($StartStream -and (Port-InUse ($StreamPort+1) $false)){throw 'Stream port is already owned.'}
$reuseRelay=[bool]$Server -or (Port-InUse $RelayPort $false)
if($reuseRelay -and -not $Server -and -not (Relay-Healthy)){throw 'Relay port is occupied by an unrecognized or unhealthy service; no process was changed.'}
$sessionDirectory=Join-Path $env:LOCALAPPDATA ('MidnightClub3-Port/multiplayer/'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($sessionDirectory)
$owned=New-Object 'System.Collections.Generic.List[object]'
function Start-Owned([string]$Executable,[string[]]$Arguments,[string]$Name,[hashtable]$Environment){
    $prior=@{}
    try {
        foreach($key in $Environment.Keys){$prior[$key]=[Environment]::GetEnvironmentVariable($key,'Process');[Environment]::SetEnvironmentVariable($key,$Environment[$key],'Process')}
        $line=($Arguments|ForEach-Object {Quote-Native $_})-join ' '
        $p=Start-Process -FilePath $Executable -ArgumentList $line -WorkingDirectory $relayDir -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $sessionDirectory ($Name+'.log')) -RedirectStandardError (Join-Path $sessionDirectory ($Name+'.err'))
        $owned.Add([pscustomobject]@{Process=$p;Started=$p.StartTime.ToUniversalTime().Ticks;Name=$Name})
        return $p
    }finally{foreach($key in $prior.Keys){[Environment]::SetEnvironmentVariable($key,$prior[$key],'Process')}}
}
try {
    if(-not $reuseRelay){
        $null=Start-Owned $node @((Join-Path $relayDir 'server.js')) 'relay' @{RELAY_PORT=[string]$RelayPort;RELAY_HOST='127.0.0.1';PUBLIC_ORIGIN=$base}
        $ready=$false
        for($i=0;$i -lt 40;$i++){if(Relay-Healthy){$ready=$true;break};Start-Sleep -Milliseconds 100}
        if(-not $ready){throw "Owned relay did not start; inspect $sessionDirectory."}
    }
    $argsTunnel=@('-u',$proxyScript,'--relay',$relay,'--instance',[string]$Instance)
    if(-not $Guest){$argsTunnel+='--account'}
    if($Room.Trim()){$argsTunnel+=@('--room',$Room.Trim())}
    $null=Start-Owned $Python $argsTunnel 'tunnel' @{}
    $browser=$base+'/?relay='+[uri]::EscapeDataString($relay)
    if($StartStream){
        $rng=[Security.Cryptography.RandomNumberGenerator]::Create();$bytes=New-Object byte[] 32
        try{$rng.GetBytes($bytes)}finally{$rng.Dispose()}
        $pair=([BitConverter]::ToString($bytes)).Replace('-','').ToLowerInvariant()
        $argsStream=@('-u',(Join-Path $PSScriptRoot 'stream/stream_proxy.py'),'--relay',$relay,'--port',[string]$StreamPort,'--instance',[string]$Instance,'--capture-window','--origin',$base,'--game-executable',$GameExecutable)
        if($AllowControl){$argsStream+='--allow-control'}
        $null=Start-Owned $Python $argsStream 'stream' @{MC3_STREAM_PAIRING_TOKEN=$pair}
        $browser+='&stream='+[uri]::EscapeDataString($plan.Stream)+'#pair='+$pair
    }
    if(-not $NoBrowser){Start-Process $browser}
    Write-Host "MC3 account page: $base/"
    Write-Host "Instance $Instance is online in the $roomLabel. The game was not launched."
    Write-Host 'In the game: Networking > Xbox System Link, then Create Match or Join Game.'
    Write-Host "Logs: $sessionDirectory. Ctrl+C stops only this launcher's children."
    if($StartStream -and $NoBrowser){Write-Host 'Pairing link is private; omit -NoBrowser to open it.'}
    while($true){
        foreach($entry in $owned){$entry.Process.Refresh();if($entry.Process.HasExited){throw "$($entry.Name) exited. Inspect $sessionDirectory."}}
        Start-Sleep -Milliseconds 500
    }
}finally{
    foreach($entry in $owned){
        try {if(-not $entry.Process.HasExited -and $entry.Process.StartTime.ToUniversalTime().Ticks -eq $entry.Started){$entry.Process.Kill()}}catch{ }
        $entry.Process.Dispose()
    }
}
