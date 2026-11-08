param([string]$OutputDirectory = 'tests/build/small-write-poll-cpu/final')
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $probe = Get-Content tests/bitfield_layout.c -Raw
    foreach ($version in @('before', 'normal')) {
        $library = "$rootAbsolute/$version/lib"
        foreach ($architecture in @('32', '64')) {
            $directory = "$rootAbsolute/layout-$version-$architecture"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $source = $probe.Replace('#include "../lib/asyncfatfs.c"', "#include `"$library/asyncfatfs.c`"")
            Set-Content "$directory/probe.c" $source
            $vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars$architecture.bat"
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS /I`"$library`" /Itests `"$directory/probe.c`" tests/sdcard_sim.c `"$library/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/probe.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "Layout build failed: $version/$architecture" }
            & "$directory/probe.exe" > "$directory/results.log"
            if ($LASTEXITCODE) { throw "Layout checks failed: $version/$architecture" }
            Write-Output "Layout/flags passed: $version/$architecture."
        }
        foreach ($enums in @('normal', 'short')) {
            $directory = "$rootAbsolute/layout-$version-16-$enums"
            New-Item -ItemType Directory -Force $directory | Out-Null
            Copy-Item "$rootAbsolute/layout-$version-64/probe.c" "$directory/probe.c" -Force
            [string[]]$flags = if ($enums -eq 'short') { @('-fshort-enums') } else { @() }
            & 'C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe' -mcpu=24FJ256GA702 -std=c99 -Os "-I$library" @flags -S "$directory/probe.c" -o "$directory/layout.s" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "XC16 layout failed: $version/$enums" }
            Write-Output "XC16 layout/alignment compiled: $version/$enums."
        }
    }
} finally {
    Pop-Location
}
