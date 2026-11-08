"""Avoid duplicate append dispatch checks while preserving collection boundaries."""
from pathlib import Path
import shutil

from full_upstream_report import fixed_clock

root = Path('tests/build/small-write-poll-cpu')
byte_source = (root / 'byte_both-production.c').read_text(encoding='utf-8')
start = byte_source.index('    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {', byte_source.index('static void afatfs_fileOperationsPoll()\n{'))
end = byte_source.index('\n#if ASYNCFS_ENABLE_FATFS_API', start)
replacement = '''    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->operation.active == AFATFS_FILE_OPERATION_NONE) {
            continue;
        }
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
            file->type != AFATFS_FILE_TYPE_NONE) {
            if (file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT) {
                // Preserve the original increment and full sealing boundary.
                if (afatfs.cacheFlushInProgress) {
                    continue;
                }
                if (file->operation.state.appendFreeCluster.collectIdle != 63) {
                    file->operation.state.appendFreeCluster.collectIdle++;
                    continue;
                }
            }
            afatfs_appendRegularFreeClusterContinue(file);
        } else {
            afatfs_fileOperationContinue(file);
        }
#else
        afatfs_fileOperationContinue(file);
#endif
    }'''
combined = byte_source[:start] + replacement + byte_source[end:]
narrow = combined.replace('    uint8_t collectIdle;', '    unsigned char collectIdle : 6;')
for name, candidate in (('dispatch', narrow), ('byte_dispatch', combined)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / f'{name}-production.c').write_text(candidate, encoding='utf-8')
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate), encoding='utf-8')
shutil.copyfile(root / 'experiment.json', root / 'byte-experiment.json')
for name in ('before', 'upstream'):
    shutil.copytree(root / f'cpu-interleaved-{name}', root / f'byte-cpu-interleaved-{name}', dirs_exist_ok=True)
runner = Path('tests/run_small_write_idle_byte_experiment.ps1').read_text()
runner = runner.replace("@('before', 'byte', 'byte_phase', 'byte_both', 'upstream')",
    "@('before', 'dispatch', 'byte_dispatch', 'upstream')")
Path('tests/run_small_write_dispatch_experiment.ps1').write_text(runner)
