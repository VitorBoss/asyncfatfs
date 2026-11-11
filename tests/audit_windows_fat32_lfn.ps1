#requires -Version 7.2
param([Parameter(Mandatory)][string]$RunName)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($RunName -notmatch '^run-[a-zA-Z0-9-]+$') {
    throw 'Invalid test run name.'
}
$runDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "build/windows-fat32-lfn/$RunName"))
$native = Get-Content -LiteralPath (Join-Path $runDirectory 'report.json') -Raw | ConvertFrom-Json
if ($native.status -ne 'passed' -or $native.cleanupErrors.Count -ne 0) {
    throw 'The native workload must finish successfully before the cleanup audit.'
}
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Fat32AuditAlias {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetShortPathNameW(string path, StringBuilder output, uint capacity);
    public static string Get(string path) {
        var output = new StringBuilder(32768);
        uint size = GetShortPathNameW(path, output, (uint)output.Capacity);
        if (size == 0 || size >= output.Capacity) {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        return output.ToString();
    }
}
'@
$report = [ordered]@{
    status = 'running'
    checkedUtc = [DateTime]::UtcNow.ToString('o')
    auditScriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath).Hash
    images = @()
}
try {
    foreach ($case in $native.cases) {
        $imagePath = [IO.Path]::GetFullPath($case.image)
        if (!$imagePath.StartsWith($runDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
            $imagePath -ne (Join-Path $runDirectory ("fat32-" + $case.name + '.vhd'))) {
            throw 'Unexpected VHD path in the report.'
        }
        if ((Get-DiskImage -ImagePath $imagePath).Attached) {
            throw 'The workload left its test VHD attached.'
        }
        $item = [ordered]@{ name = $case.name; attached = $true; footerSha256 = ''; aliases = @() }
        $report.images += $item
        try {
            Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadOnly -NoDriveLetter | Out-Null
            $disks = @(Get-DiskImage -ImagePath $imagePath | Get-Disk)
            if ($disks.Count -ne 1 -or $disks[0].IsBoot -or $disks[0].IsSystem -or $disks[0].Size -ne 512MB -or
                [string]$disks[0].BusType -notin @('File Backed Virtual', 'FileBackedVirtual', 'Virtual')) {
                throw 'Unexpected disk behind the test image.'
            }
            $volumes = @($disks[0] | Get-Partition | Where-Object Type -ne 'Reserved' | Get-Volume)
            if ($volumes.Count -ne 1 -or $volumes[0].FileSystem -ne 'FAT32' -or $volumes[0].FileSystemLabel -ne 'AFATFS_TEST') {
                throw 'Unexpected filesystem in the test VHD.'
            }
            $root = $volumes[0].Path
            $fixtures = @(
                @('Concurrent library collision filename number 00.bin', 6101, 91),
                @('Concurrent library collision filename number 01.bin', 8103, 92),
                @('Concurrent library renamed while writers pending.bin', 2300, 183),
                @('Concurrent library final roundtrip.bin', 731, 94)
            )
            $aliases = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($fixture in $fixtures) {
                $longPath = [IO.Path]::Combine($root, [string]$fixture[0])
                $shortPath = [Fat32AuditAlias]::Get($longPath)
                if (!$aliases.Add($shortPath)) {
                    throw 'Concurrent LFN outputs share a short alias.'
                }
                foreach ($path in @($longPath, $shortPath)) {
                    $data = [IO.File]::ReadAllBytes($path)
                    if ($data.Length -ne [int]$fixture[1]) {
                        throw 'Wrong concurrent file length.'
                    }
                    for ($offset = 0; $offset -lt $data.Length; $offset++) {
                        if ($data[$offset] -ne [byte](($offset * 17 + [int]$fixture[2]) -band 255)) {
                            throw 'Concurrent file content mismatch.'
                        }
                    }
                }
                $item.aliases += [ordered]@{
                    name = $fixture[0]
                    alias = [IO.Path]::GetFileName($shortPath)
                    sha256 = (Get-FileHash -LiteralPath $longPath).Hash
                }
            }
        } finally {
            if ((Get-DiskImage -ImagePath $imagePath).Attached) {
                Dismount-DiskImage -ImagePath $imagePath
            }
            $item.attached = [bool](Get-DiskImage -ImagePath $imagePath).Attached
        }
        $stream = [IO.File]::OpenRead($imagePath)
        try {
            if ($stream.Length -ne 512MB + 512) {
                throw 'Unexpected fixed VHD size.'
            }
            [void]$stream.Seek(-512, [IO.SeekOrigin]::End)
            $footer = [byte[]]::new(512)
            if ($stream.Read($footer, 0, 512) -ne 512) {
                throw 'Could not read VHD footer.'
            }
            $item.footerSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($footer))
            if ($item.footerSha256 -ne $case.footerSha256 -or $item.attached) {
                throw 'VHD footer or final detached state changed.'
            }
        } finally {
            $stream.Dispose()
        }
    }
    $report.status = 'passed'
} catch {
    $report.status = 'failed'
    $report.error = ($_ | Out-String)
    throw
} finally {
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDirectory 'cleanup-verification.json')
}
