# Synthetic local packs only. No network calls, native game, user install or player data are used.
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Update-MC3.ps1') -LibraryOnly
if(-not $OutputDirectory){$OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('mc3-update-tests-'+[Guid]::NewGuid().ToString('N'))}
$root=[IO.Path]::GetFullPath($OutputDirectory)
Assert-Mc3 (-not (Test-Path -LiteralPath $root)) 'Choose an unused test output directory.'
[void][IO.Directory]::CreateDirectory($root)
$script:checks=0;$script:networkDownloads=0;$script:busy=@();$script:fixtureRelease=$null
function Check([bool]$Condition,[string]$Message){Assert-Mc3 $Condition $Message;$script:checks++}
function Reject([scriptblock]$Operation,[string]$Message){$hit=$false;try{&$Operation|Out-Null}catch{$hit=$true};Check $hit $Message}
function Put([string]$Path,[string]$Text){[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path));[IO.File]::WriteAllText($Path,$Text,$script:Mc3Utf8)}
function Hash-Text([string]$Text){$h=[Security.Cryptography.SHA256]::Create();try{return ([BitConverter]::ToString($h.ComputeHash($script:Mc3Utf8.GetBytes($Text)))).Replace('-','').ToLowerInvariant()}finally{$h.Dispose()}}
function New-Pack([string]$Name,[hashtable]$Files,[string]$BadHash='', [string]$LinkName=''){
    $rows=@();foreach($key in @($Files.Keys|Sort-Object)){$text=[string]$Files[$key];$rows+=[ordered]@{path=$key;size=$script:Mc3Utf8.GetByteCount($text);sha256=$(if($key-eq $BadHash){'0'*64}else{Hash-Text $text})}}
    $manifest=[ordered]@{format=1;name='MidnightClub3-Native';architecture='windows-x64';version='1.0-fixture';source_revision=('a'*40);expected_xbe_sha1='5bca1cf092edaadf876e8f80129900407a4a07b8';files=$rows}
    $zip=Join-Path $root ($Name+'.zip');$archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
    try{
        foreach($key in $Files.Keys){$entry=$archive.CreateEntry([string]$key)
            if($key-eq $LinkName){$entry.ExternalAttributes=[BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]2717843456),0)}
            $writer=[IO.StreamWriter]::new($entry.Open(),$script:Mc3Utf8);try{$writer.Write([string]$Files[$key])}finally{$writer.Dispose()}}
        $entry=$archive.CreateEntry('release-manifest.json');$writer=[IO.StreamWriter]::new($entry.Open(),$script:Mc3Utf8)
        try{$writer.Write(($manifest|ConvertTo-Json -Depth 8))}finally{$writer.Dispose()}
    }finally{$archive.Dispose()};return $zip
}
function New-Install([string]$Name){$dir=Join-Path $root $Name;Put (Join-Path $dir 'mc3.exe') 'old runtime';Put (Join-Path $dir 'settings.toml') 'player preferences';Put (Join-Path $dir 'hdd7/save.bin') 'player career';Put (Join-Path $dir 'textures/replace/user.png') 'player art';return $dir}
function Select-Pack([string]$Zip){
    $script:fixtureZip=$Zip
    $script:fixtureRelease=[pscustomobject]@{id=10;draft=$false;published_at='2026-11-04T00:00:00Z';tag_name='v1.0-fixture';html_url='https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v1.0-fixture';assets=@([pscustomobject]@{id=20;name='MidnightClub3-Native-1.0-fixture-win64.zip';state='uploaded';digest=('sha256:'+(Get-Mc3Hash $Zip));size=(Get-Item -LiteralPath $Zip).Length;browser_download_url='https://github.com/christiand0797/MidnightClub3-Port/releases/download/v1.0-fixture/MidnightClub3-Native-1.0-fixture-win64.zip'})}
}
function Get-Mc3PublishedRelease([long]$Id=0){return $script:fixtureRelease}
function Get-Mc3TagRevision($Release){return ('a'*40)}
function Get-Mc3LatestSource {return ('a'*40)}
function Get-Mc3RevisionRelation([string]$Current,[string]$Candidate){if(Test-Mc3Revision $Current $Candidate){return 'identical'};return 'ahead'}
function Get-Mc3Busy([string]$Install,[int]$ParentId){return $script:busy}
function Invoke-WebRequest([switch]$UseBasicParsing,[string]$Uri,[string]$OutFile,[int]$TimeoutSec){$script:networkDownloads++;[IO.File]::Copy($script:fixtureZip,$OutFile)}

$zip=New-Pack 'good' @{'mc3.exe'='new runtime';'compiler/config/game.toml'='distributed tool config';'docs/change.md'='release notes';'config/default-settings.toml'='new defaults';'content-source.txt'='new content pin'}
Select-Pack $zip
$verified=Read-Mc3Archive $zip (Get-Mc3Hash $zip) (Get-Item -LiteralPath $zip).Length
Check ($verified.manifest.files.Count-eq 5 -and $verified.manifest_sha256-match '^[0-9a-f]{64}$') 'Valid manifest was not verified.'
foreach($path in @('../mc3.exe','/mc3.exe','C:/mc3.exe','compiler/a:stream','compiler/../mc3.exe','compiler/a.','compiler/CON.txt','compiler\a.h')){Reject {Get-Mc3Relative $path} "Unsafe path accepted: $path"}
foreach($path in @('settings.toml','hdd7/save.bin','assets/default.xbe','textures/replace/a.png','video_hd/a.mp4','compiler/work/current.bin','enhance/tools/pbr/overrides/user.json','unknown.exe')){Reject {Get-Mc3FilePolicy $path} "Unsafe update target accepted: $path"}
foreach($name in @('../escape.exe','settings.toml','compiler/a:stream','assets/default.xbe')){
    $bad=New-Pack ('unsafe-'+[Guid]::NewGuid().ToString('N')) @{'mc3.exe'='runtime';$name='unsafe'}
    Reject {Read-Mc3Archive $bad (Get-Mc3Hash $bad) (Get-Item -LiteralPath $bad).Length} 'Unsafe archive accepted.'
}
$bad=New-Pack 'bad-entry-hash' @{'mc3.exe'='runtime'} 'mc3.exe';Reject {Read-Mc3Archive $bad (Get-Mc3Hash $bad) (Get-Item -LiteralPath $bad).Length} 'Wrong manifest hash accepted.'
Reject {Read-Mc3Archive $zip ('0'*64) (Get-Item -LiteralPath $zip).Length} 'Wrong GitHub ZIP hash accepted.'
$bad=New-Pack 'link' @{'mc3.exe'='runtime';'compiler/link.h'='outside'} '' 'compiler/link.h';Reject {Read-Mc3Archive $bad (Get-Mc3Hash $bad) (Get-Item -LiteralPath $bad).Length} 'Archive symlink accepted.'
$bad=Join-Path $root 'case-collision.zip';[IO.File]::Copy($zip,$bad)
$archive=[IO.Compression.ZipFile]::Open($bad,[IO.Compression.ZipArchiveMode]::Update);try{[void]$archive.CreateEntry('MC3.EXE')}finally{$archive.Dispose()}
Reject {Read-Mc3Archive $bad (Get-Mc3Hash $bad) (Get-Item -LiteralPath $bad).Length} 'Case-colliding archive entry accepted.'
$bad=Join-Path $root 'unlisted.zip';[IO.File]::Copy($zip,$bad)
$archive=[IO.Compression.ZipFile]::Open($bad,[IO.Compression.ZipArchiveMode]::Update);try{[void]$archive.CreateEntry('compiler/unlisted.txt')}finally{$archive.Dispose()}
Reject {Read-Mc3Archive $bad (Get-Mc3Hash $bad) (Get-Item -LiteralPath $bad).Length} 'Unlisted archive entry accepted.'

$InstallDirectory=New-Install 'install-success';Put (Join-Path $InstallDirectory 'config/default-settings.toml') 'old defaults';Put (Join-Path $InstallDirectory 'content-source.txt') 'old content pin'
$Mode='Check';$ReleaseId=0
$checked=Invoke-Mc3Updater;Check ($checked.status-eq 'available' -and $script:networkDownloads-eq 0) 'Check downloaded or failed to report a published runtime.'
$Mode='Stage';$StageDirectory=Join-Path $root 'stage-success';$staged=Invoke-Mc3Updater
Check ($staged.status-eq 'staged' -and $script:networkDownloads-eq 1 -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'old runtime') 'Stage installed without a separate choice.'
$script:busy=@([pscustomobject]@{ProcessId=123});Reject {Invoke-Mc3Apply $InstallDirectory $StageDirectory 123 0} 'Busy game/parent did not defer Apply.';$script:busy=@()
$applied=Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0
Check ($applied.status-eq 'applied' -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'new runtime') 'Explicit Apply did not update runtime.'
Check ([IO.File]::ReadAllText((Join-Path $InstallDirectory 'settings.toml'))-eq 'player preferences' -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'hdd7/save.bin'))-eq 'player career' -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'textures/replace/user.png'))-eq 'player art') 'Player data changed.'
Check ([IO.File]::ReadAllText((Join-Path $InstallDirectory 'config/default-settings.toml'))-eq 'old defaults' -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'content-source.txt'))-eq 'old content pin') 'Existing content/default metadata changed.'
Check ([IO.File]::ReadAllText((Join-Path $StageDirectory 'backup/mc3.exe'))-eq 'old runtime') 'Verified backup not retained.'
$Mode='Check';$checked=Invoke-Mc3Updater;Check ($checked.status-eq 'current' -and $script:networkDownloads-eq 1) 'Current source revision was not compared without downloading.'
function Get-Mc3LatestSource {return ('b'*40)}
$checked=Invoke-Mc3Updater;Check ($checked.status-eq 'source_only' -and $checked.source_ahead -and $script:networkDownloads-eq 1) 'Source commits were mistaken for an executable update.'
$script:publishedOriginal=${function:Get-Mc3PublishedRelease}
function Get-Mc3PublishedRelease([long]$Id=0){throw 'Synthetic no published runtime'}
$checked=Invoke-Mc3Updater;Check ($checked.status-eq 'source_only' -and $checked.release_id-eq 0 -and $script:networkDownloads-eq 1) 'Source-only repository offered a binary.'
${function:Get-Mc3PublishedRelease}=$script:publishedOriginal
function Get-Mc3RevisionRelation([string]$Current,[string]$Candidate){return 'behind'}
$current=Read-Mc3Json (Join-Path $InstallDirectory 'release-manifest.json');$current.source_revision='c'*40;Write-Mc3Json (Join-Path $InstallDirectory 'release-manifest.json') $current
$checked=Invoke-Mc3Updater;Check ($checked.status-eq 'current' -and -not $checked.source_ahead) 'Older release was offered as an update to newer installed source.'
function Get-Mc3RevisionRelation([string]$Current,[string]$Candidate){if(Test-Mc3Revision $Current $Candidate){return 'identical'};return 'ahead'}
function Get-Mc3LatestSource {return ('a'*40)}

# A locally changed payload manifest cannot approve itself; its bytes must match the pinned ZIP.
$InstallDirectory=New-Install 'install-tamper';$Mode='Stage';$StageDirectory=Join-Path $root 'stage-tamper';[void](Invoke-Mc3Updater)
Put (Join-Path $StageDirectory 'payload/release-manifest.json') '{}'
Reject {Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0} 'Tampered extracted manifest accepted.'
Check ([IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'old runtime') 'Tamper rejection changed runtime.'

# Published asset changes after Stage also fail closed before any install writes.
$InstallDirectory=New-Install 'install-remote-change';$Mode='Stage';$StageDirectory=Join-Path $root 'stage-remote-change';[void](Invoke-Mc3Updater)
$script:fixtureRelease.assets[0].digest='sha256:'+('0'*64)
Reject {Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0} 'Changed published asset accepted.'
Select-Pack $zip

# Controlled failure after the first leaf replacement: rollback restores old files and removes only its own new leaf.
$InstallDirectory=New-Install 'install-rollback';$Mode='Stage';$StageDirectory=Join-Path $root 'stage-rollback';[void](Invoke-Mc3Updater)
$script:originalCopy=${function:Copy-Mc3Durable}
function Copy-Mc3Durable([string]$Source,[string]$Target,[string]$ExpectedHash){
    if($Target-match 'mc3\.exe\.mc3-update-'){throw 'Synthetic copy failure'}
    &$script:originalCopy $Source $Target $ExpectedHash
}
Reject {Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0} 'Controlled apply failure did not reject.'
${function:Copy-Mc3Durable}=$script:originalCopy
Check ([IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'old runtime' -and -not (Test-Path -LiteralPath (Join-Path $InstallDirectory 'docs/change.md'))) 'Rollback did not restore the previous install.'
Check ((Read-Mc3Json (Join-Path $StageDirectory 'transaction.json')).state-eq 'rolled_back') 'Rollback journal is not durable.'

# Simulate a crash between File.Replace and the applied journal state; recovery recognizes exact after bytes.
$InstallDirectory=New-Install 'install-crash';$Mode='Stage';$StageDirectory=Join-Path $root 'stage-crash';[void](Invoke-Mc3Updater)
$receipt=Read-Mc3Json (Join-Path $StageDirectory 'stage.json');$before=Get-Mc3Hash (Join-Path $InstallDirectory 'mc3.exe')
Copy-Mc3Durable (Join-Path $InstallDirectory 'mc3.exe') (Join-Path $StageDirectory 'backup/mc3.exe') $before
Put (Join-Path $InstallDirectory 'mc3.exe') 'new runtime'
$journal=[pscustomobject]@{format=1;id=$receipt.id;install=$InstallDirectory;state='applying';errors=@();files=@([pscustomobject]@{path='mc3.exe';before_exists=$true;before_sha256=$before;after_sha256=(Hash-Text 'new runtime');state='intent'})}
Write-Mc3Json (Join-Path $StageDirectory 'transaction.json') $journal
$recovered=Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0
Check ($recovered.status-eq 'rolled_back' -and [IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'old runtime') 'Crash recovery failed.'

# Recovery must preserve a foreign edit rather than overwrite it with the backup.
$journal.state='applying';$journal.files[0].state='intent';Write-Mc3Json (Join-Path $StageDirectory 'transaction.json') $journal
Put (Join-Path $InstallDirectory 'mc3.exe') 'foreign runtime edit'
Reject {Invoke-Mc3Apply $InstallDirectory $StageDirectory 0 0} 'Recovery overwrote a foreign edit.'
Check ([IO.File]::ReadAllText((Join-Path $InstallDirectory 'mc3.exe'))-eq 'foreign runtime edit' -and (Read-Mc3Json (Join-Path $StageDirectory 'transaction.json')).state-eq 'recovery_required') 'Foreign edit/recovery journal not preserved.'
Check ((Test-Path -LiteralPath (Join-Path $InstallDirectory '.mc3-update.lock'))) 'Pending recovery lost its ownership lock.'

# Ordinary Windows junctions are rejected before extraction/replacement; fixture output is preserved for inspection.
$outside=Join-Path $root 'outside';[void][IO.Directory]::CreateDirectory($outside)
$junction=Join-Path $root 'junction';[void](New-Item -ItemType Junction -Path $junction -Target $outside)
Reject {Assert-Mc3NoReparse (Join-Path $junction 'anything')} 'Junction ancestor accepted.'
$result=Join-Path $root 'error-result.json'
& "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Update-MC3.ps1') -Mode Apply -InstallDirectory (Join-Path $root 'does-not-exist') -StageDirectory (Join-Path $root 'no-stage') -ResultPath $result|Out-Null
$failure=Read-Mc3Json $result
Check ($LASTEXITCODE-eq 1 -and $failure.schema-eq 1 -and $failure.status-eq 'error' -and $failure.message.Length-le 2048) 'CLI error result schema/exit code failed.'
[pscustomobject]@{checks=$script:checks;status='passed';network='mocked only';output=$root}|ConvertTo-Json
