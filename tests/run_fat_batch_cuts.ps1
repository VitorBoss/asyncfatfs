param([string]$OutputDirectory = 'tests/build/fat-batch')
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($variant in @('base', 'requested_2', 'requested_4', 'requested_8')) {
        $work = "$rootAbsolute/cuts/$variant"
        New-Item -ItemType Directory -Force "$work/tests" | Out-Null
        New-Item -ItemType Directory -Force "$work/lib" | Out-Null
        Copy-Item "$rootAbsolute/raw/$variant/lib/*" "$work/lib/" -Recurse -Force
        Copy-Item tests/test_exfat.c,tests/fat32_batch_cut.c "$work/tests/"
        foreach ($files in @(1, 2)) {
            $expected = if ($variant -eq 'base') { 2 } else { [int]($variant.Split('_')[1]) * $files }
            $flags = if ($variant -eq 'base') { '' } else { '/DBATCH_CUT_PROTOTYPE' }
            $directory = "$work/files-$files"
            New-Item -ItemType Directory -Force $directory | Out-Null
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /DBATCH_CUT_FILES=$files /DEXPECTED_UNLINKED=$expected /D_CRT_SECURE_NO_WARNINGS $flags /I`"$work/lib`" `"$work/tests/fat32_batch_cut.c`" `"$work/lib/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "Cut test compilation failed: $variant/$files" }
            & "$directory/test.exe" > "$directory/results.log" 2> "$directory/errors.log"
            if ($LASTEXITCODE) { throw "Cut diagnostic failed: $variant/$files" }
            Get-Content "$directory/results.log" -Tail 1
        }
    }
} finally {
    Pop-Location
}
