param(
    [string]$OutputDirectory = 'tests/build/reserve-flush',
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
    $variants = @()
    foreach ($schedule in @('serial', 'prepared')) {
        foreach ($profile in @('default', 'multi', 'query')) {
            foreach ($version in @('before', 'after', 'upstream')) {
                $variant = "$schedule-$profile-$version"
                $variants += $variant
                if (!$RunOnly) {
                    $sourceRoot = if ($version -eq 'upstream') { "$rootAbsolute/upstream/lib" } else { "$rootAbsolute/cpu-$version-source/lib" }
                    $flags = if ($version -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT' }
                    if ($schedule -eq 'prepared') { $flags += ' /DCOMPARISON_INTERLEAVED_REQUESTS /DCOMPARISON_PREPARED_INTERLEAVED' }
                    if ($version -ne 'upstream' -and $profile -ne 'default') { $flags += ' /DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' }
                    if ($version -ne 'upstream' -and $profile -eq 'query') { $flags += ' /DAFATFS_READ_SESSION_QUERY=1' }
                    foreach ($kind in @('cpu', 'audit')) {
                        $directory = "$rootAbsolute/$kind-$variant"
                        New-Item -ItemType Directory -Force $directory | Out-Null
                        $auditFlags = if ($kind -eq 'audit') { '/DCOMPARISON_METADATA_TRACE /DCOMPARISON_CPU_WORK_AUDIT' } else { '' }
                        if ($kind -eq 'audit' -and $version -ne 'upstream') { $auditFlags += ' /DCOMPARISON_CHECK_ALLOCATION_ORDER' }
                        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_CPU_ONLY /DCOMPARISON_CPU_READ_PASSES=32 /D_CRT_SECURE_NO_WARNINGS $flags $auditFlags /I`"$sourceRoot`" `"$rootAbsolute/harness.c`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
                        if ($LASTEXITCODE) { throw "Compilation failed: $kind/$variant" }
                    }
                }
            }
        }
    }
    if (!$RunOnly) {
        $vcvars32 = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars32.bat'
        foreach ($version in @('before', 'after', 'upstream')) {
            $sourceRoot = if ($version -eq 'upstream') { "$rootAbsolute/upstream/lib" } else { "$rootAbsolute/cpu-$version-source/lib" }
            $directory = "$rootAbsolute/resources-x86-$version"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $flags = if ($version -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT' }
            & cmd /c "`"$vcvars32`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /c /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`"" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "x86 resources compilation failed: $version" }
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
        Write-Output "CPU paired trial $trial passed."
    }
    foreach ($variant in $variants) {
        $directory = "$rootAbsolute/audit-$variant"
        & "$directory/benchmark.exe" 1 > "$directory/trial-0.csv" 2> "$directory/trial-0.log"
        if ($LASTEXITCODE) { throw "Metadata/order audit failed: $variant" }
    }
} finally {
    Pop-Location
}
