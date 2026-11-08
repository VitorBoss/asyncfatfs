param(
    [string]$OutputDirectory = 'tests/build/io-cpu-relation',
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
    $schedules = [ordered]@{
        serial = ''
        interleaved = '/DCOMPARISON_INTERLEAVED_REQUESTS'
        prepared = '/DCOMPARISON_INTERLEAVED_REQUESTS /DCOMPARISON_PREPARED_INTERLEAVED'
    }
    $variants = @()
    foreach ($schedule in $schedules.Keys) {
        foreach ($version in @('current', 'upstream')) {
            $variants += "$schedule-$version"
            if (!$RunOnly) {
                $sourceRoot = "$rootAbsolute/$version/lib"
                $flags = $schedules[$schedule]
                if ($version -eq 'current') {
                    $flags += ' /DCOMPARISON_CURRENT'
                } else {
                    $flags += " /FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018"
                }
                foreach ($kind in @('cpu', 'audit')) {
                    $directory = "$rootAbsolute/$kind-$schedule-$version"
                    New-Item -ItemType Directory -Force $directory | Out-Null
                    $auditFlags = if ($kind -eq 'audit') { '/DCOMPARISON_METADATA_TRACE /DCOMPARISON_CPU_WORK_AUDIT' } else { '' }
                    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_CPU_ONLY /DCOMPARISON_CPU_READ_PASSES=32 /D_CRT_SECURE_NO_WARNINGS $flags $auditFlags /I`"$sourceRoot`" `"$rootAbsolute/harness.c`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
                    if ($LASTEXITCODE) { throw "Compilation failed: $kind/$schedule/$version" }
                }
            }
        }
    }
    if ($BuildOnly) { return }
    for ($trial = 0; $trial -lt $Repetitions; $trial++) {
        $order = @($variants)
        if ($trial % 2) { [array]::Reverse($order) }
        foreach ($variant in $order) {
            $directory = "$rootAbsolute/cpu-$variant"
            & "$directory/benchmark.exe" 1 > "$directory/trial-$trial.csv" 2> "$directory/trial-$trial.log"
            if ($LASTEXITCODE) { throw "CPU/integrity check failed: $variant/$trial" }
        }
    }
    foreach ($variant in $variants) {
        $directory = "$rootAbsolute/audit-$variant"
        & "$directory/benchmark.exe" 1 > "$directory/trial-0.csv" 2> "$directory/trial-0.log"
        if ($LASTEXITCODE) { throw "Work/metadata audit failed: $variant" }
        Write-Output "$variant`: $Repetitions CPU trials and independent work/metadata audit passed."
    }
} finally {
    Pop-Location
}
