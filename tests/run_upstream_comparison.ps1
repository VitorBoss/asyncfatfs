param(
    [string]$OutputDirectory = 'tests/build/upstream-fat32-83-20261116',
    [string]$ReportPath = 'tests/UPSTREAM_FAT32_83_COMPARISON.md',
    [string]$Python = 'python',
    [string]$Xc16Directory = 'C:/Program Files/Microchip/xc16/v2.10/bin'
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    $rootAbsolute = (Resolve-Path $OutputDirectory).Path
    @'
import io, json, shutil, subprocess, sys, tarfile
from pathlib import Path
root = Path(sys.argv[1]).resolve()
commit = subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip()
archive = subprocess.check_output(['git', 'archive', commit, 'lib'])
with tarfile.open(fileobj=io.BytesIO(archive)) as source:
    for member in source.getmembers():
        if member.isfile() and member.name.startswith('lib/') and '..' not in Path(member.name).parts:
            target = root / 'upstream' / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.extractfile(member).read())
shutil.copytree('lib', root / 'current/lib', dirs_exist_ok=True)
(root / 'portability.h').write_text('#define __attribute__(attributes)\n#pragma pack(push, 1)\n#include "fat_standard.h"\n#pragma pack(pop)\n')
(root / 'config').mkdir(exist_ok=True)
(root / 'config/afatfs_config.h').write_text('#define AFATFS_NUM_CACHE_SECTORS 4\n#define AFATFS_MAX_OPEN_FILES 3\n#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2\n#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100\n#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"\n#define AFATFS_INTROSPEC_LOG_FILENAME "ASYNCFAT.LOG"\n')
for variant in ('upstream_native', 'upstream_equal', 'current_native'):
    version = 'upstream' if variant.startswith('upstream') else 'current'
    (root / f'{variant}-layout.c').write_text('#include "' + (root / version / 'lib/asyncfatfs.c').as_posix() + '"\nconst uint16_t comparison_layout[] = {sizeof(afatfs_t), sizeof(afatfsFile_t), sizeof(afatfsCacheBlockDescriptor_t)};\n')
(root / 'manifest.json').write_text(json.dumps(dict(upstream_commit=commit, current_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(), worktree_status=subprocess.check_output(['git', 'status', '--short'], text=True)), indent=2))
'@ | & $Python - $OutputDirectory
    if ($LASTEXITCODE) { throw 'Source preparation failed.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) { throw 'Visual Studio C/C++ build tools were not found.' }
    foreach ($variant in @('upstream_native', 'upstream_equal', 'current_native')) {
        $sourceVersion = if ($variant -like 'upstream*') { 'upstream' } else { 'current' }
        $sourceRoot = "$OutputDirectory/$sourceVersion/lib"
        foreach ($bits in @('32', '64')) {
            $directory = "$OutputDirectory/$variant-$bits"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $vcvars = Join-Path $visualStudio "VC/Auxiliary/Build/vcvars$bits.bat"
            $flags = if ($sourceVersion -eq 'upstream') { "/FI`"$rootAbsolute/portability.h`" /wd4244 /wd4267 /wd4018" } else { '/DCOMPARISON_CURRENT' }
            if ($variant -eq 'upstream_equal') { $flags += " /DAFATFS_EXTERNAL_CONFIG /I`"$OutputDirectory/config`"" }
            & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" `"$OutputDirectory/$variant-layout.c`" /Fo`"$directory/layout.obj`"" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw "Layout compilation failed: $variant/$bits" }
            if ($bits -eq '32') {
                & cmd /c "`"$vcvars`" >nul && cl /nologo /c /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`"" *> "$directory/memory-build.log"
                if ($LASTEXITCODE) { throw "Object compilation failed: $variant/$bits" }
            } else {
                & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /D_CRT_SECURE_NO_WARNINGS $flags /I`"$sourceRoot`" tests/upstream_comparison.c `"$sourceRoot/asyncfatfs.c`" `"$sourceRoot/fat_standard.c`" /Fo`"$directory/`" /Fe`"$directory/benchmark.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
                if ($LASTEXITCODE) { throw "Benchmark compilation failed: $variant" }
                & "$directory/benchmark.exe" 7 > "$directory/results.csv" 2> "$directory/audit.log"
                if ($LASTEXITCODE) { throw "Integrity test failed: $variant" }
            }
        }
        foreach ($enumMode in @('normal', 'short')) {
            $directory = "$OutputDirectory/$variant-16-$enumMode"
            New-Item -ItemType Directory -Force $directory | Out-Null
            $targetFlags = @('-mcpu=24FJ256GA702', '-std=c99', '-Os', "-I$sourceRoot")
            if ($variant -eq 'upstream_equal') { $targetFlags += @('-DAFATFS_EXTERNAL_CONFIG', "-I$OutputDirectory/config") }
            if ($enumMode -eq 'short') { $targetFlags += '-fshort-enums' }
            & "$Xc16Directory/xc16-gcc.exe" @targetFlags -S "$OutputDirectory/$variant-layout.c" -o "$directory/layout.s" *> "$directory/layout-build.log"
            if ($LASTEXITCODE) { throw "XC16 layout compilation failed: $directory" }
            foreach ($source in @('asyncfatfs', 'fat_standard')) {
                & "$Xc16Directory/xc16-gcc.exe" @targetFlags -c "$sourceRoot/$source.c" -o "$directory/$source.o" *> "$directory/$source-build.log"
                if ($LASTEXITCODE) { throw "XC16 compilation failed: $directory/$source" }
                & "$Xc16Directory/xc16-objdump.exe" -h -t "$directory/$source.o" > "$directory/$source-sections.txt"
                if ($LASTEXITCODE) { throw 'Object inspection failed.' }
            }
        }
        Write-Output "$variant`: benchmark integrity and memory probes completed."
    }
    & $Python tests/upstream_comparison_report.py --directory $OutputDirectory --output $ReportPath
    if ($LASTEXITCODE) { throw 'Report validation failed.' }
} finally {
    Pop-Location
}
