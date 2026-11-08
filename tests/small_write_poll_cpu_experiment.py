"""Prepare bounded FAT-pair lookup and collecting-writer poll experiments."""
import json
from pathlib import Path
import shutil

from full_upstream_report import digest, fixed_clock

root = Path('tests/build/small-write-poll-cpu')
root.mkdir(parents=True, exist_ok=True)
source = Path('lib/asyncfatfs.c').read_text(encoding='utf-8')
old = '''        if (entry->sectorIndex == primarySector) {
            primaryIndex = index;
        } else if (entry->sectorIndex == primarySector + afatfs.fatSectors) {
            mirrorIndex = index;
        }'''
new = '''        if (entry->sectorIndex == primarySector) {
            primaryIndex = index;
            if (mirrorIndex >= 0) {
                break;
            }
        } else if (entry->sectorIndex == primarySector + afatfs.fatSectors) {
            mirrorIndex = index;
            if (primaryIndex >= 0) {
                break;
            }
        }'''
assert source.count(old) == 1
scan = source.replace(old, new)
start = source.index('static void afatfs_fileOperationsPoll()\n{')
end = source.index('\n/**', start)
old_poll = source[start:end]
needle = '''    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
#if defined(AFATFS_PIPELINE_ALLOCATION)'''
replacement = '''    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs_collectWritable(file) && file->type != AFATFS_FILE_TYPE_NONE) {
            // Before sealing, collection only advances its bounded idle count.
            // Keep the same increment per pass and the original boundary path.
            if (afatfs.cacheFlushInProgress) {
                continue;
            }
            if (file->operation.state.appendFreeCluster.collectIdle != 63) {
                file->operation.state.appendFreeCluster.collectIdle++;
                continue;
            }
        }
#endif
#if defined(AFATFS_PIPELINE_ALLOCATION)'''
assert old_poll.count(needle) == 1
poll = old_poll.replace('        afatfsFile_t *file = &afatfs.openFiles[index];\n', '')
poll = poll.replace(needle, replacement)
poll = poll.replace('afatfs.openFiles[index].operation.active', 'file->operation.active')
poll = poll.replace('afatfs_fileOperationContinue(&afatfs.openFiles[index]);', 'afatfs_fileOperationContinue(file);')
idle = source.replace(old_poll, poll)
both = scan.replace(old_poll, poll)
for name, candidate in (('before', source), ('scan', scan), ('idle', idle), ('both', both)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / f'{name}-production.c').write_text(candidate, encoding='utf-8')
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate), encoding='utf-8')
old_root = Path('tests/build/small-write-flush-cpu/final')
shutil.copytree(old_root / 'upstream', root / 'upstream', dirs_exist_ok=True)
for filename in ('harness.c', 'portability.h'):
    shutil.copyfile(old_root / filename, root / filename)
for kind in ('cpu', 'io'):
    for source_name, name in (('after', 'before'), ('upstream', 'upstream')):
        shutil.copytree(old_root / f'{kind}-interleaved-{source_name}', root / f'{kind}-interleaved-{name}', dirs_exist_ok=True)
runner = Path('tests/run_small_write_flush_cpu_experiment.ps1').read_text()
runner = runner.replace('tests/build/small-write-flush-cpu', 'tests/build/small-write-poll-cpu')
runner = runner.replace("@('before', 'eligible', 'range', 'both', 'upstream')", "@('before', 'scan', 'idle', 'both', 'upstream')")
Path('tests/run_small_write_poll_cpu_experiment.ps1').write_text(runner)
(root / 'before-source.json').write_text(json.dumps({path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()}, indent=2))
print(root)
