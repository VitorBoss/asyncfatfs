#requires -Version 7.2
param(
    [ValidateSet('all', '512', '4096', 'default')][string]$Cases = 'all',
    [switch]$ElevatedWorker,
    [switch]$NoWait,
    [switch]$CompileOnly,
    [string]$RunName = ('run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$outputRoot = Join-Path $PSScriptRoot 'build/windows-fat32-lfn'
if ($RunName -notmatch '^run-[a-zA-Z0-9-]+$') {
    throw 'Invalid test run name.'
}
$runDirectory = [IO.Path]::GetFullPath((Join-Path $outputRoot $RunName))
$reportPath = Join-Path $runDirectory 'report.json'
$binary = Join-Path $runDirectory 'workload.exe'
function Test-Administrator
{
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-TestImage([string]$ImagePath)
{
    $absolute = [IO.Path]::GetFullPath($ImagePath)
    if (!$absolute.StartsWith($runDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetExtension($absolute) -ne '.vhd') {
        throw "Image outside this test run: $absolute"
    }
    return $absolute
}

function Get-TestDisk([string]$ImagePath)
{
    $absolute = Assert-TestImage $ImagePath
    $image = Get-DiskImage -ImagePath $absolute
    if (!$image.Attached) {
        throw "Test VHD is not attached: $absolute"
    }
    $disks = @($image | Get-Disk)
    if ($disks.Count -ne 1) {
        throw 'The test image must map to exactly one disk.'
    }
    $disk = $disks[0]
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne 512MB -or
        [string]$disk.BusType -notin @('File Backed Virtual', 'FileBackedVirtual', 'Virtual')) {
        throw "Refusing unexpected disk for $absolute (number $($disk.Number), bus $($disk.BusType), size $($disk.Size))."
    }
    return $disk
}

function Get-TestVolume([string]$ImagePath)
{
    $disk = Get-TestDisk $ImagePath
    $partitions = @($disk | Get-Partition | Where-Object Type -ne 'Reserved')
    if ($partitions.Count -ne 1) {
        throw 'Expected exactly one data partition in the test VHD.'
    }
    $partition = $partitions[0]
    $volumes = @($partition | Get-Volume)
    if ($volumes.Count -ne 1) {
        throw 'Expected one volume in the test partition.'
    }
    $volume = $volumes[0]
    if ($volume.FileSystem -ne 'FAT32' -or $volume.FileSystemLabel -ne 'AFATFS_TEST' -or
        $volume.Path -notin $partition.AccessPaths -or !$volume.Path.StartsWith('\\?\Volume{')) {
        throw 'Refusing a volume that does not match the test VHD partition.'
    }
    return $volume
}

function Save-Report
{
    $script:report.updatedUtc = [DateTime]::UtcNow.ToString('o')
    $json = $script:report | ConvertTo-Json -Depth 12
    $temporary = $reportPath + '.tmp'
    [IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine)
    Move-Item -LiteralPath $temporary -Destination $reportPath -Force
}

function Set-Phase([string]$Name)
{
    $script:report.phase = $Name
    Save-Report
}

function Invoke-CheckedCommand([string]$Program, [string[]]$CommandArguments, [string]$Name)
{
    $lines = & $Program @CommandArguments 2>&1
    $code = $LASTEXITCODE
    $text = ($lines | Out-String)
    $script:report.commands += [ordered]@{
        name = $Name
        program = $Program
        arguments = $CommandArguments
        exitCode = $code
        output = $text
    }
    Save-Report
    if ($code -ne 0) {
        throw "$Name exited with $code. $text"
    }
    return $text
}

function New-Pattern([int]$Count, [int]$Tag)
{
    $bytes = [byte[]]::new($Count)
    for ($i = 0; $i -lt $Count; $i++) {
        $bytes[$i] = [byte](($i * 17 + $Tag) -band 255)
    }
    return ,$bytes
}

function Native-Path([string]$Root, [string]$Relative)
{
    # Relative names below are fixed test data. File I/O uses the volume GUID returned from the VHD.
    if ($Relative.Contains('..') -or [IO.Path]::IsPathRooted($Relative)) {
        throw 'Invalid relative fixture name.'
    }
    return [IO.Path]::Combine($Root, $Relative)
}

function Write-NativeFile([string]$Root, [string]$Name, [int]$Count, [int]$Tag)
{
    [IO.File]::WriteAllBytes((Native-Path $Root $Name), (New-Pattern $Count $Tag))
}

function Assert-NativeFile([string]$Root, [string]$Name, [int]$Count, [int]$Tag)
{
    $data = [IO.File]::ReadAllBytes((Native-Path $Root $Name))
    if ($data.Length -ne $Count) {
        throw "Wrong length for ${Name}: $($data.Length), expected $Count."
    }
    for ($i = 0; $i -lt $Count; $i++) {
        if ($data[$i] -ne [byte](($i * 17 + $Tag) -band 255)) {
            throw "Wrong data in $Name at $i."
        }
    }
}

function Get-FixedFooter([string]$ImagePath)
{
    $absolute = Assert-TestImage $ImagePath
    if ((Get-DiskImage -ImagePath $absolute).Attached) {
        throw 'The VHD must be detached before raw access.'
    }
    $stream = [IO.File]::Open($absolute, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -ne (512MB + 512)) {
            throw 'Expected a 512-MiB fixed VHD plus its footer.'
        }
        [void]$stream.Seek(-512, [IO.SeekOrigin]::End)
        $footer = [byte[]]::new(512)
        if ($stream.Read($footer, 0, 512) -ne 512 -or [Text.Encoding]::ASCII.GetString($footer, 0, 8) -ne 'conectix' -or
            $footer[60] -ne 0 -or $footer[61] -ne 0 -or $footer[62] -ne 0 -or $footer[63] -ne 2) {
            throw 'Not a fixed VHD.'
        }
        return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($footer))
    } finally {
        $stream.Dispose()
    }
}

function Check-NativeVolume([string]$ImagePath, [string]$Name)
{
    $volume = Get-TestVolume $ImagePath
    $disk = Get-TestDisk $ImagePath
    $partition = @($disk | Get-Partition | Where-Object Type -ne 'Reserved')[0]
    # CHKDSK requires an actual mount point, even though native file I/O accepts the GUID path.
    $mountDirectory = Join-Path $runDirectory ('mount-' + [IO.Path]::GetFileNameWithoutExtension($ImagePath))
    if (Test-Path -LiteralPath $mountDirectory) {
        if (((Get-Item -LiteralPath $mountDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            @([IO.Directory]::EnumerateFileSystemEntries($mountDirectory)).Count -ne 0) {
            throw 'Refusing an existing nonempty mount point.'
        }
    } else {
        [void][IO.Directory]::CreateDirectory($mountDirectory)
    }
    $accessPath = $mountDirectory + [IO.Path]::DirectorySeparatorChar
    $partition | Add-PartitionAccessPath -AccessPath $accessPath
    # No /f, /r, /x or repair options: a dirty/corrupt image must fail, not be repaired into passing.
    try {
        [void](Get-TestDisk $ImagePath)
        [void](Invoke-CheckedCommand (Join-Path $env:SystemRoot 'System32/chkdsk.exe') @($accessPath) $Name)
    } finally {
        [void](Get-TestDisk $ImagePath)
        $partition | Remove-PartitionAccessPath -AccessPath $accessPath
    }
}


if (!$ElevatedWorker) {
    if (Test-Path -LiteralPath $runDirectory) {
        throw 'Refusing to overwrite a previous run.'
    }
    New-Item -ItemType Directory -Force $runDirectory | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) {
        throw 'Visual Studio C/C++ build tools were not found.'
    }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    Push-Location $repository
    try {
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /DTEST_NATIVE_LFN /DSDCARD_SIM_TRACK_BATCHES /Ilib /Itests tests\test_exfat_external.c tests\sdcard_sim.c lib\asyncfatfs.c lib\fat_standard.c /Fo`"$runDirectory\\`" /Fe`"$binary`" /link /INCREMENTAL:NO"
        if ($LASTEXITCODE) {
            throw 'FAT32/LFN workload compilation failed.'
        }
    } finally {
        Pop-Location
    }
    if ($CompileOnly) {
        [ordered]@{ binary = $binary; report = $reportPath } | ConvertTo-Json
        return
    }
    $shell = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'), '-ElevatedWorker', '-RunName', $RunName, '-Cases', $Cases)
    $parameters = @{
        FilePath = $shell
        ArgumentList = $arguments
        WindowStyle = 'Hidden'
        PassThru = $true
    }
    if (!(Test-Administrator)) {
        $parameters.Verb = 'RunAs'
    }
    $worker = Start-Process @parameters
    [ordered]@{ processId = $worker.Id; report = $reportPath } | ConvertTo-Json
    if (!$NoWait) {
        $worker.WaitForExit()
        if ($worker.ExitCode -ne 0) {
            throw "FAT32 VHD validation failed. Read $reportPath"
        }
        Get-Content -LiteralPath $reportPath
    }
    return
}

if (!(Test-Administrator)) {
    throw 'Native VHD APIs require an elevated worker.'
}
if (!(Test-Path -LiteralPath $binary)) {
    throw 'Compile the workload using parent mode first.'
}
$script:report = [ordered]@{
    status = 'running'
    phase = 'initializing'
    startedUtc = [DateTime]::UtcNow.ToString('o')
    updatedUtc = ''
    windowsVersion = [Environment]::OSVersion.VersionString
    fatDriverVersion = (Get-Item (Join-Path $env:SystemRoot 'System32/drivers/fastfat.sys')).VersionInfo.FileVersion
    binarySha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    sourceSha256 = @{}
    cases = @()
    commands = @()
    error = $null
    cleanupErrors = @()
}
$images = [Collections.Generic.List[string]]::new()
Save-Report
try {
    foreach ($source in @(Get-ChildItem (Join-Path $repository 'lib') -File) + @(
        Get-Item -LiteralPath $PSCommandPath, (Join-Path $PSScriptRoot 'test_exfat_external.c'),
            (Join-Path $PSScriptRoot 'exfat_native_api.inc'), (Join-Path $PSScriptRoot 'fat_lfn_native.inc'),
            (Join-Path $PSScriptRoot 'sdcard_sim.c'))) {
        if ($source.Extension -in @('.c', '.h', '.inc', '.ps1')) {
            $script:report.sourceSha256[$source.FullName.Substring($repository.Length + 1)] = (Get-FileHash $source.FullName).Hash
        }
    }
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class Fat32NativeNames {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetShortPathNameW(string longPath, StringBuilder shortPath, uint size);
    public static string GetAlias(string path) {
        var output = new StringBuilder(32768);
        uint length = GetShortPathNameW(path, output, (uint)output.Capacity);
        if (length == 0 || length >= output.Capacity) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        string result = output.ToString();
        return result.Substring(result.LastIndexOf('\\') + 1);
    }
    public static void WriteConcurrent(string root, bool roundtrip) {
        using (var barrier = new System.Threading.Barrier(2)) {
            var tasks = new System.Threading.Tasks.Task[2];
            for (int index = 0; index < tasks.Length; index++) {
                int selected = index;
                tasks[index] = System.Threading.Tasks.Task.Factory.StartNew(() => {
                    string name = roundtrip
                        ? String.Format("Concurrent library collision filename number {0:D2}.bin", selected)
                        : (selected == 0 ? "Windows concurrent seed first.bin" : "Windows concurrent seed second.bin");
                    int size = roundtrip ? (selected == 0 ? 6101 : 8103) : (selected == 0 ? 33001 : 49003);
                    int tag = roundtrip ? 91 + selected : (selected == 0 ? 71 : 81);
                    byte[] data = new byte[size];
                    for (int offset = 0; offset < size; offset++) {
                        data[offset] = (byte)(offset * 17 + tag);
                    }
                    barrier.SignalAndWait();
                    using (var stream = new System.IO.FileStream(System.IO.Path.Combine(root, name),
                        System.IO.FileMode.Create, System.IO.FileAccess.Write, System.IO.FileShare.None)) {
                        for (int offset = 0; offset < size; offset += 512) {
                            stream.Write(data, offset, Math.Min(512, size - offset));
                        }
                        stream.Flush(true);
                    }
                }, System.Threading.Tasks.TaskCreationOptions.LongRunning);
            }
            System.Threading.Tasks.Task.WaitAll(tasks);
        }
    }
}
'@
    $selected = if ($Cases -eq 'all') { @('512', '4096', 'default') } else { @($Cases) }
    foreach ($case in $selected) {
        $imagePath = Assert-TestImage (Join-Path $runDirectory "fat32-$case.vhd")
        if (Test-Path -LiteralPath $imagePath) {
            throw 'Refusing to overwrite a VHD.'
        }
        $images.Add($imagePath)
        Set-Phase "create-$case"
        $diskpartScript = Join-Path $runDirectory "create-$case.txt"
        [IO.File]::WriteAllText($diskpartScript, "create vdisk file=`"$imagePath`" maximum=512 type=fixed`r`nexit`r`n", [Text.Encoding]::ASCII)
        [void](Invoke-CheckedCommand (Join-Path $env:SystemRoot 'System32/diskpart.exe') @('/s', $diskpartScript) "create-vhd-$case")
        $footer = Get-FixedFooter $imagePath
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $disk = Get-TestDisk $imagePath
        if ($disk.IsOffline) {
            $disk | Set-Disk -IsOffline $false
        }
        $disk = Get-TestDisk $imagePath
        if ($disk.PartitionStyle -ne 'RAW') {
            throw 'Only a newly created, uninitialized VHD may be initialized.'
        }
        $disk | Initialize-Disk -PartitionStyle MBR | Out-Null
        $disk = Get-TestDisk $imagePath
        $partition = $disk | New-Partition -UseMaximumSize
        if ($partition.DiskNumber -ne (Get-TestDisk $imagePath).Number) {
            throw 'Partition does not belong to the test VHD.'
        }
        $format = @{ FileSystem = 'FAT32'; NewFileSystemLabel = 'AFATFS_TEST'; Force = $true; Confirm = $false }
        if ($case -ne 'default') {
            $format.AllocationUnitSize = [uint32]$case
        }
        $partition | Format-Volume @format | Out-Null
        $volume = Get-TestVolume $imagePath
        $cluster = [int]$volume.AllocationUnitSize
        $root = $volume.Path
        $caseResult = [ordered]@{
            name = $case
            clusterSize = $cluster
            image = $imagePath
            volumePath = $root
            footerSha256 = $footer
            aliases = @()
            status = 'running'
        }
        $script:report.cases += $caseResult
        Set-Phase "native-seed-$case"
        Write-NativeFile $root 'external seed.bin' 20003 11
        Write-NativeFile $root 'external empty.bin' 0 0
        Write-NativeFile $root 'external truncate.bin' ($cluster + 99) 17
        Write-NativeFile $root 'external delete.bin' ($cluster + 7) 23
        Write-NativeFile $root 'external fragmented.bin' ($cluster * 3) 37
        [void][IO.Directory]::CreateDirectory((Native-Path $root 'External directory'))
        Write-NativeFile $root 'External directory\external child.bin' 91 49
        [Fat32NativeNames]::WriteConcurrent($root, $false)
        Assert-NativeFile $root 'Windows concurrent seed first.bin' 33001 71
        Assert-NativeFile $root 'Windows concurrent seed second.bin' 49003 81
        $manifest = [Collections.Generic.List[string]]::new()
        $nativeNames = @('Windows mixed Case long filename.txt', 'Windows truncate long filename.bin',
            'Windows delete by alias long filename.bin', 'lower.txt', (('w' * 251) + '.bin'))
        for ($index = 0; $index -lt 14; $index++) {
            $nativeNames += ('Collision native filename number {0:D2}.bin' -f $index)
        }
        for ($index = 0; $index -lt $nativeNames.Count; $index++) {
            $name = $nativeNames[$index]
            $size = 1203 + $index
            $tag = 110 + $index
            Write-NativeFile $root $name $size $tag
            $alias = [Fat32NativeNames]::GetAlias((Native-Path $root $name))
            $manifest.Add("$alias|$name|$size|$tag")
            $caseResult.aliases += [ordered]@{ name = $name; alias = $alias; size = $size; tag = $tag }
        }
        $manifestPath = Join-Path $runDirectory "aliases-$case.txt"
        [IO.File]::WriteAllLines($manifestPath, $manifest, [Text.Encoding]::ASCII)
        Check-NativeVolume $imagePath "baseline-chkdsk-$case"
        Dismount-DiskImage -ImagePath $imagePath
        Set-Phase "library-write-$case"
        if ((Get-FixedFooter $imagePath) -ne $footer) {
            throw 'VHD footer changed before raw access.'
        }
        [void](Invoke-CheckedCommand $binary @($imagePath, 'exercise', [string]$cluster) "library-generic-$case")
        [void](Invoke-CheckedCommand $binary @($imagePath, 'exercise-lfn', [string]$cluster, $manifestPath) "library-lfn-$case")
        $batchOutput = Invoke-CheckedCommand $binary @($imagePath, 'exercise-lfn-concurrent', [string]$cluster) "library-concurrent-$case"
        $batchMatch = [regex]::Match($batchOutput, 'NATIVE_BATCH,exercise-lfn-concurrent,(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)')
        if (!$batchMatch.Success -or [uint32]$batchMatch.Groups[3].Value -eq 0 -or [uint32]$batchMatch.Groups[4].Value -eq 0) {
            throw 'Concurrent workload did not exercise multi-block writes.'
        }
        $caseResult.batching = [ordered]@{
            reads = [uint32]$batchMatch.Groups[1].Value
            writes = [uint32]$batchMatch.Groups[2].Value
            multiSessions = [uint32]$batchMatch.Groups[3].Value
            multiBlocks = [uint32]$batchMatch.Groups[4].Value
            readInterrupts = [uint32]$batchMatch.Groups[5].Value
            addressInterrupts = [uint32]$batchMatch.Groups[6].Value
        }
        if ((Get-FixedFooter $imagePath) -ne $footer) {
            throw 'The library modified the VHD footer.'
        }
        Set-Phase "native-verify-$case"
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $root = (Get-TestVolume $imagePath).Path
        Check-NativeVolume $imagePath "after-library-chkdsk-$case"
        Assert-NativeFile $root 'library output.bin' ($cluster * 3 + 123) 73
        Assert-NativeFile $root (('n' * 251) + '.bin') 777 55
        Assert-NativeFile $root 'Library renamed native filename.data' 1203 110
        Assert-NativeFile $root 'Windows truncate long filename.bin' 333 111
        Assert-NativeFile $root 'LIBRARY CASE CHANGE.TXT' 193 201
        Assert-NativeFile $root '1234567890123' 27 210
        Assert-NativeFile $root '12345678901234567890123456' 27 211
        Assert-NativeFile $root 'Library nested directory\Library child directory\Child long filename.bin' ($cluster + 13) 221
        Assert-NativeFile $root 'Concurrent library collision filename number 00.bin' 65536 41
        Assert-NativeFile $root 'Concurrent library collision filename number 01.bin' (32768 + 333) 77
        Assert-NativeFile $root 'Concurrent library renamed while writers pending.bin' 2300 183
        if ([IO.File]::Exists((Native-Path $root 'Concurrent library collision filename number 02.bin'))) {
            throw 'Concurrent rename left its source visible.'
        }
        if (![IO.Directory]::Exists((Native-Path $root 'Library renamed empty directory'))) {
            throw 'Renamed empty directory is not visible.'
        }
        foreach ($name in @('Windows mixed Case long filename.txt', 'Windows delete by alias long filename.bin', 'external delete.bin')) {
            if ([IO.File]::Exists((Native-Path $root $name))) {
                throw "Deleted/renamed source remains visible: $name"
            }
        }
        for ($index = 0; $index -lt 14; $index++) {
            $name = 'Collision library filename number {0:D2}.bin' -f $index
            Assert-NativeFile $root $name (97 + $index) (160 + $index)
            $alias = [Fat32NativeNames]::GetAlias((Native-Path $root $name))
            Assert-NativeFile $root $alias (97 + $index) (160 + $index)
            $caseResult.aliases += [ordered]@{ name = $name; alias = $alias }
        }
        $actualNames = @([IO.Directory]::EnumerateFiles($root) | ForEach-Object { [IO.Path]::GetFileName($_) })
        if ($actualNames -cnotcontains 'LIBRARY CASE CHANGE.TXT') {
            throw 'Case-only rename was not preserved by the native driver.'
        }
        Set-Phase "native-roundtrip-$case"
        [Fat32NativeNames]::WriteConcurrent($root, $true)
        Write-NativeFile $root 'library output.bin' 731 231
        [IO.File]::Move((Native-Path $root 'library output.bin'), (Native-Path $root 'Windows renamed library output.data'))
        [IO.File]::Move((Native-Path $root (('n' * 251) + '.bin')), (Native-Path $root 'Windows renamed maximum filename.bin'))
        [IO.File]::Delete((Native-Path $root 'LIBRARY CASE CHANGE.TXT'))
        [IO.Directory]::Move((Native-Path $root 'Library nested directory'), (Native-Path $root 'Windows renamed nested directory'))
        Write-NativeFile $root 'Windows renamed nested directory\Library child directory\Native child created after rename.bin' 777 93
        Check-NativeVolume $imagePath "after-native-write-chkdsk-$case"
        Dismount-DiskImage -ImagePath $imagePath
        Set-Phase "library-roundtrip-$case"
        [void](Get-FixedFooter $imagePath)
        [void](Invoke-CheckedCommand $binary @($imagePath, 'verify-lfn-roundtrip', [string]$cluster) "library-lfn-roundtrip-$case")
        [void](Invoke-CheckedCommand $binary @($imagePath, 'verify-lfn-concurrent-roundtrip', [string]$cluster) "library-concurrent-roundtrip-$case")
        if ((Get-FixedFooter $imagePath) -ne $footer) {
            throw 'VHD footer changed during roundtrip.'
        }
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $root = (Get-TestVolume $imagePath).Path
        Assert-NativeFile $root 'Windows renamed nested directory\Library child directory\Library final native roundtrip.bin' 701 239
        Assert-NativeFile $root 'Concurrent library final roundtrip.bin' 731 94
        Check-NativeVolume $imagePath "final-chkdsk-$case"
        Dismount-DiskImage -ImagePath $imagePath
        $caseResult.status = 'passed'
        Save-Report
    }
    $script:report.status = 'passed'
    Set-Phase 'completed'
} catch {
    $script:report.status = 'failed'
    $script:report.error = ($_ | Out-String)
    Save-Report
} finally {
    foreach ($imagePath in $images) {
        try {
            [void](Assert-TestImage $imagePath)
            if ((Test-Path $imagePath) -and (Get-DiskImage -ImagePath $imagePath).Attached) {
                Dismount-DiskImage -ImagePath $imagePath
            }
        } catch {
            $script:report.cleanupErrors += ($_ | Out-String)
            $script:report.status = 'failed'
        }
    }
    Save-Report
}
if ($script:report.status -ne 'passed') {
    exit 1
}
