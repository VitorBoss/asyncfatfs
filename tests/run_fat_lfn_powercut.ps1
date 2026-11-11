param(
    [string]$Python = 'python',
    [string]$OutputDirectory = 'tests/build/fat-lfn-powercut/current',
    [switch]$Torn
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    & $Python -c "import sys; sys.path.insert(0,'tests/build/vendor'); import FATtools"
    if ($LASTEXITCODE) {
        throw 'Install tests/requirements-exfat.txt into tests/build/vendor first.'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) {
        throw 'Visual Studio C/C++ build tools were not found.'
    }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $binary = Join-Path $OutputDirectory 'workload.exe'
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /Ilib tests\fat_lfn_powercut.c lib\asyncfatfs.c lib\fat_standard.c /Fo$OutputDirectory/ /Fe$binary /link /INCREMENTAL:NO"
    if ($LASTEXITCODE) {
        throw 'FAT32 power-cut harness compilation failed.'
    }
    [string[]]$cutOptions = @()
    if ($Torn) {
        $cutOptions = @('--torn')
    }
    & $Python tests/fat_lfn_powercut.py --binary $binary --output $OutputDirectory @cutOptions
    if ($LASTEXITCODE) {
        throw 'FAT32 operation power-cut validation failed.'
    }
    & $Python tests/fat_lfn_recovery.py --binary $binary --matrix $OutputDirectory --output (Join-Path $OutputDirectory 'recovery')
    if ($LASTEXITCODE) {
        throw 'Interrupted FAT32 recovery validation failed.'
    }
    & $Python tests/fat_lfn_journal_guards.py --binary $binary --matrix $OutputDirectory --output (Join-Path $OutputDirectory 'guards')
    if ($LASTEXITCODE) {
        throw 'FAT32 journal guard validation failed.'
    }
    & $Python tests/fat_lfn_provision.py --binary $binary --matrix $OutputDirectory --output (Join-Path $OutputDirectory 'provision')
    if ($LASTEXITCODE) {
        throw 'FAT32 journal bootstrap validation failed.'
    }
} finally {
    Pop-Location
}
