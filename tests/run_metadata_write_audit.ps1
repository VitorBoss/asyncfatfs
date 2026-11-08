param(
    [string]$OutputDirectory = 'tests/build/metadata-write-audit',
    [string[]]$Variants = @('base', 'no_reservation', 'no_group', 'data_pressure', 'reserve_wait', 'upstream')
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($variant in $Variants) {
        foreach ($schedule in @('serial', 'interleaved')) {
            $directory = "$rootAbsolute/$schedule-$variant"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $flags = if ($variant -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT /DCOMPARISON_CHECK_ALLOCATION_ORDER' }
            if ($schedule -eq 'interleaved') { $flags += ' /DCOMPARISON_INTERLEAVED_REQUESTS /DCOMPARISON_PREPARED_INTERLEAVED' }
            $library = "$rootAbsolute/$variant/lib"
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_MULTIREAD_BACKEND /DCOMPARISON_CPU_ONLY /DCOMPARISON_CPU_READ_PASSES=1 /DCOMPARISON_METADATA_TRACE /DCOMPARISON_CPU_WORK_AUDIT /D_CRT_SECURE_NO_WARNINGS $flags /I`"$library`" `"$rootAbsolute/harness.c`" `"$library/asyncfatfs.c`" `"$library/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "Compilation failed: $schedule/$variant" }
            & "$directory/test.exe" 1 > "$directory/results.csv" 2> "$directory/events.log"
            if ($LASTEXITCODE) { throw "Metadata audit failed: $schedule/$variant" }
            Write-Output "$schedule/$variant`: content, FAT mirrors and applicable allocation-order checks passed."
        }
    }
} finally {
    Pop-Location
}
