param(
    [string]$OutputDirectory = 'tests/build/small-write-pipeline',
    [string]$AfterVersion = 'normal'
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $original = Get-Content tests/fat32_small_sync.c -Raw
    $harness = (Resolve-Path tests/test_exfat.c).Path.Replace('\', '/')
    foreach ($version in @('before', $AfterVersion)) {
        $library = "$rootAbsolute/$version/lib"
        $source = $original.Replace('#include "test_exfat.c"', "#include `"$harness`"").Replace('#include "../lib/asyncfatfs.c"', "#include `"$library/asyncfatfs.c`"")
        foreach ($profile in @('default', 'multi', 'query')) {
            $directory = "$rootAbsolute/sync-$version-$profile"
            New-Item -ItemType Directory -Force $directory | Out-Null
            Set-Content "$directory/test.c" $source
            $flags = if ($profile -eq 'default') { '' } elseif ($profile -eq 'multi') { '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2' } else { '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2 /DAFATFS_READ_SESSION_QUERY=1' }
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /I`"$library`" `"$directory/test.c`" `"$library/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
            if ($LASTEXITCODE) { throw "Small sync build failed: $version/$profile" }
            & "$directory/test.exe" > "$directory/results.log" 2> "$directory/errors.log"
            if ($LASTEXITCODE) { throw "Small sync failed: $version/$profile" }
            Write-Output "$version/$profile`: passed."
        }
    }
} finally {
    Pop-Location
}
