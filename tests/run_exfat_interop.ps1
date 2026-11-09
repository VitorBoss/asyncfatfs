param(
    [string]$Python = 'python',
    [string]$Exfatprogs = 'tests/build/external-tools/exfatprogs-1.3.2-1.exe',
    [ValidateSet('x64', 'x86')][string]$Architecture = 'x64',
    [ValidateSet('mbr', 'gpt')][string]$PartitionTable = 'mbr',
    [switch]$AddressSanitizer
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force tests/build | Out-Null
    if (!(Test-Path -LiteralPath $Exfatprogs)) { throw 'Supply -Exfatprogs with the Windows multicall exfatprogs executable. See tests/EXFAT_INTEROP.md.' }
    & $Python -c "import sys; sys.path.insert(0,'tests/build/vendor'); import FATtools"
    if ($LASTEXITCODE) { throw 'Install FATtools==1.1.23 in tests/build/vendor. See tests/EXFAT_INTEROP.md.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C/C++ build tools were not found.' }
    $vcvarsName = if ($Architecture -eq 'x64') { 'vcvars64.bat' } else { 'vcvars32.bat' }
    $vcvars = Join-Path $vs "VC/Auxiliary/Build/$vcvarsName"
    $suffix = if ($AddressSanitizer) { "${Architecture}_asan" } else { $Architecture }
    $binary = "tests/build/test_exfat_external_$suffix.exe"
    $sanitizerFlags = if ($AddressSanitizer) { "/fsanitize=address /Zi /Fdtests\build\interop_$suffix.pdb" } else { '' }
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /DAFATFS_EXTERNAL_CONFIG /Itests\config\exfat_only $sanitizerFlags /Ilib /Itests tests\test_exfat_external.c tests\sdcard_sim.c lib\asyncfatfs.c lib\fat_standard.c /Fotests\build\ /Fe$binary /link /INCREMENTAL:NO"
    if ($LASTEXITCODE) { throw 'Interoperability test compilation failed.' }
    if ($AddressSanitizer) {
        # The ASan DLL is installed beside the compiler, rather than beside the test executable.
        $msvc = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
        $savedPath = $env:PATH
        $env:PATH = (Join-Path $msvc.FullName "bin/Hostx64/$Architecture") + [IO.Path]::PathSeparator + $env:PATH
    }
    try {
        $outputSuffix = if ($PartitionTable -eq 'gpt') { "$suffix-gpt" } else { $suffix }
        & $Python tests/exfat_interop.py --exfatprogs $Exfatprogs --binary $binary --partition-table ($PartitionTable.ToLowerInvariant()) --output "tests/build/external-$outputSuffix"
        if ($LASTEXITCODE) { throw 'External exFAT interoperability tests failed.' }
    } finally {
        if ($AddressSanitizer) { $env:PATH = $savedPath }
    }
} finally {
    Pop-Location
}
