"""Compare safe staggered link/EOC publication with the frozen small-write baseline."""
from pathlib import Path
import shutil
from metadata_write_audit import instrument, diagnostic_harness, replace_once

root = Path('tests/build/small-write-pipeline')
source = (root / 'before/lib/asyncfatfs.c').read_text()
old = '''            if (afatfs_groupableAllocation(file) && afatfs_pendingAllocationEoc(file)) {
                // A peer has already requested an allocation. Give its EOC
                // barrier a chance to finish before submitting our ready link.
                if (!afatfs_flushAllocationCache(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                afatfs_groupDurableAllocations();
                if (afatfs_pendingAllocationEoc(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
            }'''
variants = {'before': source, 'staggered': replace_once(source, old, '            // Our destination EOC has already been acknowledged in both FATs.')}
durable = replace_once(source, '#define AFATFS_RESERVED_CLUSTER 0x80000000U',
    '#define AFATFS_RESERVED_CLUSTER 0x80000000U\n#define AFATFS_RESERVATION_DURABLE 0x40000000U')
durable = replace_once(durable,
    'static void afatfs_groupDurableAllocations(void)\n{',
    '''static void afatfs_groupDurableAllocations(void)
{
    if (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) {
        afatfs.lastClusterAllocated |= AFATFS_RESERVATION_DURABLE;
    }''')
durable = replace_once(durable,
    '                    if ((entry->sectorIndex == afatfs_fatSectorToPhysical(0, sector) ||',
    '                    if (!(afatfs.lastClusterAllocated & AFATFS_RESERVATION_DURABLE) &&\n                        (entry->sectorIndex == afatfs_fatSectorToPhysical(0, sector) ||')
variants['durable'] = durable
variants['durable_staggered'] = replace_once(durable, old, '            // Our destination EOC has already been acknowledged in both FATs.')
def closing(code):
    code = replace_once(code,
        'file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER)) {',
        '''file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER ||
                (file->operation.active == AFATFS_FILE_OPERATION_CLOSE && file->operation.state.closeFile.phase < 2) ||
                (file->operation.active == AFATFS_FILE_OPERATION_SYNC && file->operation.state.syncFile.phase < 2))) {''')
    return replace_once(code,
        '''        if (!afatfs_flushCache()) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;''',
        '''#ifdef AFATFS_PIPELINE_ALLOCATION
        if (!afatfs_flushAllocationCache(file)) {
#else
        if (!afatfs_flushCache()) {
#endif
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;''')
variants['closing'] = closing(source)
variants['durable_closing'] = closing(durable)
no_peer_reserve = replace_once(source,
    'static void afatfs_reserveNextCluster(const afatfsFile_t *file)\n{',
    '''static void afatfs_reserveNextCluster(const afatfsFile_t *file)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *peer = &afatfs.openFiles[index];
        if (peer != file && peer->type == AFATFS_FILE_TYPE_NORMAL &&
            peer->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
            return;
        }
    }''')
variants['no_peer_reserve'] = no_peer_reserve
variants['no_peer_reserve_closing'] = closing(no_peer_reserve)
early_peer = replace_once(no_peer_reserve,
    '        if (peer != file && peer->type == AFATFS_FILE_TYPE_NORMAL &&',
    '''        if (!file->operation.state.appendFreeCluster.previousCluster &&
            file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1 &&
            peer != file && peer->type == AFATFS_FILE_TYPE_NORMAL &&''')
variants['early_peer_closing'] = closing(early_peer)
for name, code in variants.items():
    raw = root / name / 'lib'
    if name != 'before':
        shutil.copytree(root / 'before/lib', raw, dirs_exist_ok=True)
        (raw / 'asyncfatfs.c').write_text(code)
    library = root / 'audit' / name / 'lib'
    shutil.copytree(raw, library, dirs_exist_ok=True)
    (library / 'asyncfatfs.c').write_text(instrument(code))
(root / 'audit/harness.c').write_text(diagnostic_harness(Path('tests/upstream_comparison.c').read_text()))
shutil.copyfile('tests/build/write-cache-batch/audit/portability.h', root / 'audit/portability.h')
