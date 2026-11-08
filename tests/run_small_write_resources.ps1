param([string]$OutputDirectory = 'tests/build/small-write-cpu/final')
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars32.bat'
    foreach ($version in @('before', 'normal')) {
        $directory = "$rootAbsolute/resources-32-$version"
        $library = "$rootAbsolute/$version/lib"
        New-Item -ItemType Directory -Force $directory | Out-Null
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /c /D_CRT_SECURE_NO_WARNINGS /I`"$library`" `"$library/asyncfatfs.c`" `"$library/fat_standard.c`" /Fo`"$directory/`"" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Resource build failed: $version" }
        Write-Output "x86 resource build passed: $version."
    }
} finally {
    Pop-Location
}
