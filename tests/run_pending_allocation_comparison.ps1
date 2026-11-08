param(
    [string]$OutputDirectory = 'tests/build/fat32-83-pending-batch-20261116',
    [int]$Repetitions = 7,
    [switch]$OnlyAfter
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    if (!(Test-Path "$OutputDirectory/before/lib/asyncfatfs.c")) {
        throw 'Capture the pre-change library under before/lib first.'
    }
    New-Item -ItemType Directory -Force "$OutputDirectory/after" | Out-Null
    Copy-Item -Recurse -Force lib "$OutputDirectory/after/"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $xc16Directory = 'C:/Program Files/Microchip/xc16/v2.10/bin'
    $variants = if ($OnlyAfter) { @('after') } else { @('before', 'after') }
    foreach ($variant in $variants) {
        $sourceRoot = "$rootAbsolute/$variant/lib"
        $layoutSource = "$rootAbsolute/$variant/layout.c"
        "#include `"$sourceRoot/asyncfatfs.c`"`nconst uint16_t comparison_layout[] = {sizeof(afatfs_t), sizeof(afatfsFile_t), sizeof(afatfsCacheBlockDescriptor_t)};" | Set-Content $layoutSource
        foreach ($bits in @('32', '64')) {
            $directory = "$rootAbsolute/$variant-$bits"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars$bits.bat"
            & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /I`"$sourceRoot`" `"$layoutSource`" /Fo`"$directory/layout.obj`"" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw "Layout compilation failed: $variant/$bits" }
            & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /I`"$sourceRoot`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`"" *> "$directory/memory-build.log"
            if ($LASTEXITCODE) { throw "Memory compilation failed: $variant/$bits" }
            if ($bits -eq '64') {
                foreach ($schedule in @('serial', 'interleaved')) {
                    $scheduleFlags = if ($schedule -eq 'interleaved') { '/DCOMPARISON_INTERLEAVED_REQUESTS' } else { '' }
                    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_CURRENT /DCOMPARISON_METADATA_TRACE /DCOMPARISON_CHECK_ALLOCATION_ORDER /D_CRT_SECURE_NO_WARNINGS $scheduleFlags /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark-$schedule.exe`" /link /INCREMENTAL:NO" *> "$directory/build-$schedule.log"
                    if ($LASTEXITCODE) { throw "Benchmark compilation failed: $variant/$schedule" }
                    & "$directory/benchmark-$schedule.exe" $Repetitions > "$directory/results-$schedule.csv" 2> "$directory/audit-$schedule.log"
                    if ($LASTEXITCODE) { throw "Benchmark failed: $variant/$schedule" }
                }
            }
        }
        foreach ($enumMode in @('normal', 'short')) {
            $directory = "$rootAbsolute/$variant-16-$enumMode"
            New-Item -ItemType Directory -Force $directory | Out-Null
            [string[]]$targetFlags = @('-mcpu=24FJ256GA702', '-std=c99', '-Os', "-I$sourceRoot")
            if ($enumMode -eq 'short') { $targetFlags += '-fshort-enums' }
            & "$xc16Directory/xc16-gcc.exe" @targetFlags -S $layoutSource -o "$directory/layout.s" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw "PIC24 layout failed: $variant/$enumMode" }
            foreach ($source in @('asyncfatfs', 'fat_standard')) {
                & "$xc16Directory/xc16-gcc.exe" @targetFlags -c "$sourceRoot/$source.c" -o "$directory/$source.o" *> "$directory/$source-build.log"
                if ($LASTEXITCODE) { throw "PIC24 build failed: $variant/$enumMode/$source" }
                & "$xc16Directory/xc16-objdump.exe" -h -t -d "$directory/$source.o" > "$directory/$source-inspection.txt"
                if ($LASTEXITCODE) { throw 'PIC24 inspection failed.' }
            }
        }
        Write-Output "$variant`: comparison completed."
    }
} finally {
    Pop-Location
}
