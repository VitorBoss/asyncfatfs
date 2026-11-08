param(
    [string]$OutputDirectory = 'tests/build/fat32-multiread-20261116/tests',
    [string[]]$Cases = @('default', 'fat_legacy', 'fat_only', 'fat_lfn', 'exfat_only', 'exfat_freefile', 'lfn_exfat', 'focused', 'backend', 'integration', 'deferred_directory'),
    [switch]$ReadSessionQuery
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($variant in $Cases) {
        $directory = "$OutputDirectory/$variant"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2'
        if ($ReadSessionQuery) { $flags += ' /DAFATFS_READ_SESSION_QUERY=1' }
        $source = 'tests/test_exfat.c lib/asyncfatfs.c lib/fat_standard.c'
        if ($variant -in @('default', 'fat_legacy', 'fat_only', 'focused', 'deferred_directory')) { $flags += ' /DTEST_FAT_ONLY' }
        if ($variant -in @('fat_lfn', 'lfn_exfat')) { $flags += ' /DTEST_LFN' }
        if ($variant -eq 'fat_lfn') { $flags += ' /DTEST_FAT_ONLY' }
        if ($variant -notin @('default', 'focused', 'backend', 'integration', 'deferred_directory')) { $flags += " /DAFATFS_EXTERNAL_CONFIG /Itests/config/$variant" }
        if ($variant -eq 'focused') { $source = 'tests/fat32_multiread.c lib/fat_standard.c' }
        if ($variant -eq 'deferred_directory') { $source = 'tests/deferred_directory_stack.c lib/fat_standard.c' }
        if ($variant -eq 'backend') { $source = 'tests/multiread_backend.c' }
        if ($variant -eq 'integration') { $source = 'tests/multiread_integration.c lib/asyncfatfs.c lib/fat_standard.c' }
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /Ilib $source /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Compilation failed: $variant" }
        if ($variant -in @('backend', 'integration')) {
            & "$directory/test.exe" "$directory/media.img" > "$directory/results.log"
        } else {
            & "$directory/test.exe" > "$directory/results.log"
        }
        if ($LASTEXITCODE) { throw "Multi-read regression failed: $variant" }
        Write-Output "$variant`: passed."
    }
} finally {
    Pop-Location
}
