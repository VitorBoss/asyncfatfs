"""Diagnostic only: request-sized batches amplify unrecoverable EOCs after a cut; do not promote."""
from metadata_write_audit import replace_once


PREPARE = '''
/** @brief Initialize only clusters covered by the current write request, within one FAT page. */
static afatfsOperationStatus_e afatfs_prepareRequestedBatch(afatfsFile_t *file)
{
    afatfsAppendFreeCluster_t *state = &file->operation.state.appendFreeCluster;
    bool ownsReservation = (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) != 0;
    if (ownsReservation) {
        if (!afatfs_flushAllocationCache(file)) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        afatfs_groupDurableAllocations();
        state->searchCluster = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
    } else {
        afatfsFindClusterStatus_e found = afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE,
            &state->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);
        if (found == AFATFS_FIND_CLUSTER_IN_PROGRESS) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (found != AFATFS_FIND_CLUSTER_FOUND) {
            state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
            return AFATFS_OPERATION_FAILURE;
        }
    }
    afatfsFATSector_t sector;
    uint8_t *mirror;
    afatfsOperationStatus_e status = afatfs_FATWriteSector(
        state->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR, &sector, &mirror,
        AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, AFATFS_FAT_MIRROR_STATE(NULL) 0);
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    uint32_t count = 0;
    uint32_t offset = state->searchCluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
    uint32_t limit = MIN(state->progress.zeroSector, AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR - offset);
    limit = MIN(limit, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER - state->searchCluster);
    limit = MIN(limit, (UINT32_MAX - file->physicalSize) / AFATFS_SECTOR_SIZE);
    while (count < limit) {
        uint32_t next = sector.fat32[offset + count];
        if (!(ownsReservation && count == 0) && !fat_isFreeSpace(next & 0x0FFFFFFF)) {
            break;
        }
        sector.fat32[offset + count] = (next & 0xF0000000) | 0x0FFFFFFF;
        count++;
    }
    if (!count) {
        state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
        return AFATFS_OPERATION_FAILURE;
    }
    memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
    state->progress.zeroSector = state->searchCluster + count;
    afatfs.lastClusterAllocated = state->progress.zeroSector - 1;
    file->cursorCluster = state->searchCluster;
    file->physicalSize += count * AFATFS_SECTOR_SIZE;
    if (!state->previousCluster) {
        file->firstCluster = state->searchCluster;
    }
    state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
    return AFATFS_OPERATION_SUCCESS;
}
'''


def requested_batch(source, maximum):
    source = replace_once(source, '''    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE,
} afatfsAppendFreeClusterPhase_e;''', '''    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE,
#ifdef AFATFS_PIPELINE_ALLOCATION
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS,
#endif
} afatfsAppendFreeClusterPhase_e;''')
    marker = '#endif\n\n/**\n * @brief Advance single-cluster allocation while ordering data, both FAT copies and publication.'
    source = replace_once(source, marker, PREPARE + marker)
    source = replace_once(source, '''        } else if (state->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE) {
            state->phase = state->previousCluster ? AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2''', '''        } else if (state->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE) {
            if (state->wrapped) {
                state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS;
                continue;
            }
            state->phase = state->previousCluster ? AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2''')
    marker = '''    switch (opState->phase) {
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:'''
    source = replace_once(source, marker, '''    switch (opState->phase) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND:
            status = afatfs_prepareRequestedBatch(file);
            if (status == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_pendingAllocationPeer(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                goto doMore;
            }
            if (status == AFATFS_OPERATION_FAILURE) {
                goto doMore;
            }
            return AFATFS_OPERATION_IN_PROGRESS;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS:
            // Every destination EOC has been acknowledged in both FATs.
            while (opState->progress.zeroSector > opState->searchCluster + 1) {
                status = afatfs_FATSetNextCluster(opState->progress.zeroSector - 2, opState->progress.zeroSector - 1);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                opState->progress.zeroSector--;
            }
            if (opState->previousCluster) {
                status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
            if (afatfs_pendingAllocationPeer(file)) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            goto doMore;
#endif
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:''')
    marker = '        sectorBuffer = afatfs_fileLockCursorSectorForWrite(file, writingFullSector);'
    source = replace_once(source, marker, '''#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL && file->writeLockedCacheIndex == -1 &&
            len >= 2 * AFATFS_SECTOR_SIZE && afatfs_isEndOfAllocatedFile(file)) {
            file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
            afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster,
                file->cursorPreviousCluster);
            file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND;
            file->operation.state.appendFreeCluster.progress.zeroSector = MIN(len / AFATFS_SECTOR_SIZE, ''' + str(maximum) + ''');
            // Native FAT32 otherwise does not use the exFAT wrap marker.
            file->operation.state.appendFreeCluster.wrapped = true;
            break;
        }
#endif
''' + marker)
    return source
