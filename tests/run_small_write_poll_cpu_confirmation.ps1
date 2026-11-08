param(
    [string]$OutputDirectory = 'tests/build/small-write-poll-cpu/final-dispatch',
    [int]$Repetitions = 21,
    [switch]$BuildOnly,
    [switch]$RunOnly
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $variants = @('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')
    $schedules = [ordered]@{
        serial = ''
        interleaved = '/DCOMPARISON_INTERLEAVED_REQUESTS /DCOMPARISON_PREPARED_INTERLEAVED'
    }
    $combinations = @()
    foreach ($schedule in $schedules.Keys) {
        foreach ($variant in $variants) {
            $combinations += "$schedule-$variant"
            if (!$RunOnly -and !(Test-Path "$rootAbsolute/cpu-$schedule-$variant/benchmark.exe")) {
                $sourceName = if ($variant -eq 'upstream') { 'upstream' } elseif ($variant.StartsWith('before')) { 'before-bench' } else { 'after-bench' }
                $library = "$rootAbsolute/$sourceName/lib"
                $flags = $schedules[$schedule]
                if ($variant.StartsWith('upstream')) {
                    $flags += " /FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018"
                } else {
                    $flags += ' /DCOMPARISON_CURRENT'
                }
                if ($variant -match '_(multi|query)$') {
                    $flags += ' /DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2'
                }
                if ($variant.EndsWith('_query')) {
                    $flags += ' /DAFATFS_READ_SESSION_QUERY=1'
                }
                foreach ($kind in @('cpu', 'io')) {
                    $directory = "$rootAbsolute/$kind-$schedule-$variant"
                    New-Item -ItemType Directory -Force $directory | Out-Null
                    $kindFlags = if ($kind -eq 'cpu') { '/DCOMPARISON_CPU_ONLY /DCOMPARISON_CPU_READ_PASSES=16' } else { '/DCOMPARISON_CPU_READ_PASSES=1 /DCOMPARISON_METADATA_TRACE' }
                    if ($kind -eq 'io' -and !$variant.StartsWith('upstream')) {
                        $kindFlags += ' /DCOMPARISON_CHECK_ALLOCATION_ORDER'
                    }
                    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /D_CRT_SECURE_NO_WARNINGS $flags $kindFlags /I`"$library`" `"$rootAbsolute/harness.c`" `"$library/asyncfatfs.c`" `"$library/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
                    if ($LASTEXITCODE) { throw "Compilation failed: $kind/$schedule/$variant" }
                }
                Write-Output "Built $schedule/$variant."
            }
        }
    }
    if ($BuildOnly) { return }
    # Finish every instrumented run before starting CPU measurements.
    foreach ($combination in $combinations) {
        $directory = "$rootAbsolute/io-$combination"
        & "$directory/benchmark.exe" 1 > "$directory/results.csv" 2> "$directory/audit.log"
        if ($LASTEXITCODE) { throw "I/O audit failed: $combination" }
        Write-Output "I/O passed: $combination."
    }
    foreach ($combination in $combinations) {
        $directory = "$rootAbsolute/cpu-$combination"
        & "$directory/benchmark.exe" 1 > "$directory/warmup.csv" 2> "$directory/warmup.log"
        if ($LASTEXITCODE) { throw "Warmup failed: $combination" }
    }
    for ($trial = 0; $trial -lt $Repetitions; $trial++) {
        $order = @($combinations)
        if ($trial % 2) { [array]::Reverse($order) }
        foreach ($combination in $order) {
            $directory = "$rootAbsolute/cpu-$combination"
            & "$directory/benchmark.exe" 1 > "$directory/trial-$trial.csv" 2> "$directory/trial-$trial.log"
            if ($LASTEXITCODE) { throw "CPU integrity failed: $combination/$trial" }
        }
        Write-Output "CPU round $($trial + 1)/$Repetitions passed."
    }
} finally {
    Pop-Location
}
