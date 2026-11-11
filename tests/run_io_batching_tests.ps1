param(
    [string]$BaselineDirectory = '',
    [string]$OutputDirectory = 'tests/build/io-batching'
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    foreach ($configuration in @('default', 'no_multiblock')) {
        $directory = Join-Path $OutputDirectory "cache-$configuration"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = if ($configuration -eq 'default') { '' } else {
            '/DAFATFS_EXTERNAL_CONFIG /Itests\config\no_multiblock'
        }
        $binary = Join-Path $directory 'test.exe'
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /DTEST_FAT_ONLY /DTEST_LFN /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests\cache_batching.c lib\fat_standard.c /Fo$directory/ /Fe$binary /link /INCREMENTAL:NO" *> (Join-Path $directory 'build.log')
        if ($LASTEXITCODE) { throw "Cache harness compilation failed: $configuration" }
        & $binary *> (Join-Path $directory 'test.log')
        if ($LASTEXITCODE) { throw "Cache harness failed: $configuration" }
    }
    $variants = if ($BaselineDirectory) { @('before', 'after') } else { @('after') }
    foreach ($configuration in @('default', 'combined')) {
        $flags = if ($configuration -eq 'default') { '/DTEST_FAT_ONLY /DTEST_LFN' } else {
            '/DTEST_LFN /DAFATFS_EXTERNAL_CONFIG /Itests\config\lfn_exfat'
        }
        foreach ($variant in $variants) {
            $source = if ($variant -eq 'before') { Join-Path $BaselineDirectory 'asyncfatfs.c' } else { 'lib/asyncfatfs.c' }
            $directory = Join-Path $OutputDirectory "multi-$configuration-$variant"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $binary = Join-Path $directory 'benchmark.exe'
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /DAFATFS_BATCH_BENCHMARK /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests\cache_batching.c `"$source`" lib\fat_standard.c /Fo$directory/ /Fe$binary /link /INCREMENTAL:NO" *> (Join-Path $directory 'build.log')
            if ($LASTEXITCODE) { throw "Multi-block benchmark compilation failed: $configuration/$variant" }
            & $binary *> (Join-Path $directory 'test.log')
            if ($LASTEXITCODE) { throw "Multi-block benchmark failed: $configuration/$variant" }
            Get-Content (Join-Path $directory 'test.log') | Where-Object { $_ -like 'MULTI,*' }
        }
    }
} finally {
    Pop-Location
}
