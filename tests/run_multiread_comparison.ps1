param(
    [string]$OutputDirectory = 'tests/build/fat32-multiread-20261116',
    [string]$Python = 'C:/Users/user/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe',
    [int]$Repetitions = 7,
    [switch]$SkipBenchmarks,
    [switch]$OnlyAfter,
    [switch]$CurrentOnly,
    [switch]$ReadSessionQuery
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    & $Python tests/prepare_multiread_comparison.py --directory $OutputDirectory
    if ($LASTEXITCODE) { throw 'Snapshot preparation failed.' }
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $benchmarkVariants = if ($SkipBenchmarks) { @() } elseif ($OnlyAfter) { @('after') } elseif ($CurrentOnly) { @('default', 'after') } else { @('default', 'after', 'upstream_equal', 'upstream_native') }
    foreach ($variant in $benchmarkVariants) {
        $version = if ($variant -like 'upstream*') { 'upstream' } else { 'after' }
        $sourceRoot = "$rootAbsolute/$version/lib"
        $directory = "$rootAbsolute/$variant-64"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
        $flags = if ($version -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT /DCOMPARISON_CHECK_ALLOCATION_ORDER' }
        if ($variant -eq 'upstream_equal') { $flags += " /DAFATFS_EXTERNAL_CONFIG /I`"$rootAbsolute/config`"" }
        if ($variant -eq 'after') { $flags += ' /DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' }
        if ($variant -eq 'after' -and $ReadSessionQuery) { $flags += ' /DAFATFS_READ_SESSION_QUERY=1' }
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_METADATA_TRACE /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Benchmark compilation failed: $variant" }
        & "$directory/benchmark.exe" $Repetitions > "$directory/results.csv" 2> "$directory/audit.log"
        if ($LASTEXITCODE) { throw "Integrity/benchmark failed: $variant" }
        Write-Output "$variant`: benchmark/remount passed."
    }
    $memoryVariants = if ($OnlyAfter) { @('after') } elseif ($CurrentOnly) { @('default', 'after') } else { @('before', 'default', 'after') }
    foreach ($variant in $memoryVariants) {
        $version = if ($variant -eq 'before') { 'before' } else { 'after' }
        $sourceRoot = "$rootAbsolute/$version/lib"
        $flags = if ($variant -eq 'after') { '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' } else { '' }
        if ($variant -eq 'after' -and $ReadSessionQuery) { $flags += ' /DAFATFS_READ_SESSION_QUERY=1' }
        foreach ($bits in @('32', '64')) {
            $directory = "$rootAbsolute/$variant-$bits"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars$bits.bat"
            & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" `"$rootAbsolute/$variant-layout.c`" /Fo`"$directory/layout.obj`"" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw 'Host layout compilation failed.' }
            & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`"" *> "$directory/memory-build.log"
            if ($LASTEXITCODE) { throw 'Host memory compilation failed.' }
            if ($variant -ne 'default') {
                & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /D_CRT_SECURE_NO_WARNINGS /I`"$sourceRoot`" `"$rootAbsolute/$variant-backend-layout.c`" /Fo`"$directory/backend-layout.obj`"" *> "$directory/backend-build.log"
                if ($LASTEXITCODE) { throw 'Backend layout compilation failed.' }
            }
        }
        foreach ($enumMode in @('normal', 'short')) {
            $directory = "$rootAbsolute/$variant-16-$enumMode"
            New-Item -ItemType Directory -Force $directory | Out-Null
            [string[]]$targetFlags = @('-mcpu=24FJ256GA702', '-std=c99', '-Os', "-I$sourceRoot")
            if ($variant -eq 'after') { $targetFlags += '-DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' }
            if ($variant -eq 'after' -and $ReadSessionQuery) { $targetFlags += '-DAFATFS_READ_SESSION_QUERY=1' }
            if ($enumMode -eq 'short') { $targetFlags += '-fshort-enums' }
            $xc16 = 'C:/Program Files/Microchip/xc16/v2.10/bin'
            & "$xc16/xc16-gcc.exe" @targetFlags -S "$rootAbsolute/$variant-layout.c" -o "$directory/layout.s" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw 'PIC24 layout compilation failed.' }
            foreach ($source in @('asyncfatfs', 'fat_standard')) {
                & "$xc16/xc16-gcc.exe" @targetFlags -c "$sourceRoot/$source.c" -o "$directory/$source.o" *> "$directory/$source-build.log"
                if ($LASTEXITCODE) { throw 'PIC24 compilation failed.' }
                & "$xc16/xc16-objdump.exe" -h -t -d "$directory/$source.o" > "$directory/$source-inspection.txt"
            }
            if ($variant -ne 'default') {
                & "$xc16/xc16-gcc.exe" @targetFlags -S "$rootAbsolute/$variant-backend-layout.c" -o "$directory/backend-layout.s" *> "$directory/backend-build.log"
                if ($LASTEXITCODE) { throw 'PIC24 backend compilation failed.' }
            }
        }
        Write-Output "$variant`: memory probes completed."
    }
} finally {
    Pop-Location
}
