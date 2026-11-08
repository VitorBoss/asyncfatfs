"""Investigate bounded preparation of already pending FAT allocations, in frozen copies."""
import argparse
import json
from pathlib import Path
import shutil

from metadata_write_audit import replace_once
from metadata_write_report import load
from mirror_profile_fix_report import digest
from poll_metadata_audit import trace_poll
from fat_requested_batch import requested_batch


VARIANTS = ('base', 'defer_link', 'defer_eoc', 'defer_both', 'defer_preparation', 'prepare_links_first',
            'requested_2', 'requested_4', 'requested_8', 'fresh_dirty_reserve', 'fresh_defer_link', 'fresh_three',
            'fresh_pair_dirty', 'fresh_three_pair')
HELPER = '''
/** @brief Yield a barrier only to another allocation whose cache preparation is already pending. */
static bool afatfs_pendingBatchPreparation(const afatfsFile_t *file, bool linkBarrier)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *peer = &afatfs.openFiles[index];
        if (peer == file || !afatfs_groupableAllocation(peer)) {
            continue;
        }
        if (linkBarrier) {
            if (peer->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2) {
                return true;
            }
        } else {
            switch (peer->operation.state.appendFreeCluster.phase) {
                case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
                case AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN:
                case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}
'''


def variant_source(source, variant):
    if variant.startswith('requested_'):
        return requested_batch(source, int(variant.split('_')[1]))
    if variant in ('fresh_dirty_reserve', 'fresh_defer_link', 'fresh_three', 'fresh_pair_dirty', 'fresh_three_pair'):
        source = replace_once(source, '''                        // Keep the old directory image buffered while both reservation FAT pages drain.
                        afatfs_flushAllocationCache(file);''', '''                        if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationPeer(file)) {
                            // Keep the unique look-ahead reservation; allocate this already requested peer normally.
                            goto findFreshBatchCluster;
                        }
                        // Keep the old directory image buffered while both reservation FAT pages drain.
                        afatfs_flushAllocationCache(file);''')
        source = replace_once(source, '''            switch (afatfs_findClusterWithCondition(
                CLUSTER_SEARCH_FREE, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {''', '''#ifdef AFATFS_PIPELINE_ALLOCATION
findFreshBatchCluster:
#endif
            switch (afatfs_findClusterWithCondition(
                CLUSTER_SEARCH_FREE, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {''')
        if variant in ('fresh_three', 'fresh_three_pair'):
            marker = '#endif\n\n/**\n * @brief Advance single-cluster allocation while ordering data, both FAT copies and publication.'
            helper = '''
/** @brief Detect three already pending single-cluster requests without creating another reservation. */
static bool afatfs_pendingAllocationTriple(const afatfsFile_t *file)
{
    bool foundPeer = false;
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *peer = &afatfs.openFiles[index];
        if (peer != file && afatfs_groupableAllocation(peer)) {
            if (foundPeer) {
                return true;
            }
            foundPeer = true;
        }
    }
    return false;
}
'''
            source = replace_once(source, marker, helper + marker)
            source = replace_once(source,
                '                        if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationPeer(file)) {',
                '                        if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationTriple(file)) {')
        if variant in ('fresh_pair_dirty', 'fresh_three_pair'):
            marker = '#endif\n\n/**\n * @brief Advance single-cluster allocation while ordering data, both FAT copies and publication.'
            helper = '''
/** @brief Share only an entirely dirty FAT pair, without redirtying an already acknowledged copy. */
static bool afatfs_reservationPairDirty(uint32_t sector)
{
    unsigned found = 0;
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state != AFATFS_CACHE_STATE_DIRTY) {
            continue;
        }
        if (entry->sectorIndex == afatfs_fatSectorToPhysical(0, sector)) {
            found |= 1;
        } else if (entry->sectorIndex == afatfs_fatSectorToPhysical(1, sector)) {
            found |= 2;
        }
    }
    return found == 3;
}
'''
            source = replace_once(source, marker, helper + marker)
            source = source.replace('if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationPeer(file)) {',
                'if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationPeer(file) && afatfs_reservationPairDirty(sector)) {')
            source = source.replace('if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationTriple(file)) {',
                'if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationTriple(file) && afatfs_reservationPairDirty(sector)) {')
        if variant == 'fresh_defer_link':
            return variant_source(source, 'defer_link')
        return source
    if variant in ('defer_link', 'defer_eoc', 'defer_both', 'defer_preparation'):
        marker = '#endif\n\n/**\n * @brief Advance single-cluster allocation while ordering data, both FAT copies and publication.'
        source = replace_once(source, marker, HELPER + marker)
        if variant in ('defer_link', 'defer_both', 'defer_preparation'):
            condition = 'afatfs_pendingBatchPreparation(file, true)'
            if variant == 'defer_preparation':
                condition += ' || afatfs_pendingBatchPreparation(file, false)'
            source = replace_once(source, '''        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (!afatfs_flushAllocationCache(file)) {''', '''        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (afatfs_groupableAllocation(file) && (''' + condition + ''')) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (!afatfs_flushAllocationCache(file)) {''')
        if variant in ('defer_eoc', 'defer_both', 'defer_preparation'):
            condition = 'afatfs_pendingBatchPreparation(file, false)'
            if variant == 'defer_preparation':
                condition += ' || afatfs_pendingBatchPreparation(file, true)'
            source = replace_once(source, '''        case AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (!afatfs_flushAllocationCache(file)) {''', '''        case AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (afatfs_groupableAllocation(file) && (''' + condition + ''')) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (!afatfs_flushAllocationCache(file)) {''')
    elif variant == 'prepare_links_first':
        marker = '''    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
#if defined(AFATFS_PIPELINE_ALLOCATION) && AFATFS_NUM_CACHE_SECTORS >= 8 &&'''
        source = replace_once(source, marker, '''#ifdef AFATFS_PIPELINE_ALLOCATION
    // Diagnostic: prepare already ready links before any waiter starts draining the cache.
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *peer = &afatfs.openFiles[index];
        if (afatfs_groupableAllocation(peer) &&
            peer->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2) {
            afatfs_appendRegularFreeClusterContinue(peer);
        }
    }
#endif
''' + marker)
    return source


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    frozen = root / 'raw/base/lib'
    if not frozen.exists():
        assert 'afatfs_reservationPairDirty' not in Path('lib/asyncfatfs.c').read_text(), 'Use the frozen pre-grouping baseline.'
        shutil.copytree('lib', frozen)
    source = (frozen / 'asyncfatfs.c').read_text()
    for variant in VARIANTS:
        raw = root / f'raw/{variant}/lib'
        traced = root / variant / 'lib'
        if variant != 'base':
            shutil.copytree(frozen, raw, dirs_exist_ok=True)
        (raw / 'asyncfatfs.c').write_text(variant_source(source, variant))
        shutil.copytree(raw, traced, dirs_exist_ok=True)
        (traced / 'asyncfatfs.c').write_text(trace_poll(variant_source(source, variant)))
    shutil.copyfile('tests/build/poll-metadata/harness.c', root / 'harness.c')
    shutil.copyfile('tests/build/poll-metadata/portability.h', root / 'portability.h')
    (root / 'manifest.json').write_text(json.dumps(dict(production_sha256=digest(frozen / 'asyncfatfs.c'),
        variants=VARIANTS, raw={variant: digest(root / f'raw/{variant}/lib/asyncfatfs.c') for variant in VARIANTS},
        traced={variant: digest(root / variant / 'lib/asyncfatfs.c') for variant in VARIANTS}), indent=2))
    work = root / 'matrix-work/tests'
    shutil.copytree('tests/config', work / 'config', dirs_exist_ok=True)
    (work / 'config_common.h').write_text(Path('tests/config_common.h').read_text().replace('AFATFS_NUM_CACHE_SECTORS 4', 'AFATFS_NUM_CACHE_SECTORS 8'))


def prepare_three(root):
    target = root / 'three'
    variants = ('base', 'defer_link', 'defer_both', 'defer_preparation', 'prepare_links_first',
                'fresh_dirty_reserve', 'fresh_defer_link', 'fresh_three', 'fresh_pair_dirty', 'fresh_three_pair')
    for variant in variants:
        shutil.copytree(root / variant / 'lib', target / variant / 'lib', dirs_exist_ok=True)
    (target / 'harness.c').write_text('#define COMPARISON_THREE_FILES\n' + (root / 'harness.c').read_text())
    shutil.copyfile(root / 'portability.h', target / 'portability.h')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/fat-batch'))
    parser.add_argument('--three', action='store_true')
    args = parser.parse_args()
    if args.three:
        prepare_three(args.directory)
    else:
        prepare(args.directory)
