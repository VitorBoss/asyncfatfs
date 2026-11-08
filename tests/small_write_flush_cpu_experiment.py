"""Prepare read-only flush-policy CPU optimizations without changing production."""
import json
from pathlib import Path
import shutil

from full_upstream_report import digest, fixed_clock

root = Path('tests/build/small-write-flush-cpu')
root.mkdir(parents=True, exist_ok=True)
source = Path('lib/asyncfatfs.c').read_text(encoding='utf-8')
start = source.index('#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT\n        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {', source.index('static bool afatfs_flushCacheFiltered'))
split = source.index('\n#else\n        for (int i = 0;', start)
end = source.index('\n#endif\n\n        if (earliestSectorIndex', split) + len('\n#endif')
eligible = source[:start] + source[start + len('#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT\n'):split] + source[end:]
needle = '''static bool afatfs_collectFatPage(LBA_t sector)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {'''
replacement = '''static bool afatfs_collectFatPage(LBA_t sector)
{
    // Data and directory pages cannot belong to either FAT copy.
    if (sector < afatfs.fatStartSector || sector - afatfs.fatStartSector >= 2 * afatfs.fatSectors) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {'''
assert source.count(needle) == 1
assert eligible.count(needle) == 1
range_only = source.replace(needle, replacement)
both = eligible.replace(needle, replacement)
for name, candidate in (('before', source), ('eligible', eligible), ('range', range_only), ('both', both)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / f'{name}-production.c').write_text(candidate, encoding='utf-8')
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate), encoding='utf-8')
old = Path('tests/build/small-write-cpu2/final')
shutil.copytree(old / 'upstream', root / 'upstream', dirs_exist_ok=True)
for name in ('harness.c', 'portability.h'):
    shutil.copyfile(old / name, root / name)
for kind in ('cpu', 'io'):
    for source_name, name in (('after', 'before'), ('upstream', 'upstream')):
        shutil.copytree(old / f'{kind}-interleaved-{source_name}', root / f'{kind}-interleaved-{name}', dirs_exist_ok=True)
runner = Path('tests/run_small_write_cpu2_experiment.ps1').read_text()
runner = runner.replace('tests/build/small-write-cpu2', 'tests/build/small-write-flush-cpu')
runner = runner.replace("@('before', 'arithmetic', 'hit', 'both', 'upstream')", "@('before', 'eligible', 'range', 'both', 'upstream')")
Path('tests/run_small_write_flush_cpu_experiment.ps1').write_text(runner)
(root / 'before-source.json').write_text(json.dumps({path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()}, indent=2))
print(root)
