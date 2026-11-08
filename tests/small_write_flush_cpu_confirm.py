"""Freeze the flush-selection changes and prepare their final paired validation."""
import json
from pathlib import Path
import shutil

from full_upstream_report import digest, fixed_clock

experiment = Path('tests/build/small-write-flush-cpu')
root = experiment / 'final'
old = Path('tests/build/small-write-cpu2/final')
root.mkdir(parents=True, exist_ok=True)
for source, name in ((old / 'normal/lib', 'before'), (Path('lib'), 'normal')):
    shutil.copytree(source, root / name / 'lib', dirs_exist_ok=True)
for name, source in (('before', root / 'before/lib'), ('after', root / 'normal/lib')):
    target = root / f'{name}-bench/lib'
    shutil.copytree(source, target, dirs_exist_ok=True)
    path = target / 'asyncfatfs.c'
    path.write_text(fixed_clock(path.read_text(encoding='utf-8')), encoding='utf-8')
expected = (experiment / 'both/lib/asyncfatfs.c').read_text()
old_guard = '''    // Data and directory pages cannot belong to either FAT copy.
    if (sector < afatfs.fatStartSector || sector - afatfs.fatStartSector >= 2 * afatfs.fatSectors) {'''
new_guard = '''    // Reject pages outside both FAT copies before scanning collecting writers.
    // Subtractions also preserve the range test when physical LBAs wrap.
    LBA_t fatOffset = sector - afatfs.fatStartSector;
    if (fatOffset >= afatfs.fatSectors && fatOffset - afatfs.fatSectors >= afatfs.fatSectors) {'''
assert expected.count(old_guard) == 1
assert (root / 'after-bench/lib/asyncfatfs.c').read_text() == expected.replace(old_guard, new_guard)
shutil.copytree(old / 'upstream', root / 'upstream', dirs_exist_ok=True)
for filename in ('harness.c', 'portability.h'):
    shutil.copyfile(old / filename, root / filename)
for schedule in ('serial', 'interleaved'):
    for kind in ('cpu', 'io'):
        for source_name, target_name in (('after', 'before'), ('after_multi', 'before_multi'),
                                         ('after_query', 'before_query'), ('upstream', 'upstream')):
            shutil.copytree(old / f'{kind}-{schedule}-{source_name}', root / f'{kind}-{schedule}-{target_name}', dirs_exist_ok=True)
runner = Path('tests/run_small_write_cpu2_confirmation.ps1').read_text()
runner = runner.replace('tests/build/small-write-cpu2/final', 'tests/build/small-write-flush-cpu/final')
Path('tests/run_small_write_flush_cpu_confirmation.ps1').write_text(runner)
workspace = root / 'validation'
shutil.copytree(root / 'normal/lib', workspace / 'lib', dirs_exist_ok=True)
shutil.copytree(root / 'before/lib', workspace / 'before/lib', dirs_exist_ok=True)
(workspace / 'tests').mkdir(parents=True, exist_ok=True)
for path in Path('tests').iterdir():
    if path.is_file():
        shutil.copyfile(path, workspace / 'tests' / path.name)
shutil.copytree('tests/config', workspace / 'tests/config', dirs_exist_ok=True)
path = workspace / 'tests/config_common.h'
path.write_text(path.read_text().replace('#define AFATFS_NUM_CACHE_SECTORS 4', '#define AFATFS_NUM_CACHE_SECTORS 8'))
manifest = dict(upstream=json.loads((old / 'manifest.json').read_text())['upstream'],
    production={path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()},
    sources={name: {path.name: digest(path) for path in (root / name / 'lib').iterdir() if path.is_file()}
             for name in ('before', 'normal', 'before-bench', 'after-bench', 'upstream')},
    harness=digest(root / 'harness.c'))
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2))
print(root)
