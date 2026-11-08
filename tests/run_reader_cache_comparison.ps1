param(
    [string]$OutputDirectory = 'tests/build/fat32-two-readers-cache-20261116',
    [string]$Python = 'C:/Users/user/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    # before/lib is the preserved old policy with the same DMA-pointer fix.
    & tests/run_multiread_comparison.ps1 -OutputDirectory $OutputDirectory -Python $Python
    if ($LASTEXITCODE) { throw 'Comparison failed.' }
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($version in @('before', 'after')) {
        $sourceRoot = "$rootAbsolute/$version/lib"
        $directory = "$rootAbsolute/reuse-$version"
        New-Item -ItemType Directory -Force $directory | Out-Null
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_CURRENT /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_READ_CACHE_AUDIT /DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2 /D_CRT_SECURE_NO_WARNINGS /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Read-reuse compilation failed: $version" }
        & "$directory/test.exe" 7 > "$directory/results.csv" 2> "$directory/audit.log"
        if ($LASTEXITCODE) { throw "Read-reuse audit failed: $version" }
    }
    & $Python tests/two_reader_cache_report.py --directory $OutputDirectory --output tests/FAT32_TWO_READER_CACHE.json
    if ($LASTEXITCODE) { throw 'Cache/RAM/400-byte stack validation failed.' }
} finally {
    Pop-Location
}
