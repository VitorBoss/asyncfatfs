param(
    [string]$OutputDirectory = 'tests/build/fat32-read-ahead-20261116',
    [string]$Python = 'C:/Users/user/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe',
    [int]$Repetitions = 7
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & $Python tests/prepare_read_ahead_experiment.py --directory $OutputDirectory
    if ($LASTEXITCODE) { throw 'Experiment preparation failed.' }
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($variant in @('current', 'window2', 'window3', 'upstream_equal')) {
        $version = if ($variant -like 'window*') { 'window' } elseif ($variant -like 'upstream*') { 'upstream' } else { 'current' }
        $sourceRoot = "$rootAbsolute/$version/lib"
        $directory = "$rootAbsolute/$variant-64"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = if ($version -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018 /DAFATFS_EXTERNAL_CONFIG /I`"$rootAbsolute/config`"" } else { '/DCOMPARISON_CURRENT' }
        if ($variant -eq 'window2') { $flags += ' /DAFATFS_PREFETCH_WINDOW_SIZE=2' }
        if ($variant -eq 'window3') { $flags += ' /DAFATFS_PREFETCH_WINDOW_SIZE=3' }
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_METADATA_TRACE /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Compilation failed: $variant" }
        & "$directory/benchmark.exe" $Repetitions > "$directory/results.csv" 2> "$directory/audit.log"
        if ($LASTEXITCODE) { throw "Integrity/benchmark failed: $variant" }
        Write-Output "$variant`: benchmark and remount integrity passed."
    }
    foreach ($version in @('current', 'window')) {
        [string[]]$compilerFlags = @('-mcpu=24FJ256GA702', '-std=c99', '-Os', '-DAFATFS_PREFETCH_WINDOW_SIZE=3',
            "-I$rootAbsolute/$version/lib", '-S', "$rootAbsolute/$version/lib/asyncfatfs.c", '-o', "$rootAbsolute/$version-pic24.s")
        & 'C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe' @compilerFlags *> "$rootAbsolute/$version-pic24.log"
        if ($LASTEXITCODE) { throw "PIC24 compilation failed: $version" }
    }
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /DAFATFS_PREFETCH_WINDOW_SIZE=3 /D_CRT_SECURE_NO_WARNINGS /I`"$rootAbsolute/window/lib`" tests/fat32_prefetch_window_probe.c `"$rootAbsolute/window/lib/fat_standard.c`" /Fo`"$rootAbsolute/`" /Fe`"$rootAbsolute/window-probe.exe`" /link /INCREMENTAL:NO" *> "$rootAbsolute/window-probe-build.log"
    if ($LASTEXITCODE) { throw 'Window probe compilation failed.' }
    & "$rootAbsolute/window-probe.exe" > "$rootAbsolute/window-probe.log"
    if ($LASTEXITCODE) { throw 'Window probe failed.' }
    & $Python tests/read_ahead_report.py --directory $OutputDirectory --output tests/FAT32_READ_AHEAD.json
    if ($LASTEXITCODE) { throw 'Read-ahead validation failed.' }
} finally {
    Pop-Location
}
