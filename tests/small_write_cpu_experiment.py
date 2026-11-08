"""Prepare isolated small-write CPU candidates without changing production sources."""
from pathlib import Path
import json
import shutil

from full_upstream_report import digest, fixed_clock

root = Path('tests/build/small-write-cpu')
root.mkdir(parents=True, exist_ok=True)
for name in ('before', 'loop', 'fast'):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
source = Path('lib/asyncfatfs.c').read_text()
old = '''        if (afatfs_collectWritable(file) && afatfs_isEndOfAllocatedFile(file) && !afatfs_collectNext(file)) {
            break;
        }
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL && file->writeLockedCacheIndex == -1 &&'''
new = '''        bool collecting = afatfs_collectWritable(file);
        if (file->writeLockedCacheIndex == -1) {
            if (collecting && afatfs_isEndOfAllocatedFile(file) && !afatfs_collectNext(file)) {
                break;
            }
        }
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL && file->writeLockedCacheIndex == -1 &&'''
assert source.count(old) == 1
loop = source.replace(old, new)
needle = '''#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs_collectWritable(file)) {
            file->operation.state.appendFreeCluster.collectIdle = 0;'''
assert loop.count(needle) == 1
loop = loop.replace(needle, needle.replace('afatfs_collectWritable(file)', 'collecting'))
fast = source
needle = '''    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    uint32_t writtenBytes = 0;

    while (len > 0) {'''
replacement = '''    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    uint32_t writtenBytes = 0;

#ifdef AFATFS_PIPELINE_ALLOCATION
    // A collecting writer may finish a request inside its existing locked page.
    // No allocation, page acquisition or cursor-boundary work is needed here.
    if (len && len < AFATFS_SECTOR_SIZE - cursorOffsetInSector &&
        file->writeLockedCacheIndex != -1 && afatfs_collectWritable(file)) {
        uint8_t *sectorBuffer = afatfs_fileLockCursorSectorForWrite(file, false);
        if (!sectorBuffer) {
            file->error = AFATFS_ERROR_BUSY;
            return 0;
        }
        memcpy(sectorBuffer + cursorOffsetInSector, buffer, len);
        file->modified = true;
        file->operation.state.appendFreeCluster.collectIdle = 0;
        file->cursorOffset += len;
        afatfs_fileUpdateFilesize(file);
        return len;
    }
#endif

    while (len > 0) {'''
assert fast.count(needle) == 1
fast = fast.replace(needle, replacement)
for name, candidate in (('before', source), ('loop', loop), ('fast', fast)):
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate))
shutil.copytree('tests/build/full-upstream-20261119/upstream', root / 'upstream', dirs_exist_ok=True)
for name in ('harness.c', 'portability.h'):
    shutil.copyfile(Path('tests/build/full-upstream-20261119') / name, root / name)
runner = Path('tests/run_full_upstream_comparison.ps1').read_text()
runner = runner.replace("'tests/build/full-upstream-20261119'", "'tests/build/small-write-cpu'")
runner = runner.replace("@('default', 'multiread', 'query', 'upstream', 'upstream_min2')", "@('before', 'loop', 'fast', 'upstream')")
runner = runner.replace("$sourceName = if ($variant.StartsWith('upstream')) { $variant } else { 'current' }", '$sourceName = $variant')
Path('tests/run_small_write_cpu.ps1').write_text(runner)
(root / 'production.json').write_text(json.dumps({path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()}, indent=2))
print(root)
