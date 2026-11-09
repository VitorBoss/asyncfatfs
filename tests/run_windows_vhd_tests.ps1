#requires -Version 7.2
param(
    [ValidateSet('all', 'default', '4096', '65536')][string]$Cases = 'all',
    [ValidateSet('MBR', 'GPT')][string]$PartitionTable = 'MBR',
    [switch]$ElevatedWorker,
    [switch]$NoWait,
    [string]$RunName = ('run-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'build/windows-native'))
if ($RunName -notmatch '^run-[a-zA-Z0-9-]+$') { throw 'Invalid test run name.' }
$runDirectory = Join-Path $outputRoot $RunName
$reportPath = Join-Path $runDirectory 'report.json'
$binary = Join-Path $runDirectory 'workload.exe'

function Test-Administrator {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-TestImage([string]$ImagePath) {
    $absolute = [IO.Path]::GetFullPath($ImagePath)
    if (!$absolute.StartsWith($runDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetExtension($absolute) -ne '.vhd') { throw "Image outside this test run: $absolute" }
    return $absolute
}

function Get-TestDisk([string]$ImagePath) {
    $absolute = Assert-TestImage $ImagePath
    $image = Get-DiskImage -ImagePath $absolute
    if (!$image.Attached) { throw "Test VHD is not attached: $absolute" }
    $disks = @($image | Get-Disk)
    if ($disks.Count -ne 1) { throw 'The test image must map to exactly one disk.' }
    $disk = $disks[0]
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.Size -ne 256MB -or
        [string]$disk.BusType -notin @('File Backed Virtual', 'FileBackedVirtual', 'Virtual')) {
        throw "Refusing unexpected disk for $absolute (number $($disk.Number), bus $($disk.BusType), size $($disk.Size))."
    }
    return $disk
}

function Get-TestVolume([string]$ImagePath) {
    $disk = Get-TestDisk $ImagePath
    $partitions = @($disk | Get-Partition | Where-Object Type -ne 'Reserved')
    if ($partitions.Count -ne 1) { throw 'Expected exactly one data partition in the test VHD.' }
    $partition = $partitions[0]
    $volumes = @($partition | Get-Volume)
    if ($volumes.Count -ne 1) { throw 'Expected one volume in the test partition.' }
    $volume = $volumes[0]
    if ($volume.FileSystem -ne 'exFAT' -or $volume.FileSystemLabel -ne 'AFATFS_TEST' -or
        $volume.Path -notin $partition.AccessPaths -or !$volume.Path.StartsWith('\\?\Volume{')) {
        throw 'Refusing a volume that does not match the test VHD partition.'
    }
    return $volume
}

function Save-Report {
    $script:report.updatedUtc = [DateTime]::UtcNow.ToString('o')
    $json = $script:report | ConvertTo-Json -Depth 12
    $temporary = $reportPath + '.tmp'
    [IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine)
    Move-Item -LiteralPath $temporary -Destination $reportPath -Force
}

function Set-Phase([string]$Name) {
    $script:report.phase = $Name
    Save-Report
}

function Invoke-CheckedCommand([string]$Program, [string[]]$CommandArguments, [string]$Name) {
    $lines = & $Program @CommandArguments 2>&1
    $code = $LASTEXITCODE
    $text = ($lines | Out-String)
    $script:report.commands += [ordered]@{ name = $Name; program = $Program; arguments = $CommandArguments; exitCode = $code; output = $text }
    Save-Report
    if ($code -ne 0) { throw "$Name exited with $code. $text" }
    return $text
}

function New-Pattern([int]$Count, [int]$Tag) {
    $bytes = [byte[]]::new($Count)
    for ($i = 0; $i -lt $Count; $i++) { $bytes[$i] = [byte](($i * 17 + $Tag) -band 255) }
    return ,$bytes
}

function Native-Path([string]$Root, [string]$Relative) {
    # Relative names below are fixed test data. File I/O uses the volume GUID returned from the VHD.
    if ($Relative.Contains('..') -or [IO.Path]::IsPathRooted($Relative)) { throw 'Invalid relative fixture name.' }
    return [IO.Path]::Combine($Root, $Relative)
}

function Write-NativeFile([string]$Root, [string]$Name, [int]$Count, [int]$Tag) {
    [IO.File]::WriteAllBytes((Native-Path $Root $Name), (New-Pattern $Count $Tag))
}

function Assert-NativeFile([string]$Root, [string]$Name, [int]$Count, [int]$Tag) {
    $data = [IO.File]::ReadAllBytes((Native-Path $Root $Name))
    if ($data.Length -ne $Count) { throw "Wrong length for ${Name}: $($data.Length), expected $Count." }
    for ($i = 0; $i -lt $Count; $i++) {
        if ($data[$i] -ne [byte](($i * 17 + $Tag) -band 255)) { throw "Wrong data in $Name at $i." }
    }
}

function Assert-NativeAllocation([string]$Root, [string]$Name, [long]$Size, [long]$Allocated) {
    $actual = [VhdNativeInfo]::GetSizes((Native-Path $Root $Name))
    if ($actual[0] -ne $Allocated -or $actual[1] -ne $Size) {
        throw "Wrong native allocation/EOF for ${Name}: $($actual -join '/'), expected $Allocated/$Size."
    }
    $script:report.commands += [ordered]@{ name = "native-allocation-$Name"; allocationSize = $actual[0]; endOfFile = $actual[1] }
    Save-Report
}

function Assert-NativeApi([string]$Root, [int]$Cluster) {
    Assert-NativeFile $Root 'api renamed truncate.bin' ($Cluster + 17) 101
    Assert-NativeAllocation $Root 'api renamed truncate.bin' ($Cluster + 17) ($Cluster * 2)
    Assert-NativeFile $Root 'api renamed file.bin' 1203 103
    Assert-NativeFile $Root 'api native collision.bin' 33 107
    Assert-NativeFile $Root 'API renamed directory\child.bin' 87 109
    Assert-NativeFile $Root 'api renamed fragmented.bin' ($Cluster - 7) 149
    Assert-NativeAllocation $Root 'api renamed fragmented.bin' ($Cluster - 7) $Cluster
    Assert-NativeFile $Root 'api native fragment blocker.bin' $Cluster 151
    Assert-NativeFile $Root 'api prealloc clipped.bin' ($Cluster - 7) 113
    Assert-NativeAllocation $Root 'api prealloc clipped.bin' ($Cluster - 7) $Cluster
    Assert-NativeFile $Root 'api hint.bin' 0 0
    Assert-NativeAllocation $Root 'api hint.bin' 0 0
    Assert-NativeFile $Root 'api zero truncate.bin' 0 0
    Assert-NativeAllocation $Root 'api zero truncate.bin' 0 0
    foreach ($old in @('api native truncate.bin', 'api native rename.bin', 'api native fragmented.bin', 'api native empty.bin', 'api unexpected.bin')) {
        if ([IO.File]::Exists((Native-Path $Root $old))) { throw "Unexpected old/deleted name: $old" }
    }
    if ([IO.Directory]::Exists((Native-Path $Root 'API native directory'))) { throw 'Old directory name survived rename.' }
    $renamed = Native-Path $Root 'api renamed truncate.bin'
    if (([IO.File]::GetAttributes($renamed) -band [IO.FileAttributes]::Hidden) -eq 0 -or
        [IO.File]::GetLastWriteTime($renamed) -ne [DateTime]::new(2024, 1, 2, 3, 4, 6)) { throw 'Native metadata mismatch after utime/chmod.' }
    $zeros = [IO.File]::ReadAllBytes((Native-Path $Root 'api preallocated.bin'))
    if ($zeros.Length -ne $Cluster * 3 + 123 -or @($zeros | Where-Object { $_ -ne 0 }).Count) { throw 'Unwritten preallocation exposed nonzero data.' }
    Assert-NativeAllocation $Root 'api preallocated.bin' ($Cluster * 3 + 123) ($Cluster * 4)
    if ([IO.File]::ReadAllText((Native-Path $Root 'api text.txt')) -cne "value=42`nline two`n") { throw 'Native text mismatch.' }
}

function Write-NativeApiRoundtrip([string]$Root, [int]$Cluster) {
    [IO.File]::Move((Native-Path $Root 'api renamed file.bin'), (Native-Path $Root 'api windows renamed.bin'))
    $file = [IO.File]::Open((Native-Path $Root 'api renamed truncate.bin'), [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.SetLength(333); $file.Flush($true) } finally { $file.Dispose() }
    $file = [IO.File]::Open((Native-Path $Root 'api preallocated.bin'), [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $file.Position = $Cluster + 11; $data = New-Pattern 43 131; $file.Write($data, 0, $data.Length)
        $file.Position = $file.Length; $data = New-Pattern ($Cluster + 97) 137; $file.Write($data, 0, $data.Length); $file.Flush($true)
    } finally { $file.Dispose() }
    Assert-NativeAllocation $Root 'api preallocated.bin' ($Cluster * 4 + 220) ($Cluster * 5)
    [IO.File]::AppendAllText((Native-Path $Root 'api text.txt'), "native`n", [Text.UTF8Encoding]::new($false))
}

function Get-FixedFooter([string]$ImagePath) {
    $absolute = Assert-TestImage $ImagePath
    if ((Get-DiskImage -ImagePath $absolute).Attached) { throw 'The VHD must be detached before raw access.' }
    $stream = [IO.File]::Open($absolute, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        if ($stream.Length -ne (256MB + 512)) { throw 'Expected a 256-MiB fixed VHD plus its footer.' }
        [void]$stream.Seek(-512, [IO.SeekOrigin]::End)
        $footer = [byte[]]::new(512)
        if ($stream.Read($footer, 0, 512) -ne 512 -or [Text.Encoding]::ASCII.GetString($footer, 0, 8) -ne 'conectix' -or
            $footer[60] -ne 0 -or $footer[61] -ne 0 -or $footer[62] -ne 0 -or $footer[63] -ne 2) { throw 'Not a fixed VHD.' }
        return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($footer))
    } finally { $stream.Dispose() }
}

function Check-NativeVolume([string]$ImagePath, [string]$Name) {
    $volume = Get-TestVolume $ImagePath
    $disk = Get-TestDisk $ImagePath
    $partition = @($disk | Get-Partition | Where-Object Type -ne 'Reserved')[0]
    # CHKDSK requires an actual mount point, even though native file I/O accepts the GUID path.
    $mountDirectory = Join-Path $runDirectory ('mount-' + [IO.Path]::GetFileNameWithoutExtension($ImagePath))
    if (Test-Path -LiteralPath $mountDirectory) {
        if (((Get-Item -LiteralPath $mountDirectory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            @([IO.Directory]::EnumerateFileSystemEntries($mountDirectory)).Count -ne 0) { throw 'Refusing an existing nonempty mount point.' }
    } else { [void][IO.Directory]::CreateDirectory($mountDirectory) }
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
    if (Test-Path -LiteralPath $runDirectory) { throw 'Refusing to overwrite an existing test run.' }
    New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C/C++ build tools were not found.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    Push-Location $repository
    try {
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /Ilib /Itests tests\test_exfat_external.c tests\sdcard_sim.c lib\asyncfatfs.c lib\fat_standard.c /Fo`"$runDirectory\\`" /Fe`"$binary`" /link /INCREMENTAL:NO"
        if ($LASTEXITCODE) { throw 'Workload compilation failed.' }
    } finally { Pop-Location }
    $shell = (Get-Process -Id $PID).Path
    $arguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'), '-ElevatedWorker', '-RunName', $RunName, '-Cases', $Cases, '-PartitionTable', $PartitionTable)
    $parameters = @{ FilePath = $shell; ArgumentList = $arguments; WindowStyle = 'Hidden'; PassThru = $true }
    if (!(Test-Administrator)) { $parameters.Verb = 'RunAs' }
    $worker = Start-Process @parameters
    [ordered]@{ processId = $worker.Id; runDirectory = $runDirectory; report = $reportPath } | ConvertTo-Json
    if (!$NoWait) {
        $worker.WaitForExit()
        if ($worker.ExitCode -ne 0) { throw "Native VHD validation failed. Read $reportPath" }
        Get-Content -LiteralPath $reportPath
    }
    return
}

if (!(Test-Administrator)) { throw 'The Windows VHD APIs require an elevated worker.' }
if (!(Test-Path -LiteralPath $binary)) { throw 'Compile the workload using the parent mode first.' }
$script:report = [ordered]@{
    status = 'running'; phase = 'initializing'; startedUtc = [DateTime]::UtcNow.ToString('o'); updatedUtc = ''
    windowsVersion = [Environment]::OSVersion.VersionString
    exfatDriverVersion = (Get-Item -LiteralPath (Join-Path $env:SystemRoot 'System32/drivers/exfat.sys')).VersionInfo.FileVersion
    binarySha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    partitionTable = $PartitionTable
    sourceSha256 = @{}
    cases = @(); commands = @(); error = $null; cleanupErrors = @()
}
$images = [Collections.Generic.List[string]]::new()
Save-Report
try {
    foreach ($source in @(Get-ChildItem -LiteralPath (Join-Path $repository 'lib') -File) + @(
        Get-Item -LiteralPath $PSCommandPath, (Join-Path $PSScriptRoot 'test_exfat_external.c'), (Join-Path $PSScriptRoot 'exfat_native_api.inc'))) {
        if ($source.Extension -in @('.c', '.h', '.inc', '.ps1')) {
            $script:report.sourceSha256[$source.FullName.Substring($repository.Length + 1)] = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
        }
    }
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class VhdNativeInfo {
    [StructLayout(LayoutKind.Sequential)]
    private struct StandardInfo {
        public long AllocationSize;
        public long EndOfFile;
        public uint NumberOfLinks;
        public byte DeletePending;
        public byte Directory;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int kind,
        out StandardInfo info, uint size);
    public static long[] GetSizes(string path) {
        using (var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            StandardInfo info;
            if (!GetFileInformationByHandleEx(handle, 1, out info, (uint)Marshal.SizeOf<StandardInfo>()))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return new long[] {info.AllocationSize, info.EndOfFile};
        }
    }
}
'@
    $selected = if ($Cases -eq 'all') { @('4096', '65536', 'default') } else { @($Cases) }
    foreach ($case in $selected) {
        $imagePath = Assert-TestImage (Join-Path $runDirectory "native-$case.vhd")
        if (Test-Path -LiteralPath $imagePath) { throw 'Refusing to overwrite a test VHD.' }
        $images.Add($imagePath)
        Set-Phase "create-$case"
        $diskpartScript = Join-Path $runDirectory "create-$case.txt"
        [IO.File]::WriteAllText($diskpartScript, "create vdisk file=`"$imagePath`" maximum=256 type=fixed`r`nexit`r`n", [Text.Encoding]::ASCII)
        [void](Invoke-CheckedCommand (Join-Path $env:SystemRoot 'System32/diskpart.exe') @('/s', $diskpartScript) "create-vhd-$case")
        $footer = Get-FixedFooter $imagePath
        Set-Phase "format-$case"
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $disk = Get-TestDisk $imagePath
        if ($disk.IsOffline) { $disk | Set-Disk -IsOffline $false }
        $disk = Get-TestDisk $imagePath
        if ($disk.PartitionStyle -ne 'RAW') { throw 'Only a newly created, uninitialized VHD may be initialized.' }
        $disk | Initialize-Disk -PartitionStyle $PartitionTable | Out-Null
        $disk = Get-TestDisk $imagePath
        $partition = $disk | New-Partition -UseMaximumSize
        if ($partition.DiskNumber -ne (Get-TestDisk $imagePath).Number) { throw 'Partition does not belong to the test VHD.' }
        $format = @{ FileSystem = 'exFAT'; NewFileSystemLabel = 'AFATFS_TEST'; Force = $true; Confirm = $false }
        if ($case -ne 'default') { $format.AllocationUnitSize = [uint32]$case }
        $partition | Format-Volume @format | Out-Null
        $volume = Get-TestVolume $imagePath
        $cluster = [int]$volume.AllocationUnitSize
        if ($cluster -lt 512 -or $cluster -gt 65536) { throw "Unexpected allocation unit size: $cluster" }
        $caseResult = [ordered]@{ name = $case; image = $imagePath; clusterSize = $cluster; volumePath = $volume.Path; status = 'running'; footerSha256 = $footer }
        $script:report.cases += $caseResult

        Set-Phase "seed-$case"
        $root = $volume.Path
        Write-NativeFile $root 'external seed.bin' 20003 11
        Write-NativeFile $root 'external empty.bin' 0 0
        Write-NativeFile $root 'external truncate.bin' ($cluster + 99) 17
        Write-NativeFile $root 'external delete.bin' ($cluster + 7) 23
        $fragmented = [IO.File]::Open((Native-Path $root 'external fragmented.bin'), [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $first = New-Pattern $cluster 37
            $fragmented.Write($first, 0, $first.Length); $fragmented.Flush($true)
            Write-NativeFile $root 'external blocker.bin' $cluster 41
            $tail = New-Pattern ($cluster * 2) (($cluster * 17 + 37) -band 255)
            $fragmented.Write($tail, 0, $tail.Length); $fragmented.Flush($true)
        } finally { $fragmented.Dispose() }
        [void][IO.Directory]::CreateDirectory((Native-Path $root 'External directory'))
        Write-NativeFile $root 'External directory\external child.bin' 91 49
        Assert-NativeFile $root 'external fragmented.bin' ($cluster * 3) 37
        Write-NativeFile $root 'api native truncate.bin' ($cluster * 3 + 99) 101
        Write-NativeFile $root 'api native rename.bin' 1203 103
        Write-NativeFile $root 'api native collision.bin' 33 107
        Write-NativeFile $root 'api native empty.bin' 0 0
        [void][IO.Directory]::CreateDirectory((Native-Path $root 'API native directory'))
        Write-NativeFile $root 'API native directory\child.bin' 87 109
        $fragmented = [IO.File]::Open((Native-Path $root 'api native fragmented.bin'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $data = New-Pattern $cluster 149; $fragmented.Write($data, 0, $data.Length); $fragmented.Flush($true)
            Write-NativeFile $root 'api native fragment blocker.bin' $cluster 151
            $data = New-Pattern ($cluster * 2 + 99) (($cluster * 17 + 149) -band 255)
            $fragmented.Write($data, 0, $data.Length); $fragmented.Flush($true)
        } finally { $fragmented.Dispose() }
        Check-NativeVolume $imagePath "baseline-chkdsk-$case"
        Dismount-DiskImage -ImagePath $imagePath

        Set-Phase "library-exercise-$case"
        if ((Get-FixedFooter $imagePath) -ne $footer) { throw 'VHD footer changed before library access.' }
        [void](Invoke-CheckedCommand $binary @($imagePath, 'exercise', [string]$cluster) "library-exercise-$case")
        [void](Invoke-CheckedCommand $binary @($imagePath, 'exercise-api', [string]$cluster) "library-api-$case")
        if ((Get-FixedFooter $imagePath) -ne $footer) { throw 'The library modified the VHD footer.' }

        Set-Phase "native-verify-$case"
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $root = (Get-TestVolume $imagePath).Path
        Check-NativeVolume $imagePath "after-library-chkdsk-$case"
        $seed = [IO.File]::ReadAllBytes((Native-Path $root 'external seed.bin'))
        $expected = (New-Pattern 20003 11) + (New-Pattern ($cluster * 2 + 77) 21)
        if (![Linq.Enumerable]::SequenceEqual([byte[]]$seed, [byte[]]$expected)) { throw 'Native driver read incorrect appended seed data.' }
        Assert-NativeFile $root 'external truncate.bin' 333 17
        if ([IO.File]::Exists((Native-Path $root 'external delete.bin'))) { throw 'Deleted file is still visible to Windows.' }
        Assert-NativeFile $root 'library output.bin' ($cluster * 3 + 123) 73
        Assert-NativeFile $root (('n' * 251) + '.bin') 777 55
        Assert-NativeFile $root 'library empty.bin' 0 0
        $children = [int][Math]::Floor($cluster / 96) + 20
        foreach ($directory in @('External directory', 'Library directory')) {
            for ($n = 0; $n -lt $children; $n++) {
                Assert-NativeFile $root ("$directory\child{0:D4}.bin" -f $n) 37 $n
            }
        }
        Assert-NativeFile $root 'External directory\external child.bin' 91 49
        Assert-NativeApi $root $cluster

        Set-Phase "native-roundtrip-$case"
        Write-NativeFile $root 'external roundtrip.bin' ($cluster + 93) 91
        $append = [IO.File]::Open((Native-Path $root 'library output.bin'), [IO.FileMode]::Append, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $data = New-Pattern ($cluster + 93) 91; $append.Write($data, 0, $data.Length); $append.Flush($true) } finally { $append.Dispose() }
        Write-NativeApiRoundtrip $root $cluster
        Check-NativeVolume $imagePath "after-native-write-chkdsk-$case"
        Dismount-DiskImage -ImagePath $imagePath

        Set-Phase "library-roundtrip-$case"
        [void](Get-FixedFooter $imagePath)
        [void](Invoke-CheckedCommand $binary @($imagePath, 'verify-roundtrip', [string]$cluster) "library-roundtrip-$case")
        [void](Invoke-CheckedCommand $binary @($imagePath, 'verify-api-roundtrip', [string]$cluster) "library-api-roundtrip-$case")
        if ((Get-FixedFooter $imagePath) -ne $footer) { throw 'VHD footer changed during roundtrip.' }
        Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
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
            if ((Test-Path -LiteralPath $imagePath) -and (Get-DiskImage -ImagePath $imagePath).Attached) {
                Dismount-DiskImage -ImagePath $imagePath
            }
        } catch { $script:report.cleanupErrors += ($_ | Out-String); $script:report.status = 'failed' }
    }
    Save-Report
}
if ($script:report.status -ne 'passed') { exit 1 }
