Set-StrictMode -Version Latest

function Get-PortablePEInfo {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)
    $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($Path))
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 256 -or $reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows executable: $Path" }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadUInt32()
        if ($peOffset + 24 -gt $stream.Length) { throw 'Invalid PE header offset.' }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw 'Invalid PE signature.' }
        $machine = $reader.ReadUInt16()
        $sectionCount = $reader.ReadUInt16()
        $stream.Position = $peOffset + 20
        $optionalSize = $reader.ReadUInt16()
        $characteristics = $reader.ReadUInt16()
        $optionalOffset = $peOffset + 24
        $magic = $reader.ReadUInt16()
        if ($machine -ne 0x8664 -or $magic -ne 0x20b) { throw 'The portable release requires a Windows x64 PE32+ build.' }
        if ($optionalSize -lt 224 -or $sectionCount -gt 128) { throw 'Invalid PE optional header or sections.' }
        $stream.Position = $optionalOffset + 120
        $importRva = $reader.ReadUInt32()
        $importSize = $reader.ReadUInt32()
        $sections = @()
        for ($index = 0; $index -lt $sectionCount; $index++) {
            $stream.Position = $optionalOffset + $optionalSize + $index * 40 + 8
            $sections += [pscustomobject]@{
                VirtualSize = $reader.ReadUInt32()
                Rva = $reader.ReadUInt32()
                RawSize = $reader.ReadUInt32()
                Offset = $reader.ReadUInt32()
            }
        }
        $rvaOffset = {
            param([uint32]$rva)
            foreach ($section in $sections) {
                if ($rva -ge $section.Rva -and [uint64]$rva -lt [uint64]$section.Rva + [Math]::Max($section.VirtualSize, $section.RawSize)) {
                    $offset = [uint64]$section.Offset + $rva - $section.Rva
                    if ($offset -ge $stream.Length) { throw 'A PE RVA points outside its file.' }
                    return [long]$offset
                }
            }
            throw 'A PE RVA has no matching section.'
        }
        $imports = @()
        if ($importRva) {
            $descriptorOffset = & $rvaOffset $importRva
            $limit = [Math]::Min(4096, [Math]::Max(1, [int]($importSize / 20)))
            for ($index = 0; $index -lt $limit; $index++) {
                $stream.Position = $descriptorOffset + $index * 20
                $originalThunk = $reader.ReadUInt32()
                $timestamp = $reader.ReadUInt32()
                $forwarder = $reader.ReadUInt32()
                $nameRva = $reader.ReadUInt32()
                $firstThunk = $reader.ReadUInt32()
                if (-not ($originalThunk -or $timestamp -or $forwarder -or $nameRva -or $firstThunk)) { break }
                if (-not $nameRva) { throw 'PE import descriptor has no DLL name.' }
                $stream.Position = & $rvaOffset $nameRva
                $name = [Text.StringBuilder]::new()
                for ($character = 0; $character -lt 512; $character++) {
                    $byte = $reader.ReadByte()
                    if (-not $byte) { break }
                    [void]$name.Append([char]$byte)
                }
                if ($character -eq 512 -or $name.Length -eq 0 -or $name.ToString() -notmatch '^[A-Za-z0-9_.-]+\.dll$') { throw 'Invalid PE DLL name.' }
                $imports += $name.ToString().ToLowerInvariant()
            }
        }
        return [pscustomobject]@{ Machine = 'AMD64'; IsDll = [bool]($characteristics -band 0x2000); Imports = @($imports | Sort-Object -Unique) }
    } finally { $reader.Dispose() }
}

function Find-PortableCRT {
    [CmdletBinding()]
    param([string]$Directory)
    if ($Directory) {
        $candidate = [IO.Path]::GetFullPath($Directory)
        if (-not (Test-Path -LiteralPath (Join-Path $candidate 'vcruntime140.dll') -PathType Leaf)) { throw 'CRTDirectory must contain the x64 Microsoft Visual C++ redistributable DLLs.' }
        return $candidate
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio redistributable not found. Pass -CRTDirectory explicitly.' }
    $installations = @(& $vswhere -products '*' -property installationPath)
    if (-not $installations.Count) { throw 'Visual Studio installation not found. Pass -CRTDirectory explicitly.' }
    $candidates = @()
    foreach ($installation in $installations) {
        $redist = Join-Path $installation 'VC/Redist/MSVC'
        $versions = @(Get-ChildItem -LiteralPath $redist -Directory -ErrorAction SilentlyContinue | Where-Object Name -Match '^\d+\.\d+\.\d+$')
        foreach ($version in $versions) {
            $runtimes = @(Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue)
            foreach ($runtime in $runtimes) {
                if (Test-Path -LiteralPath (Join-Path $runtime.FullName 'vcruntime140.dll') -PathType Leaf) { $candidates += [pscustomobject]@{ Version = [version]$version.Name; Directory = $runtime.FullName } }
            }
        }
    }
    if ($candidates.Count) { return ($candidates | Sort-Object Version -Descending | Select-Object -First 1).Directory }
    throw 'No x64 Microsoft Visual C++ runtime found. Pass -CRTDirectory explicitly.'
}

function Test-PortableWindowsDll {
    param([Parameter(Mandatory)][string]$Name)
    # Windows 10/11 x64 provides these. Keep this explicit: a missing third-party DLL must fail packaging.
    return $Name -match '^(api-ms-win-|ext-ms-win-)' -or $Name -in @(
        'advapi32.dll', 'avrt.dll', 'bcrypt.dll', 'cfgmgr32.dll', 'combase.dll', 'comctl32.dll', 'comdlg32.dll',
        'crypt32.dll', 'd3d12.dll', 'd3dcompiler_47.dll', 'dbghelp.dll', 'devobj.dll', 'dinput8.dll', 'dnsapi.dll',
        'dwmapi.dll', 'dxgi.dll', 'gdi32.dll', 'hid.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll', 'ksuser.dll',
        'mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mmdevapi.dll', 'msacm32.dll', 'msimg32.dll', 'msvcrt.dll',
        'ncrypt.dll', 'netapi32.dll', 'normaliz.dll', 'ntdll.dll', 'ole32.dll', 'oleacc.dll', 'oleaut32.dll',
        'powrprof.dll', 'propsys.dll', 'psapi.dll', 'rpcrt4.dll', 'secur32.dll', 'setupapi.dll', 'shell32.dll',
        'shlwapi.dll', 'user32.dll', 'userenv.dll', 'ucrtbase.dll', 'usp10.dll', 'uxtheme.dll', 'version.dll',
        'win32u.dll', 'windowscodecs.dll', 'winhttp.dll', 'winmm.dll', 'winspool.drv', 'wintrust.dll',
        'ws2_32.dll', 'wtsapi32.dll', 'xaudio2_9.dll', 'xinput1_4.dll'
    )
}

function New-PortableContentArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$Files,
        [Parameter(Mandatory)][string]$OutputDirectory,
        [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$Version,
        [ValidateRange(1, 1900)][int]$ChunkSizeMiB = 1800
    )
    $utf8 = [Text.UTF8Encoding]::new($false)
    $entries = @($Files)
    if (-not $entries.Count -or $entries.Count -gt 200000) { throw 'Invalid content file count.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries) {
        if ($entry.Target -notmatch '^(game/[^/]+\.iso|textures/replace/[0-9a-fA-F]{16}\.png|video_hd/.+\.mp4)$' -or
            $entry.Target -match '[\x00-\x1f:\\<>"|?*]' -or $entry.Target -match '(^|/)\.\.?(/|$)' -or
            -not $seen.Add($entry.Target)) { throw 'Unsafe or duplicate content target.' }
        $source = Get-Item -LiteralPath $entry.Source
        if ($source.PSIsContainer -or ($source.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Content source must be a regular non-linked file.' }
    }
    $output = [IO.Path]::GetFullPath($OutputDirectory)
    [void][IO.Directory]::CreateDirectory($output)
    $prefix = "mc3-content-$Version"
    $manifestPath = Join-Path $output 'content-manifest.tsv'
    $inventoryPath = Join-Path $output 'content-files.tsv'
    if ((Test-Path -LiteralPath $manifestPath) -or (Test-Path -LiteralPath (Join-Path $output "$prefix.001.mc3c"))) { throw 'Content output already exists; use a new output directory/version.' }
    $script:bundle = @{
        Limit = [long]$ChunkSizeMiB * 1MB; Index = 0; Stream = $null; Hash = $null; ChunkBytes = [long]0; TotalBytes = [long]0
        ArchiveHash = [Security.Cryptography.SHA256]::Create(); Chunks = [Collections.Generic.List[object]]::new()
    }
    function Close-BundleChunk {
        if ($null -eq $script:bundle.Stream) { return }
        $script:bundle.Stream.Flush($true)
        $script:bundle.Stream.Dispose()
        [void]$script:bundle.Hash.TransformFinalBlock([byte[]]@(), 0, 0)
        $hash = ([BitConverter]::ToString($script:bundle.Hash.Hash)).Replace('-', '').ToLowerInvariant()
        $script:bundle.Hash.Dispose()
        $script:bundle.Chunks.Add([pscustomobject]@{ Name = $script:bundle.Name; Size = $script:bundle.ChunkBytes; Hash = $hash })
        $script:bundle.Stream = $null
    }
    function Write-BundleBytes([byte[]]$Buffer, [int]$Count = -1) {
        if ($Count -lt 0) { $Count = $Buffer.Length }
        $offset = 0
        while ($Count -gt 0) {
            if ($null -eq $script:bundle.Stream) {
                $script:bundle.Index++
                $script:bundle.Name = '{0}.{1:000}.mc3c' -f $prefix, $script:bundle.Index
                $script:bundle.Stream = [IO.File]::Open((Join-Path $output $script:bundle.Name), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                $script:bundle.Hash = [Security.Cryptography.SHA256]::Create()
                $script:bundle.ChunkBytes = [long]0
            }
            $take = [int][Math]::Min($Count, $script:bundle.Limit - $script:bundle.ChunkBytes)
            $script:bundle.Stream.Write($Buffer, $offset, $take)
            [void]$script:bundle.Hash.TransformBlock($Buffer, $offset, $take, $Buffer, $offset)
            [void]$script:bundle.ArchiveHash.TransformBlock($Buffer, $offset, $take, $Buffer, $offset)
            $script:bundle.ChunkBytes += $take
            $script:bundle.TotalBytes += $take
            $offset += $take
            $Count -= $take
            if ($script:bundle.ChunkBytes -eq $script:bundle.Limit) { Close-BundleChunk }
        }
    }
    $inventory = [Collections.Generic.List[string]]::new()
    $inventory.Add("MC3-CONTENT-FILES`t1")
    try {
        Write-BundleBytes ([Text.Encoding]::ASCII.GetBytes('MC3C0001'))
        Write-BundleBytes ([BitConverter]::GetBytes([uint32]$entries.Count))
        $buffer = [byte[]]::new(1MB)
        $index = 0
        foreach ($file in $entries) {
            $index++
            $before = Get-Item -LiteralPath $file.Source
            Write-Progress -Activity 'Preparing complete game content' -Status "$index / $($entries.Count): $($file.Target)" -PercentComplete ($index * 100 / $entries.Count)
            $hash = (Get-FileHash -LiteralPath $file.Source -Algorithm SHA256).Hash.ToLowerInvariant()
            $pathBytes = $utf8.GetBytes($file.Target)
            $hashBytes = [byte[]]::new(32)
            for ($byte = 0; $byte -lt 32; $byte++) { $hashBytes[$byte] = [Convert]::ToByte($hash.Substring($byte * 2, 2), 16) }
            Write-BundleBytes ([BitConverter]::GetBytes([uint32]$pathBytes.Length))
            Write-BundleBytes ([BitConverter]::GetBytes([uint64]$before.Length))
            Write-BundleBytes $hashBytes
            Write-BundleBytes $pathBytes
            $inputFile = [IO.File]::OpenRead($file.Source)
            try {
                while (($read = $inputFile.Read($buffer, 0, $buffer.Length)) -gt 0) { Write-BundleBytes $buffer $read }
            } finally { $inputFile.Dispose() }
            $after = Get-Item -LiteralPath $file.Source
            if ($after.Length -ne $before.Length -or $after.LastWriteTimeUtc -ne $before.LastWriteTimeUtc) { throw "Content changed while being packaged: $($file.Target)" }
            $inventory.Add("$($file.Target)`t$($before.Length)`t$hash")
        }
        Close-BundleChunk
        [void]$script:bundle.ArchiveHash.TransformFinalBlock([byte[]]@(), 0, 0)
        $archiveHash = ([BitConverter]::ToString($script:bundle.ArchiveHash.Hash)).Replace('-', '').ToLowerInvariant()
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add("MC3-CONTENT`t1")
        $lines.Add("archive`t$($script:bundle.TotalBytes)`t$archiveHash")
        foreach ($chunk in $script:bundle.Chunks) { $lines.Add("chunk`t$($chunk.Name)`t$($chunk.Size)`t$($chunk.Hash)") }
        [IO.File]::WriteAllText($manifestPath, ($lines -join "`n") + "`n", $utf8)
        [IO.File]::WriteAllText($inventoryPath, ($inventory -join "`n") + "`n", $utf8)
        Write-Progress -Activity 'Preparing complete game content' -Completed
        [pscustomobject]@{ Manifest = $manifestPath; ManifestSHA256 = (Get-FileHash $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant(); Files = $entries.Count; Chunks = $script:bundle.Chunks.Count; ArchiveBytes = $script:bundle.TotalBytes; Inventory = $inventoryPath }
    } finally {
        if ($script:bundle.Stream) { $script:bundle.Stream.Dispose() }
        if ($script:bundle.Hash) { $script:bundle.Hash.Dispose() }
        $script:bundle.ArchiveHash.Dispose()
    }
}

Export-ModuleMember -Function Get-PortablePEInfo, Find-PortableCRT, Test-PortableWindowsDll, New-PortableContentArchive
