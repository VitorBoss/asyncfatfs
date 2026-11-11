param(
    [string]$BaselineDirectory = '',
    [string]$OutputDirectory = 'tests/build/metadata-benchmark'
)
$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) {
        throw 'Visual Studio C/C++ build tools were not found.'
    }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $variants = if ($BaselineDirectory) { @('before', 'after') } else { @('after') }
    foreach ($configuration in @('default', 'combined')) {
        $flags = if ($configuration -eq 'default') {
            '/DTEST_FAT_ONLY /DTEST_LFN'
        } else {
            '/DTEST_LFN /DAFATFS_EXTERNAL_CONFIG /Itests\config\lfn_exfat'
        }
        foreach ($variant in $variants) {
            $source = if ($variant -eq 'before') {
                Join-Path $BaselineDirectory 'asyncfatfs.c'
            } else {
                'lib/asyncfatfs.c'
            }
            $directory = Join-Path $OutputDirectory "$configuration-$variant"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $binary = Join-Path $directory 'benchmark.exe'
            & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests\metadata_batch.c `"$source`" lib\fat_standard.c /Fo`"$directory/`" /Fe`"$binary`" /link /INCREMENTAL:NO" *> (Join-Path $directory 'build.log')
            if ($LASTEXITCODE) {
                throw "Benchmark compilation failed: $configuration/$variant"
            }
            & $binary | Tee-Object -FilePath (Join-Path $OutputDirectory "$configuration-$variant.log")
            if ($LASTEXITCODE) {
                throw "Benchmark integrity verification failed: $configuration/$variant"
            }
        }
    }
} finally {
    Pop-Location
}
