param(
    [string]$Python = 'python',
    [string]$Exfatprogs = 'tests/build/external-tools/exfatprogs-1.3.2-1.exe',
    [string]$OutputDirectory = 'tests/build/powercut-journal',
    [switch]$AddressSanitizer,
    [switch]$Api
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    if ($Api -and !$PSBoundParameters.ContainsKey('OutputDirectory')) { $OutputDirectory = 'tests/build/api-powercut' }
    New-Item -ItemType Directory -Force tests/build | Out-Null
    if (!(Test-Path -LiteralPath $Exfatprogs)) { throw 'Supply -Exfatprogs. See tests/EXFAT_INTEROP.md for the pinned tools.' }
    & $Python -c "import sys; sys.path.insert(0,'tests/build/vendor'); import FATtools"
    if ($LASTEXITCODE) { throw 'Install tests/requirements-exfat.txt into tests/build/vendor first.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C/C++ build tools were not found.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $suffix = if ($AddressSanitizer) { '_asan' } else { '' }
    $binary = if ($Api) { "tests/build/test_exfat_api_powercut$suffix.exe" } else { "tests/build/test_exfat_powercut$suffix.exe" }
    $sanitizerFlags = if ($AddressSanitizer) { '/fsanitize=address /Zi /Fdtests\build\powercut_asan.pdb' } else { '' }
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /DAFATFS_EXTERNAL_CONFIG /Itests\config\exfat_only $sanitizerFlags /Ilib /Itests tests\test_exfat_powercut.c lib\asyncfatfs.c lib\fat_standard.c /Fotests\build\ /Fe$binary /link /INCREMENTAL:NO"
    if ($LASTEXITCODE) { throw 'Power-cut harness compilation failed.' }
    $savedPath = $env:PATH
    if ($AddressSanitizer) {
        $msvc = Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
        $env:PATH = (Join-Path $msvc.FullName 'bin/Hostx64/x64') + [IO.Path]::PathSeparator + $env:PATH
    }
    try {
        if ($Api) {
            & $Python tests/exfat_api_powercut.py --exfatprogs $Exfatprogs --binary $binary --output $OutputDirectory
            if ($LASTEXITCODE) { throw 'API power-cut/replay validation failed.' }
        } else {
            & $Python tests/exfat_powercut.py --exfatprogs $Exfatprogs --binary $binary --output $OutputDirectory --journal --torn
            if ($LASTEXITCODE) { throw 'Power-cut recovery validation failed.' }
            & $Python tests/exfat_journal_recovery.py --exfatprogs $Exfatprogs --binary $binary --input $OutputDirectory --output (Join-Path $OutputDirectory 'replay')
            if ($LASTEXITCODE) { throw 'Interrupted journal replay validation failed.' }
        }
    } finally { $env:PATH = $savedPath }
} finally { Pop-Location }
