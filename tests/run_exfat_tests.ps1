param([switch]$CompileLegacy)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force tests/build | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C/C++ build tools were not found.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /Ilib tests\test_exfat.c lib\asyncfatfs.c lib\fat_standard.c /Fotests\build\ /Fetests\build\test_exfat.exe /link /INCREMENTAL:NO"
    if ($LASTEXITCODE) { throw 'Compilation failed.' }
    & ./tests/build/test_exfat.exe
    if ($LASTEXITCODE) { throw 'exFAT tests failed.' }
    foreach ($variant in @('fat_only', 'exfat_freefile')) {
        $testFlags = if ($variant -eq 'fat_only') { '/DTEST_FAT_ONLY' } else { '' }
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /DAFATFS_EXTERNAL_CONFIG $testFlags /Ilib /Itests\config\$variant tests\test_exfat.c lib\asyncfatfs.c lib\fat_standard.c /Fotests\build\ /Fetests\build\test_$variant.exe /link /INCREMENTAL:NO"
        if ($LASTEXITCODE) { throw "Compilation failed for $variant." }
        & "./tests/build/test_$variant.exe"
        if ($LASTEXITCODE) { throw "Tests failed for $variant." }
    }
    if ($CompileLegacy) {
        foreach ($source in Get-ChildItem tests/test_*.c | Where-Object Name -notin @('test_exfat.c', 'test_exfat_external.c', 'test_exfat_powercut.c')) {
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /DAFATFS_DEBUG /DAFATFS_EXTERNAL_CONFIG /Ilib /Itests\config\exfat_freefile $($source.FullName) tests\common.c tests\sdcard_sim.c lib\asyncfatfs.c lib\fat_standard.c /Fotests\build\ /Fetests\build\$($source.BaseName).exe /link /INCREMENTAL:NO"
            if ($LASTEXITCODE) { throw "Compilation failed for $($source.Name)." }
        }
    }
} finally {
    Pop-Location
}
