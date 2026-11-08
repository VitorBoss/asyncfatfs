param(
    [string]$Xc16Compiler = 'C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe',
    [string]$OutputDirectory = 'tests/build/bitfield-tests'
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $outputRoot = $OutputDirectory
    New-Item -ItemType Directory -Force $outputRoot | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) {
        throw 'Visual Studio C/C++ build tools were not found.'
    }
    $report = [ordered]@{
        status = 'incomplete'
        host = @()
        xc16 = @()
        xc16Available = (Test-Path -LiteralPath $Xc16Compiler)
    }
    try {
        foreach ($variant in @('default', 'fat_only', 'fat_lfn', 'exfat_only', 'exfat_freefile', 'lfn_exfat')) {
            $flags = if ($variant -eq 'default') {
                ''
            } else {
                "/DAFATFS_EXTERNAL_CONFIG /Itests\config\$variant"
            }
            foreach ($architecture in @('32', '64')) {
                $directory = "$outputRoot/$variant-$architecture"
                New-Item -ItemType Directory -Force $directory | Out-Null
                $vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars$architecture.bat"
                & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /Ilib /Itests $flags tests\bitfield_layout.c tests\sdcard_sim.c lib\fat_standard.c /Fo$directory/ /Fe$directory/probe.exe /link /INCREMENTAL:NO" *> "$directory/build.log"
                if ($LASTEXITCODE) {
                    throw "Layout probe compilation failed: $variant/$architecture. See $directory/build.log"
                }
                $lines = @(& "$directory/probe.exe")
                if ($LASTEXITCODE) {
                    throw "Flag/layout test failed: $variant/$architecture"
                }
                $report.host += [ordered]@{
                    configuration = $variant
                    bits = [int]$architecture
                    passed = $true
                    output = $lines
                }
                Write-Output "$variant/$architecture`: $($lines -join ' ')"
            }
            if ($report.xc16Available) {
                [string[]]$targetFlags = if ($variant -eq 'default') {
                    @()
                } else {
                    @('-DAFATFS_EXTERNAL_CONFIG', "-Itests/config/$variant")
                }
                foreach ($shortEnums in @($false, $true)) {
                    $suffix = if ($shortEnums) {
                        'short-enums'
                    } else {
                        'normal-enums'
                    }
                    [string[]]$enumFlags = if ($shortEnums) {
                        @('-fshort-enums')
                    } else {
                        @()
                    }
                    $assembly = "$outputRoot/$variant-16-$suffix.s"
                    & $Xc16Compiler -mcpu=24FJ256GA702 -std=c99 -Wall -Wextra -Ilib @targetFlags @enumFlags -S tests/bitfield_layout.c -o $assembly *> "$assembly.log"
                    if ($LASTEXITCODE) {
                        throw "XC16 C99 alignment/flag probe failed: $variant/$suffix. See $assembly.log"
                    }
                    $report.xc16 += [ordered]@{
                        configuration = $variant
                        bits = 16
                        shortEnums = $shortEnums
                        passed = $true
                        assembly = $assembly
                    }
                    Write-Output "$variant/16/$suffix`: C99 compilation and layout assertions passed"
                }
            }
        }
        $report.status = 'passed'
    } finally {
        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$outputRoot/report.json"
    }
} finally {
    Pop-Location
}
