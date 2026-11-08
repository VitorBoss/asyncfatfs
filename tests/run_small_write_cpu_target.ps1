param([int]$Repetitions = 31)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path 'tests/build/small-write-cpu/final').Path
    foreach ($variant in @('before_multi', 'after_multi')) {
        $directory = "$rootAbsolute/target-confirm/cpu-interleaved-$variant"
        New-Item -ItemType Directory -Force $directory | Out-Null
        Copy-Item "$rootAbsolute/cpu-interleaved-$variant/benchmark.exe" "$directory/benchmark.exe" -Force
        for ($warmup = 0; $warmup -lt 3; $warmup++) {
            & "$directory/benchmark.exe" 1 > "$directory/warmup-$warmup.csv" 2> "$directory/warmup-$warmup.log"
            if ($LASTEXITCODE) { throw "Warmup failed: $variant" }
        }
    }
    for ($trial = 0; $trial -lt $Repetitions; $trial++) {
        $order = @('before_multi', 'after_multi')
        if ($trial % 2) { [array]::Reverse($order) }
        foreach ($variant in $order) {
            $directory = "$rootAbsolute/target-confirm/cpu-interleaved-$variant"
            & "$directory/benchmark.exe" 1 > "$directory/trial-$trial.csv" 2> "$directory/trial-$trial.log"
            if ($LASTEXITCODE) { throw "Target confirmation failed: $variant/$trial" }
        }
    }
    Write-Output "$Repetitions isolated paired trials passed."
} finally {
    Pop-Location
}
