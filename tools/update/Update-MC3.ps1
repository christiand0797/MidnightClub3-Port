<#
.SYNOPSIS
Checks published MC3 runtime releases. Downloading and applying are separate, explicit choices.
.DESCRIPTION
Check never downloads a runtime. Stage downloads a published runtime ZIP only after checking its
GitHub asset SHA256. Apply verifies that same published asset again, waits for this installation's
game/Make HD jobs to exit, and replaces only distributed runtime/tool/document files. It never
kills a process or replaces saves, preferences, game content or generated media. Backups and a
durable journal remain in the stage directory. Re-run Apply on an interrupted stage to roll back.
#>
[CmdletBinding()]
param(
    [ValidateSet('Check','Stage','Apply')][string]$Mode='Check',
    [string]$InstallDirectory=(Join-Path $PSScriptRoot '../..'),
    [string]$StageDirectory,
    [long]$ReleaseId=0,
    [int]$ParentProcessId=0,
    [ValidateRange(0,86400)][int]$WaitSeconds=900,
    [switch]$LibraryOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$script:Mc3Repo='christiand0797/MidnightClub3-Port'
$script:Mc3Utf8=[Text.UTF8Encoding]::new($false,$true)
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert-Mc3([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Get-Mc3Hash([string]$Path){return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Test-Mc3Revision([string]$A,[string]$B){
    return $A -match '^[0-9a-fA-F]{7,40}$' -and $B -match '^[0-9a-fA-F]{7,40}$' -and
        ($A.StartsWith($B,[StringComparison]::OrdinalIgnoreCase)-or $B.StartsWith($A,[StringComparison]::OrdinalIgnoreCase))
}
function Assert-Mc3NoReparse([string]$Path){
    $full=[IO.Path]::GetFullPath($Path)
    Assert-Mc3 ($full -match '^[A-Za-z]:\\') 'Updates require an ordinary local drive path.'
    $scan=$full
    while($scan){
        if(Test-Path -LiteralPath $scan){
            $item=Get-Item -LiteralPath $scan -Force
            Assert-Mc3 (($item.Attributes -band [IO.FileAttributes]::ReparsePoint)-eq 0) "Reparse points are not update targets: $scan"
        }
        $parent=[IO.Path]::GetDirectoryName($scan)
        if($parent -eq $scan){break};$scan=$parent
    }
    return $full.TrimEnd('\')
}
function Get-Mc3Relative([string]$Name){
    Assert-Mc3 ($Name.Length -gt 0 -and $Name.Length -le 1024 -and $Name -notmatch '[\\:\x00-\x1f]' -and -not $Name.StartsWith('/')) 'Unsafe archive path.'
    $parts=$Name.Split('/')
    foreach($part in $parts){
        Assert-Mc3 ($part -and $part-ne '.' -and $part-ne '..' -and $part.Length-le 240 -and $part-notmatch '[<>"|?*]' -and
            $part-notmatch '[. ]$' -and $part-notmatch '^(?i:CON|PRN|AUX|NUL|CLOCK\$|COM[1-9]|LPT[1-9])(?:\.|$)') 'Unsafe Windows path component.'
    }
    return ($parts -join '/')
}
function Join-Mc3Child([string]$Root,[string]$Relative){
    $relative=Get-Mc3Relative $Relative
    $full=[IO.Path]::GetFullPath((Join-Path $Root $relative))
    Assert-Mc3 ($full.StartsWith($Root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) 'Path escaped its root.'
    [void](Assert-Mc3NoReparse $full);return $full
}
function Get-Mc3FilePolicy([string]$Name){
    $name=Get-Mc3Relative $Name
    # These release descriptors/default templates are validated but existing installation metadata is preserved.
    if($name -in @('content-manifest.tsv','content-source.txt','config/default-settings.toml')){return 'Preserve'}
    Assert-Mc3 ($name -notmatch '(?i)(^|/)(hdd[^/]*|saves?|settings(?:\.toml(?:\..*)?)?|assets|textures|video_hd|shadercache|captures|work|logs)(/|$)') 'Player data/content is forbidden in runtime updates.'
    if($name -eq 'release-manifest.json'){return 'Apply'}
    if($name -match '^(?i:mc3\.exe|Check-PbrMaterials\.exe|nvngx_dlss\.dll|(?:concrt140|msvcp140(?:_1|_2|_atomic_wait|_codecvt_ids)?|vccorlib140|vcruntime140(?:_1|_threads)?)\.dll)$'){return 'Apply'}
    if($name -in @('LICENSE','THIRD_PARTY.txt','INSTALL.md','PLAYER_GUIDE.md','START HERE.txt','NVIDIA_DLSS_LICENSE.txt','Make HD.cmd')){return 'Apply'}
    if($name -match '^(?i:compiler)/' -and $name -notmatch '(?i)\.(pdb|xbe|iso|bik|mp4|png|dds)$'){return 'Apply'}
    if($name -match '^(?i:enhance)/(python/|tools/(enhance|texture_pack|pbr|video)/|upscale_tools/)' -and
       $name -notmatch '(?i)/(source|replace|overrides)/' -and $name -notmatch '(?i)\.(xbe|iso|bik|mp4|png|dds)$'){return 'Apply'}
    if($name -match '^(?i:docs)/.+\.(md|txt)$' -or $name -match '^(?i:update)/(Update-MC3\.ps1|README\.md)$'){return 'Apply'}
    throw "Not a distributed runtime/tool/document path: $name"
}
function Write-Mc3Json([string]$Path,$Value){
    [void](Assert-Mc3NoReparse $Path)
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $data=$script:Mc3Utf8.GetBytes(($Value|ConvertTo-Json -Depth 12))
    $file=[IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try{$file.Write($data,0,$data.Length);$file.Flush($true)}finally{$file.Dispose()}
    if(Test-Path -LiteralPath $Path){[IO.File]::Replace($temporary,$Path,$null)}else{[IO.File]::Move($temporary,$Path)}
}
function Read-Mc3Json([string]$Path){
    [void](Assert-Mc3NoReparse $Path)
    Assert-Mc3 ((Get-Item -LiteralPath $Path).Length-le 16MB) 'JSON metadata exceeds its bound.'
    return ([IO.File]::ReadAllText($Path,$script:Mc3Utf8)|ConvertFrom-Json)
}
function Invoke-Mc3Api([string]$Suffix){
    [Net.ServicePointManager]::SecurityProtocol=[Net.ServicePointManager]::SecurityProtocol-bor [Net.SecurityProtocolType]::Tls12
    return Invoke-RestMethod -Uri ('https://api.github.com/repos/'+$script:Mc3Repo+'/'+$Suffix) -Headers @{'User-Agent'='MidnightClub3-Port-Update';Accept='application/vnd.github+json'} -TimeoutSec 30
}
function Get-Mc3PublishedRelease([long]$Id=0){
    if($Id-gt 0){$releases=@(Invoke-Mc3Api ('releases/'+$Id))}
    else{$releases=@(Invoke-Mc3Api 'releases?per_page=100')}
    foreach($release in @($releases|Where-Object {-not $_.draft -and $_.published_at}|Sort-Object published_at -Descending)){
        $assets=@($release.assets|Where-Object {$_.name-match '^MidnightClub3-Native-.+-win64\.zip$' -and $_.state-eq 'uploaded'})
        if($assets.Count-eq 1){return $release}
    }
    throw 'No published runtime ZIP release was found. Source commits alone are not an installable update.'
}
function Get-Mc3TagRevision($Release){
    $ref=Invoke-Mc3Api ('git/ref/tags/'+[Uri]::EscapeDataString([string]$Release.tag_name));$object=$ref.object
    for($i=0;$i-lt 4;$i++){
        Assert-Mc3 ([string]$object.sha-match '^[0-9a-fA-F]{40}$') 'Invalid release tag object.'
        if($object.type-eq 'commit'){return ([string]$object.sha).ToLowerInvariant()}
        Assert-Mc3 ($object.type-eq 'tag') 'Release tag does not identify a commit.'
        $object=(Invoke-Mc3Api ('git/tags/'+$object.sha)).object
    }
    throw 'Release tag chain exceeds its bound.'
}
function Get-Mc3Asset($Release){
    Assert-Mc3 (-not $Release.draft -and $Release.published_at) 'Draft/unpublished releases cannot update the game.'
    $assets=@($Release.assets|Where-Object {$_.name-match '^MidnightClub3-Native-.+-win64\.zip$' -and $_.state-eq 'uploaded'})
    Assert-Mc3 ($assets.Count-eq 1) 'Release must contain one unambiguous runtime ZIP.'
    $asset=$assets[0]
    Assert-Mc3 ([string]$asset.digest-match '^sha256:[0-9a-fA-F]{64}$') 'Published ZIP has no GitHub SHA256 digest; staging is unavailable.'
    Assert-Mc3 ([long]$asset.size-gt 0 -and [long]$asset.size-le 4GB) 'Published ZIP size exceeds its bound.'
    $expected='https://github.com/'+$script:Mc3Repo+'/releases/download/'+[Uri]::EscapeDataString([string]$Release.tag_name)+'/'+[Uri]::EscapeDataString([string]$asset.name)
    Assert-Mc3 ([Uri]::UnescapeDataString([string]$asset.browser_download_url)-ceq [Uri]::UnescapeDataString($expected)) 'Runtime asset URL is outside the selected fixed repository release.'
    return $asset
}
function Read-Mc3Archive([string]$Zip,[string]$ExpectedHash,[long]$ExpectedSize,[string]$Destination=''){
    [void](Assert-Mc3NoReparse $Zip)
    Assert-Mc3 ($ExpectedHash-match '^[0-9a-fA-F]{64}$' -and (Get-Item -LiteralPath $Zip).Length-eq $ExpectedSize -and
        (Get-Mc3Hash $Zip)-eq $ExpectedHash) 'Runtime ZIP differs from its GitHub asset digest/size.'
    $archive=[IO.Compression.ZipFile]::OpenRead($Zip)
    try{
        Assert-Mc3 ($archive.Entries.Count-gt 1 -and $archive.Entries.Count-le 50000) 'Archive entry count exceeds its bound.'
        $entries=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase);$total=[long]0
        foreach($entry in $archive.Entries){
            $directory=$entry.FullName.EndsWith('/');$name=Get-Mc3Relative ($entry.FullName.TrimEnd('/'))
            Assert-Mc3 (-not $entries.ContainsKey($name)) 'Duplicate/case-colliding ZIP entry.'
            $bits=[BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$entry.ExternalAttributes),0)
            Assert-Mc3 (($bits-band 0x400)-eq 0 -and (($bits-shr 16)-band 0xF000)-ne 0xA000) 'Archive links/reparse entries are forbidden.'
            Assert-Mc3 ($entry.Length-ge 0 -and $entry.Length-le 2GB) 'Archive file size exceeds its bound.'
            $total+=$entry.Length;Assert-Mc3 ($total-le 8GB) 'Unpacked archive exceeds its bound.'
            $entries.Add($name,[pscustomobject]@{entry=$entry;directory=$directory})
            if(-not $directory){[void](Get-Mc3FilePolicy $name)}
        }
        foreach($name in $entries.Keys){$parent=$name;while($parent.Contains('/')){$parent=$parent.Substring(0,$parent.LastIndexOf('/'))
            Assert-Mc3 (-not $entries.ContainsKey($parent)-or $entries[$parent].directory) 'File/directory path collision.'}}
        Assert-Mc3 ($entries.ContainsKey('release-manifest.json') -and -not $entries['release-manifest.json'].directory -and
            $entries['release-manifest.json'].entry.Length-le 16MB) 'Missing/bounded release manifest.'
        $reader=[IO.StreamReader]::new($entries['release-manifest.json'].entry.Open(),$script:Mc3Utf8,$false)
        try{$manifest=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
        Assert-Mc3 ($manifest.format-eq 1 -and $manifest.name-eq 'MidnightClub3-Native' -and $manifest.architecture-eq 'windows-x64' -and
            $manifest.source_revision-match '^[0-9a-fA-F]{7,40}$' -and $manifest.expected_xbe_sha1-eq '5bca1cf092edaadf876e8f80129900407a4a07b8') 'Unsupported release manifest identity.'
        $rows=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach($row in @($manifest.files)){
            $name=Get-Mc3Relative ([string]$row.path)
            Assert-Mc3 ($name-ne 'release-manifest.json' -and -not $rows.ContainsKey($name) -and $entries.ContainsKey($name) -and
                -not $entries[$name].directory -and [long]$row.size-eq $entries[$name].entry.Length -and [string]$row.sha256-match '^[0-9a-fA-F]{64}$') 'Manifest entry mismatch/duplicate.'
            $rows.Add($name,$row)
        }
        Assert-Mc3 ($rows.ContainsKey('mc3.exe') -and $rows.Count-eq @($entries.Values|Where-Object {-not $_.directory}).Count-1) 'Manifest does not cover every archive file.'
        if($Destination){[void](Assert-Mc3NoReparse $Destination);Assert-Mc3 (-not (Test-Path -LiteralPath $Destination)) 'Payload destination already exists.';[void][IO.Directory]::CreateDirectory($Destination)}
        $manifestHash=''
        foreach($name in $entries.Keys){if($entries[$name].directory){continue}
            $entry=$entries[$name].entry;$stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create();$output=$null
            try{
                if($Destination){$target=Join-Mc3Child $Destination $name;[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target));$output=[IO.FileStream]::new($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)}
                $buffer=New-Object byte[] 131072;$count=[long]0
                while(($n=$stream.Read($buffer,0,$buffer.Length))-gt 0){$count+=$n;Assert-Mc3 ($count-le $entry.Length) 'Archive decompression exceeded declared size.'
                    [void]$sha.TransformBlock($buffer,0,$n,$buffer,0);if($output){$output.Write($buffer,0,$n)}}
                [void]$sha.TransformFinalBlock([byte[]]@(),0,0)
                Assert-Mc3 ($count-eq $entry.Length) 'Archive decompression size mismatch.'
                $hash=([BitConverter]::ToString($sha.Hash)).Replace('-','').ToLowerInvariant()
                if($name-ne 'release-manifest.json'){Assert-Mc3 ($hash-eq $rows[$name].sha256) "Manifest hash mismatch: $name"}else{$manifestHash=$hash}
                if($output){$output.Flush($true)}
            }finally{if($output){$output.Dispose()};$sha.Dispose();$stream.Dispose()}
        }
        return [pscustomobject]@{manifest=$manifest;manifest_sha256=$manifestHash}
    }finally{$archive.Dispose()}
}
function Copy-Mc3Durable([string]$Source,[string]$Target,[string]$ExpectedHash){
    [void](Assert-Mc3NoReparse $Source);[void](Assert-Mc3NoReparse $Target)
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target))
    $input=[IO.File]::Open($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try{$output=[IO.FileStream]::new($Target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$input.CopyTo($output);$output.Flush($true)}finally{$output.Dispose()}
    }finally{$input.Dispose()}
    Assert-Mc3 ((Get-Mc3Hash $Target)-eq $ExpectedHash) 'Copied file failed hash verification.'
}
function Get-Mc3Busy([string]$Install,[int]$ParentId){
    $busy=@();$prefix=$Install.TrimEnd('\')+'\'
    foreach($process in @(Get-CimInstance Win32_Process)){
        if($process.ProcessId-eq $PID){continue}
        if($ParentId-gt 0 -and $process.ProcessId-eq $ParentId){$busy+=$process;continue}
        $exe=[string]$process.ExecutablePath;$cmd=[string]$process.CommandLine
        $inside=$exe.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)
        if(($process.Name-ieq 'mc3.exe' -and ($inside -or -not $exe)) -or
           ($cmd-match '(?i)(build_hd\.py|Make HD\.cmd|generate\.py|upscale_movies\.py|upscale\.py)' -and
            ($inside -or $cmd.IndexOf($Install,[StringComparison]::OrdinalIgnoreCase)-ge 0 -or $cmd-notmatch '(?i)--native\s+'))){$busy+=$process}
    }
    return $busy
}
function Wait-Mc3Idle([string]$Install,[int]$ParentId,[int]$Seconds){
    $until=[DateTime]::UtcNow.AddSeconds($Seconds)
    while(@(Get-Mc3Busy $Install $ParentId).Count){
        Assert-Mc3 ([DateTime]::UtcNow-lt $until) 'Game/Make HD/parent still running; staged update was preserved. Close it and choose Apply again.'
        Start-Sleep -Milliseconds 500
    }
}
function Restore-Mc3Transaction([string]$Stage,$Journal){
    $Journal.state='rolling_back';Write-Mc3Json (Join-Path $Stage 'transaction.json') $Journal
    $errors=@()
    foreach($row in @($Journal.files|Sort-Object path -Descending)){
        if($row.state-eq 'pending'){continue}
        try{
            Assert-Mc3 ((Get-Mc3FilePolicy $row.path)-eq 'Apply' -and $row.after_sha256-match '^[0-9a-fA-F]{64}$' -and
                (-not $row.before_exists -or $row.before_sha256-match '^[0-9a-fA-F]{64}$')) 'Unsafe transaction row.'
            Assert-Mc3 (@(Get-Mc3Busy $Journal.install 0).Count-eq 0) 'A game/Make HD job is running; recovery remains pending until it exits.'
            $target=Join-Mc3Child $Journal.install $row.path;$exists=Test-Path -LiteralPath $target -PathType Leaf
            $actual=if($exists){Get-Mc3Hash $target}else{''}
            if($row.before_exists -and $actual-eq $row.before_sha256){$row.state='restored';continue}
            Assert-Mc3 (-not $exists -or $actual-eq $row.after_sha256) "Foreign edit prevents rollback: $($row.path)"
            if($row.before_exists){$backup=Join-Mc3Child (Join-Path $Stage 'backup') $row.path
                Assert-Mc3 ((Get-Mc3Hash $backup)-eq $row.before_sha256) 'Backup hash changed.'
                $temp=$target+'.mc3-restore-'+$Journal.id+'.tmp';Copy-Mc3Durable $backup $temp $row.before_sha256
                if($exists){[IO.File]::Replace($temp,$target,$null)}else{[IO.File]::Move($temp,$target)}
            }elseif($exists){Remove-Item -LiteralPath $target -Force} # Only a new transaction-owned leaf with its exact after hash.
            $row.state='restored'
        }catch{$errors+=$_.Exception.Message}
    }
    $Journal.state=if($errors.Count){'recovery_required'}else{'rolled_back'};$Journal.errors=@($errors)
    Write-Mc3Json (Join-Path $Stage 'transaction.json') $Journal
    Assert-Mc3 ($errors.Count-eq 0) ($errors-join '; ')
}
function Invoke-Mc3Apply([string]$Install,[string]$Stage,[int]$ParentId,[int]$Seconds){
    $Install=Assert-Mc3NoReparse $Install;$Stage=Assert-Mc3NoReparse $Stage
    $receipt=Read-Mc3Json (Join-Path $Stage 'stage.json')
    Assert-Mc3 ($receipt.format-eq 1 -and $receipt.repo-ceq $script:Mc3Repo -and $receipt.install-ieq $Install -and
        $receipt.id-match '^[0-9a-f]{32}$') 'Stage receipt does not belong to this install.'
    Wait-Mc3Idle $Install $ParentId $Seconds
    $lockPath=Join-Path $Install '.mc3-update.lock';[void](Assert-Mc3NoReparse $lockPath)
    $prior=Test-Path -LiteralPath $lockPath;$lock=[IO.FileStream]::new($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $token=$receipt.id;$owned=$false
    try{
        if($prior){$bytes=New-Object byte[] ([int]$lock.Length);[void]$lock.Read($bytes,0,$bytes.Length)
            Assert-Mc3 ($script:Mc3Utf8.GetString($bytes)-ceq $token) 'A foreign update lock exists; it was preserved.'}
        $owned=$true;$lock.Position=0;$data=$script:Mc3Utf8.GetBytes($token);$lock.Write($data,0,$data.Length);$lock.SetLength($data.Length);$lock.Flush($true)
        $journalPath=Join-Path $Stage 'transaction.json'
        if(Test-Path -LiteralPath $journalPath){$journal=Read-Mc3Json $journalPath
            Assert-Mc3 ($journal.install-ieq $Install -and $journal.id-ceq $token) 'Transaction belongs to another install.'
            if($journal.state-in @('applying','rolling_back','recovery_required')){Restore-Mc3Transaction $Stage $journal;return [pscustomobject]@{status='rolled_back';message='Interrupted update restored. Choose Apply again to install.'}}
            Assert-Mc3 ($journal.state-ne 'rolled_back') 'This stage was rolled back. Stage a fresh update before applying.'
            Assert-Mc3 ($journal.state-eq 'applied') 'Unknown transaction state.'
            return [pscustomobject]@{status='already_applied';source_revision=$receipt.source_revision}
        }
        $release=Get-Mc3PublishedRelease ([long]$receipt.release_id);$asset=Get-Mc3Asset $release
        Assert-Mc3 ([long]$asset.id-eq [long]$receipt.asset_id -and $asset.digest-ceq $receipt.digest -and [long]$asset.size-eq [long]$receipt.size) 'Published asset changed after staging.'
        $zip=Join-Path $Stage 'runtime.zip';$verified=Read-Mc3Archive $zip $asset.digest.Substring(7) ([long]$asset.size);$manifest=$verified.manifest
        Assert-Mc3 ((Test-Mc3Revision $manifest.source_revision (Get-Mc3TagRevision $release)) -and $manifest.source_revision-ceq $receipt.source_revision) 'Runtime manifest differs from the release source.'
        $rows=@($manifest.files)+@([pscustomobject]@{path='release-manifest.json';size=(Get-Item -LiteralPath (Join-Path $Stage 'payload/release-manifest.json')).Length;sha256=$verified.manifest_sha256})
        $files=@()
        foreach($row in $rows){if((Get-Mc3FilePolicy $row.path)-ne 'Apply'){continue}
            $source=Join-Mc3Child (Join-Path $Stage 'payload') $row.path;$target=Join-Mc3Child $Install $row.path
            Assert-Mc3 ((Get-Item -LiteralPath $source).Length-eq [long]$row.size -and (Get-Mc3Hash $source)-eq $row.sha256) 'Staged payload changed.'
            $exists=Test-Path -LiteralPath $target
            if($exists){Assert-Mc3 (Test-Path -LiteralPath $target -PathType Leaf) 'Target is not an ordinary file.'}
            $before=if($exists){Get-Mc3Hash $target}else{''}
            if($before-eq $row.sha256){continue}
            if($exists){Copy-Mc3Durable $target (Join-Mc3Child (Join-Path $Stage 'backup') $row.path) $before}
            $files+=[pscustomobject]@{path=$row.path;before_exists=$exists;before_sha256=$before;after_sha256=$row.sha256;state='pending'}
        }
        $journal=[pscustomobject]@{format=1;id=$token;install=$Install;state='applying';files=@($files);errors=@()}
        Write-Mc3Json $journalPath $journal
        try{
            foreach($row in $files){
                Assert-Mc3 (@(Get-Mc3Busy $Install $ParentId).Count-eq 0) 'A game/Make HD job started during update; rolling back.'
                $source=Join-Mc3Child (Join-Path $Stage 'payload') $row.path;$target=Join-Mc3Child $Install $row.path
                $exists=Test-Path -LiteralPath $target
                Assert-Mc3 ($exists-eq $row.before_exists -and (-not $exists -or (Get-Mc3Hash $target)-eq $row.before_sha256)) 'Target changed before replacement.'
                $temp=$target+'.mc3-update-'+$token+'.tmp';Copy-Mc3Durable $source $temp $row.after_sha256
                $row.state='intent';Write-Mc3Json $journalPath $journal
                if($exists){[IO.File]::Replace($temp,$target,$null)}else{[IO.File]::Move($temp,$target)}
                $row.state='applied';Write-Mc3Json $journalPath $journal
            }
            $journal.state='applied';Write-Mc3Json $journalPath $journal
        }catch{$failure=$_.Exception.Message;Restore-Mc3Transaction $Stage $journal;throw "Update rolled back: $failure"}
        return [pscustomobject]@{status='applied';source_revision=$manifest.source_revision;backups=(Join-Path $Stage 'backup');files=$files.Count}
    }finally{
        $lock.Dispose()
        if($owned -and (Test-Path -LiteralPath $lockPath) -and [IO.File]::ReadAllText($lockPath,$script:Mc3Utf8)-ceq $token){Remove-Item -LiteralPath $lockPath -Force}
    }
}
function Invoke-Mc3Updater {
    $install=Assert-Mc3NoReparse $InstallDirectory
    Assert-Mc3 (Test-Path -LiteralPath (Join-Path $install 'mc3.exe') -PathType Leaf) 'Select an existing MC3 install.'
    if($Mode-eq 'Apply'){Assert-Mc3 ([bool]$StageDirectory) 'Apply requires the explicitly selected stage directory.';return Invoke-Mc3Apply $install $StageDirectory $ParentProcessId $WaitSeconds}
    $release=Get-Mc3PublishedRelease $ReleaseId;$revision=Get-Mc3TagRevision $release
    $current=$null;$currentPath=Join-Path $install 'release-manifest.json'
    if(Test-Path -LiteralPath $currentPath){$current=Read-Mc3Json $currentPath}
    if($Mode-eq 'Check'){
        $same=$null-ne $current -and (Test-Mc3Revision ([string]$current.source_revision) $revision)
        return [pscustomobject]@{status=$(if($same){'current'}else{'available'});release_id=$release.id;tag=$release.tag_name;source_revision=$revision;
            current_source_revision=$(if($current){$current.source_revision}else{''});url=$release.html_url;message='Published runtime release only. Download and installation require separate player choices.'}
    }
    $asset=Get-Mc3Asset $release
    if(-not $StageDirectory){$StageDirectory=Join-Path $env:LOCALAPPDATA ('MidnightClub3-Port/updates/'+[Guid]::NewGuid().ToString('N'))}
    $stage=Assert-Mc3NoReparse $StageDirectory;Assert-Mc3 (-not (Test-Path -LiteralPath $stage)) 'Stage directory already exists; existing work was preserved.'
    [void][IO.Directory]::CreateDirectory($stage)
    $part=Join-Path $stage 'runtime.zip.part';Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $part -TimeoutSec 600
    $zip=Join-Path $stage 'runtime.zip';[IO.File]::Move($part,$zip)
    $verified=Read-Mc3Archive $zip $asset.digest.Substring(7) ([long]$asset.size) (Join-Path $stage 'payload');$manifest=$verified.manifest
    Assert-Mc3 (Test-Mc3Revision $manifest.source_revision $revision) 'Manifest source is not the published release tag.'
    Write-Mc3Json (Join-Path $stage 'stage.json') ([ordered]@{format=1;id=[Guid]::NewGuid().ToString('N');repo=$script:Mc3Repo;install=$install;
        release_id=$release.id;asset_id=$asset.id;digest=$asset.digest;size=[long]$asset.size;source_revision=$manifest.source_revision})
    return [pscustomobject]@{status='staged';stage=$stage;source_revision=$manifest.source_revision;message='Nothing installed. Close the game and choose Apply to install this staged release.'}
}
if(-not $LibraryOnly){try{Invoke-Mc3Updater|ConvertTo-Json -Depth 6}catch{Write-Error $_.Exception.Message;exit 1}}
