param([string]$OutputDirectory = 'tests/build/fat32-allocation-checks')

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$visualStudio) { throw 'Visual Studio C/C++ build tools were not found.' }
    $vcvars = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $cases = [ordered]@{
        order_idle = '/DCOMPARISON_CHECK_IDLE'
        three_files = '/DCOMPARISON_CHECK_IDLE /DCOMPARISON_THREE_FILES'
        pending_order = '/DCOMPARISON_CHECK_IDLE /DCOMPARISON_INTERLEAVED_REQUESTS'
        three_pending = '/DCOMPARISON_CHECK_IDLE /DCOMPARISON_THREE_FILES /DCOMPARISON_INTERLEAVED_REQUESTS'
        near_full = '/DCOMPARISON_NEAR_FULL'
        small_near_full = '/DCOMPARISON_NEAR_FULL /DCOMPARISON_SMALL_WINDOW_NEAR_FULL'
    }
    foreach ($caseName in $cases.Keys) {
        $directory = Join-Path $OutputDirectory $caseName
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = $cases[$caseName]
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DCOMPARISON_CURRENT /DCOMPARISON_CHECK_ALLOCATION_ORDER /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/upstream_comparison.c lib/asyncfatfs.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Compilation failed: $caseName" }
        & "$directory/test.exe" 1 > "$directory/results.csv" 2> "$directory/audit.log"
        if ($LASTEXITCODE) { throw "Allocation check failed: $caseName" }
        Write-Output "$caseName`: passed."
    }
    $directory = Join-Path $OutputDirectory 'metadata_hints'
    New-Item -ItemType Directory -Force $directory | Out-Null
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS /Ilib tests/fat32_metadata_hint.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
    if ($LASTEXITCODE) { throw 'Metadata hint compilation failed.' }
    & "$directory/test.exe" > "$directory/results.log"
    if ($LASTEXITCODE) { throw 'Metadata hint check failed.' }
    Write-Output 'metadata_hints: passed.'
    $mirrorProfiles = [ordered]@{
        mirror_cache = ''
        mirror_cache_multi = '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2'
        mirror_cache_query = '/DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2 /DAFATFS_READ_SESSION_QUERY=1'
    }
    foreach ($profile in $mirrorProfiles.Keys) {
        $directory = Join-Path $OutputDirectory $profile
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = $mirrorProfiles[$profile]
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_cache_mirror.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Mirror cache compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Mirror cache check failed: $profile" }
        Write-Output "$profile`: passed."
    }
    $directory = Join-Path $OutputDirectory 'reservation_commit'
    New-Item -ItemType Directory -Force $directory | Out-Null
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS /Ilib tests/fat32_reservation_commit.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
    if ($LASTEXITCODE) { throw 'Reservation commit compilation failed.' }
    & "$directory/test.exe" > "$directory/results.log"
    if ($LASTEXITCODE) { throw 'Reservation commit check failed.' }
    Write-Output 'reservation_commit: passed.'
    $directory = Join-Path $OutputDirectory 'pending_allocations'
    New-Item -ItemType Directory -Force $directory | Out-Null
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS /Ilib tests/fat32_pending_allocations.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
    if ($LASTEXITCODE) { throw 'Pending allocation compilation failed.' }
    & "$directory/test.exe" > "$directory/results.log"
    if ($LASTEXITCODE) { throw 'Pending allocation check failed.' }
    Write-Output 'pending_allocations: passed.'
    foreach ($profile in $mirrorProfiles.Keys) {
        $directory = Join-Path $OutputDirectory "reserve_wait_$profile"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = $mirrorProfiles[$profile]
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_reserve_wait.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Reserve wait compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Reserve wait check failed: $profile" }
        Write-Output "reserve_wait_$profile`: passed."
    }
    foreach ($profile in $mirrorProfiles.Keys) {
        $directory = Join-Path $OutputDirectory "fresh_pair_$profile"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = $mirrorProfiles[$profile]
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_fresh_pair.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Fresh pair compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Fresh pair check failed: $profile" }
        Write-Output "fresh_pair_$profile`: passed."
    }
    foreach ($profile in $mirrorProfiles.Keys) {
        $directory = Join-Path $OutputDirectory "requested_recovery_$profile"
        New-Item -ItemType Directory -Force $directory | Out-Null
        $flags = $mirrorProfiles[$profile]
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_requested_recovery.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Requested recovery compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Requested recovery check failed: $profile" }
        Write-Output "requested_recovery_$profile`: passed."
        $directory = Join-Path $OutputDirectory "small_sync_$profile"
        New-Item -ItemType Directory -Force $directory | Out-Null
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_small_sync.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Small sync compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Small sync check failed: $profile" }
        Write-Output "small_sync_$profile`: passed."
        $directory = Join-Path $OutputDirectory "small_collection_$profile"
        New-Item -ItemType Directory -Force $directory | Out-Null
        & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /DTEST_SMALL_COLLECTION /D_CRT_SECURE_NO_WARNINGS $flags /Ilib tests/fat32_requested_recovery.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
        if ($LASTEXITCODE) { throw "Small collection compilation failed: $profile" }
        & "$directory/test.exe" > "$directory/results.log"
        if ($LASTEXITCODE) { throw "Small collection check failed: $profile" }
        Write-Output "small_collection_$profile`: passed."
    }
    $directory = Join-Path $OutputDirectory 'reader_cache'
    New-Item -ItemType Directory -Force $directory | Out-Null
    & cmd /c "`"$vcvars`" >nul && cl /nologo /std:c11 /O2 /W3 /WX /DTEST_FAT_ONLY /D_CRT_SECURE_NO_WARNINGS /Ilib tests/fat32_reader_cache.c lib/fat_standard.c /Fo`"$directory/`" /Fe`"$directory/test.exe`" /link /INCREMENTAL:NO" *> "$directory/build.log"
    if ($LASTEXITCODE) { throw 'Reader cache compilation failed.' }
    & "$directory/test.exe" > "$directory/results.log"
    if ($LASTEXITCODE) { throw 'Reader cache test failed.' }
    Write-Output 'reader_cache: passed.'
} finally {
    Pop-Location
}
