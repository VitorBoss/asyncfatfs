param(
    [string]$OutputDirectory = 'tests/build/fat32-read-cpu-20261116',
    [int]$Repetitions = 7,
    [int]$ReadPasses = 32,
    [switch]$BuildOnly,
    [switch]$RunOnly,
    [string[]]$Variants = @('before_default', 'default', 'before', 'before_query', 'after', 'after_no_query', 'upstream_equal', 'upstream_native')
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    if (!$RunOnly) {
    foreach ($variant in $Variants) {
        $version = if ($variant -like 'before*') { 'before' } elseif ($variant -like 'upstream*') { 'upstream' } else { 'after' }
        if ($variant -in @('reference_query', 'reference_no_query', 'reference_default')) { $version = 'reference' }
        $sourceRoot = "$rootAbsolute/$version/lib"
        $directory = "$rootAbsolute/cpu-$variant"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = if ($version -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT' }
        if ($variant -eq 'upstream_equal') { $flags += " /DAFATFS_EXTERNAL_CONFIG /I`"$rootAbsolute/config`"" }
        if ($variant -in @('before', 'before_query', 'reference_query', 'reference_no_query', 'after', 'after_no_query')) { $flags += ' /DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' }
        if ($variant -in @('after', 'before_query', 'reference_query')) { $flags += ' /DAFATFS_READ_SESSION_QUERY=1' }
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_CPU_ONLY /DCOMPARISON_CPU_READ_PASSES=$ReadPasses /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "CPU benchmark compilation failed: $variant" }
    }
    }
    if ($BuildOnly) { return }
    # Run paired trials, reversing their order on alternate rounds.
    for ($repetition = 0; $repetition -lt $Repetitions; $repetition++) {
        $order = @('before_default', 'reference_default', 'default', 'before', 'reference_no_query', 'before_query', 'reference_query', 'after', 'upstream_equal', 'upstream_native', 'after_no_query') |
            Where-Object { $_ -in $Variants }
        if ($repetition % 2) { [array]::Reverse($order) }
        foreach ($variant in $order) {
            $directory = "$rootAbsolute/cpu-$variant"
            & "$directory/benchmark.exe" 1 > "$directory/trial-$repetition.csv" 2> "$directory/trial-$repetition.log"
            if ($LASTEXITCODE) { throw "CPU integrity/benchmark failed: $variant/$repetition" }
        }
    }
    foreach ($variant in $Variants) {
        $directory = "$rootAbsolute/cpu-$variant"
        $rows = @()
        $audit = @()
        for ($repetition = 0; $repetition -lt $Repetitions; $repetition++) {
            foreach ($row in (Import-Csv "$directory/trial-$repetition.csv")) {
                $row.repetition = "$repetition"
                $rows += $row
            }
            foreach ($line in (Get-Content "$directory/trial-$repetition.log")) {
                if ($line.StartsWith('READ_BATCH,') -or $line.StartsWith('CPU_CYCLES,')) {
                    $fields = $line.Split(',')
                    $fields[6] = "$repetition"
                    $line = $fields -join ','
                }
                $audit += $line
            }
        }
        $rows | Export-Csv "$directory/results.csv" -NoTypeInformation -Encoding utf8
        $audit | Set-Content "$directory/audit.log" -Encoding utf8
        Write-Output "$variant`: $Repetitions repetitions, $ReadPasses read passes passed."
    }
} finally {
    Pop-Location
}
