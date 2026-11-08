"""Use existing operation-union space for a byte-sized collection idle counter."""
from pathlib import Path
import shutil

from full_upstream_report import fixed_clock

root = Path('tests/build/small-write-poll-cpu')
source = Path('lib/asyncfatfs.c').read_text(encoding='utf-8')
old = '    unsigned char collectIdle : 6;'
new = '    uint8_t collectIdle;'
assert source.count(old) == 1
byte = source.replace(old, new)
idle = (root / 'idle-production.c').read_text(encoding='utf-8')
both = (root / 'both-production.c').read_text(encoding='utf-8')
needle = '        if (afatfs_collectWritable(file) && file->type != AFATFS_FILE_TYPE_NONE) {'
replacement = '''        if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
            file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT &&
            file->type != AFATFS_FILE_TYPE_NONE) {'''
assert idle.count(needle) == both.count(needle) == 1
phase = idle.replace(needle, replacement).replace(old, new)
combined = both.replace(needle, replacement).replace(old, new)
for name, candidate in (('byte', byte), ('byte_phase', phase), ('byte_both', combined)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / f'{name}-production.c').write_text(candidate, encoding='utf-8')
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate), encoding='utf-8')
shutil.copyfile(root / 'experiment.json', root / 'first-experiment.json')
for name in ('before', 'upstream'):
    shutil.copytree(root / f'cpu-interleaved-{name}', root / f'first-cpu-interleaved-{name}', dirs_exist_ok=True)
runner = Path('tests/run_small_write_poll_cpu_experiment.ps1').read_text()
runner = runner.replace("@('before', 'scan', 'idle', 'both', 'upstream')",
    "@('before', 'byte', 'byte_phase', 'byte_both', 'upstream')")
Path('tests/run_small_write_idle_byte_experiment.ps1').write_text(runner)
