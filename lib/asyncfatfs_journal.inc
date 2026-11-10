/* Shared FAT32/exFAT metadata undo engine. Filesystem adapters only handle
 * discovery, extent allocation and dirty markers. Disk formats remain distinct.
 * The retained, unmodified cache entry supplies the before-image: one aligned
 * sector buffer handles I/O, records and alternating headers, without a heap. */
static const char *afatfs_jmagic(bool record)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        return record ? "AFUNDO02" : "AFJNL002";
    }
#endif
    return record ? "AFFUNDO1" : "AFJFT001";
}
static uint32_t afatfs_jget(const uint8_t *bytes)
{
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
static void afatfs_jput(uint8_t *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}
static uint32_t afatfs_jcrc(const uint8_t *bytes, unsigned length)
{
    return ~afatfs_crc32Update(UINT32_MAX, bytes, length);
}
static void afatfs_jfail(void)
{
    afatfs.journal.phase = AFATFS_JOURNAL_ERROR;
    afatfs.readOnly = true;
    afatfs.lastError = AFATFS_ERROR_RECOVERY_REQUIRED;
}
static uint32_t afatfs_jsector(uint32_t offset)
{
    return afatfs_fileClusterToPhysical(afatfs.journal.firstCluster, 0) + offset;
}
static void afatfs_jcomplete(sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    (void)operation;
    (void)sector;
    if (generation != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) {
        return;
    }
    afatfs.journal.ioPending = false;
    afatfs.journal.ioDone = true;
    afatfs.journal.ioFailed = buffer == NULL;
}
static bool afatfs_jio(bool writing, uint32_t sector)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (state->ioDone) {
        state->ioDone = false;
        if (state->ioFailed) {
            state->ioFailed = false;
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            return false;
        }
        return true;
    }
    if (state->ioPending) {
        return false;
    }
    if (!writing) {
        state->ioPending = sdcard_readBlock(sector, state->io, afatfs_jcomplete, afatfs_mediaGeneration);
    } else {
        sdcardOperationStatus_e status = sdcard_writeBlock(sector, state->io, afatfs_jcomplete, afatfs_mediaGeneration);
        if (status == SDCARD_OPERATION_SUCCESS) {
            return true;
        }
        state->ioPending = status == SDCARD_OPERATION_IN_PROGRESS;
        if (status == SDCARD_OPERATION_FAILURE) {
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
        }
    }
    return false;
}
static bool afatfs_jvalidHeader(void)
{
    const afatfsJournal_t *state = &afatfs.journal;
    const uint8_t *bytes = state->io;
    return !memcmp(bytes, afatfs_jmagic(false), 8) && afatfs_jget(bytes + 508) == afatfs_jcrc(bytes, 508) &&
        afatfs_jget(bytes + 28) == state->serial && afatfs_jget(bytes + 32) == state->firstCluster &&
        afatfs_jget(bytes + 36) == state->sectors && afatfs_jget(bytes + 40) == state->capacity &&
        afatfs_jget(bytes + 44) == afatfs.partitionStartSector && afatfs_jget(bytes + 48) == afatfs.fatStartSector &&
        afatfs_jget(bytes + 52) == afatfs.clusterStartSector && afatfs_jget(bytes + 56) == afatfs.sectorsPerCluster &&
        afatfs_jget(bytes + 60) == afatfs.numClusters && afatfs_jget(bytes + 16) <= state->capacity &&
        afatfs_jget(bytes + 20) <= 1 && afatfs_jget(bytes + 24) <= 1 &&
        (afatfs_jget(bytes + 20) || !afatfs_jget(bytes + 16));
}
static bool afatfs_jwriteHeader(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (!state->headerPrepared) {
        memset(state->io, 0, 512);
        memcpy(state->io, afatfs_jmagic(false), 8);
        afatfs_jput(state->io + 8, ++state->sequence);
        afatfs_jput(state->io + 12, state->epoch);
        afatfs_jput(state->io + 16, state->count);
        afatfs_jput(state->io + 20, state->active);
        afatfs_jput(state->io + 24, state->owned);
        afatfs_jput(state->io + 28, state->serial);
        afatfs_jput(state->io + 32, state->firstCluster);
        afatfs_jput(state->io + 36, state->sectors);
        afatfs_jput(state->io + 40, state->capacity);
        afatfs_jput(state->io + 44, afatfs.partitionStartSector);
        afatfs_jput(state->io + 48, afatfs.fatStartSector);
        afatfs_jput(state->io + 52, afatfs.clusterStartSector);
        afatfs_jput(state->io + 56, afatfs.sectorsPerCluster);
        afatfs_jput(state->io + 60, afatfs.numClusters);
        afatfs_jput(state->io + 508, afatfs_jcrc(state->io, 508));
        state->headerPrepared = true;
    }
    if (!afatfs_jio(true, afatfs_jsector(state->headerSlot ^ 1))) {
        return false;
    }
    state->headerSlot ^= 1;
    state->headerPrepared = false;
    return true;
}
static bool afatfs_jrecord(void)
{
    const afatfsJournal_t *state = &afatfs.journal;
    uint32_t target = afatfs_jget(state->io + 12);
    return !memcmp(state->io, afatfs_jmagic(true), 8) && afatfs_jget(state->io + 8) == state->epoch &&
        afatfs_jget(state->io + 508) == afatfs_jcrc(state->io, 508) && target >= afatfs.fatStartSector &&
        (uint64_t)target < (uint64_t)afatfs.partitionStartSector + afatfs.partitionSectors &&
        !(target >= afatfs_jsector(0) && target - afatfs_jsector(0) < state->sectors);
}
static bool afatfs_jfileQuiescent(const afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE || file->operation.active == AFATFS_FILE_OPERATION_NONE) {
        return true;
    }
    switch (file->operation.active) {
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_LOCKED:
            // A freefile reservation has no metadata work of its own. The owning
            // file remains responsible for completing its allocation and close.
            return true;
#endif
        case AFATFS_FILE_OPERATION_CLOSE:
            return file->operation.state.closeFile.phase >= 3;
        case AFATFS_FILE_OPERATION_SYNC:
            return file->operation.state.syncFile.phase >= 3;
        case AFATFS_FILE_OPERATION_UNLINK:
        case AFATFS_FILE_OPERATION_TRUNCATE:
            return file->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_SUCCESS ||
                (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.journal.blockAllocation &&
                    file->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_INITIAL);
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            return afatfs.journal.blockAllocation &&
                file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
        case AFATFS_FILE_OPERATION_CREATE_FILE:
#ifdef AFATFS_USE_FREEFILE
            if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) &&
                file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_SUCCESS &&
                afatfs.freeFile.operation.active == AFATFS_FILE_OPERATION_LOCKED) {
                return true; // Waiting for the owner's close to release the reservation.
            }
#endif
            return afatfs.journal.blockAllocation &&
                (file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_INITIAL ||
                    (file->operation.state.createFile.phase ==
                            (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                                    ? AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE
                                    : AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB) &&
                        afatfs.currentDirectory.operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY));
        default:
            return false;
    }
}
static bool afatfs_jquiescent(void)
{
    if ((afatfs_request && !afatfs_request->checkpoint) || afatfs.cacheFlushInProgress ||
        (afatfs.cwdWalkMutex && !afatfs.journal.blockAllocation)) {
        return false;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.allocationOwner) {
        return false;
    }
#endif
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state == AFATFS_CACHE_STATE_WRITING ||
            (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked)) {
            return false;
        }
    }
    if (!afatfs_jfileQuiescent(&afatfs.currentDirectory)) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (!afatfs_jfileQuiescent(&afatfs.openFiles[index])) {
            return false;
        }
    }
#ifdef AFATFS_USE_FREEFILE
    if (!afatfs_jfileQuiescent(&afatfs.freeFile)) {
        return false;
    }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (!afatfs_jfileQuiescent(&afatfs.introSpecLog)) {
        return false;
    }
#endif
    return true;
}
static bool afatfs_journalCommitted(void)
{
    return !afatfs.journal.ready || (!afatfs.journal.active && afatfs.journal.phase == AFATFS_JOURNAL_IDLE);
}
static bool afatfs_journalUnresolvedError(void)
{
    return afatfs.journal.ready && afatfs.journal.phase == AFATFS_JOURNAL_ERROR;
}
// Select alternating headers using scalar state, avoiding a second sector buffer.
static void afatfs_jselectHeader(uint32_t slot)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (afatfs_jvalidHeader() && (!state->validHeader || (int32_t)(afatfs_jget(state->io + 8) - state->sequence) > 0)) {
        state->sequence = afatfs_jget(state->io + 8);
        state->epoch = afatfs_jget(state->io + 12);
        state->count = afatfs_jget(state->io + 16);
        state->active = afatfs_jget(state->io + 20) != 0;
        state->owned = afatfs_jget(state->io + 24) != 0;
        state->headerSlot = slot;
        state->validHeader = true;
    }
}
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index)
{
    afatfsJournal_t *state = &afatfs.journal;
    afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
    if (!state->ready) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (state->phase == AFATFS_JOURNAL_ERROR) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (state->active && entry->journalEpoch == state->epoch) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (state->phase != AFATFS_JOURNAL_IDLE || entry->state == AFATFS_CACHE_STATE_WRITING) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (!state->active) {
        if (++state->epoch == 0) {
            afatfs_jfail();
            return AFATFS_OPERATION_FAILURE;
        }
        state->active = true;
        state->count = 0;
        state->phase = AFATFS_JOURNAL_START;
    } else {
        state->phase = AFATFS_JOURNAL_INDEX_READ;
    }
    state->pendingCache = index;
    state->pendingSector = entry->sectorIndex;
    state->scan = 0;
    entry->retainCount++;
    return AFATFS_OPERATION_IN_PROGRESS;
}
static void afatfs_journalPoll(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (!state->ready || state->ioPending) {
        return;
    }
    switch (state->phase) {
        case AFATFS_JOURNAL_IDLE:
            if (state->active && afatfs_jquiescent()) {
                state->active = false;
                state->count = 0;
                state->phase = AFATFS_JOURNAL_COMMIT;
            }
            break;
        case AFATFS_JOURNAL_START:
            if (afatfs_jwriteHeader()) {
                state->phase = AFATFS_JOURNAL_INDEX_READ;
            }
            break;
        case AFATFS_JOURNAL_INDEX_READ:
            if (state->scan == state->count) {
                if (state->count == state->capacity) {
                    afatfs_jfail();
                    break;
                }
                memcpy(state->io, afatfs_cacheSectorGetMemory(state->pendingCache), 512);
                state->imageCRC = afatfs_jcrc(state->io, 512);
                state->phase = AFATFS_JOURNAL_IMAGE_WRITE;
            } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * state->scan))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                if (afatfs_jget(state->io + 12) == state->pendingSector) {
                    state->phase = AFATFS_JOURNAL_CAPTURE_DONE;
                } else {
                    state->scan++;
                }
            }
            break;
        case AFATFS_JOURNAL_IMAGE_WRITE:
            if (afatfs_jio(true, afatfs_jsector(3 + 2 * state->count))) {
                memset(state->io, 0, 512);
                memcpy(state->io, afatfs_jmagic(true), 8);
                afatfs_jput(state->io + 8, state->epoch);
                afatfs_jput(state->io + 12, state->pendingSector);
                afatfs_jput(state->io + 16, state->imageCRC);
                afatfs_jput(state->io + 508, afatfs_jcrc(state->io, 508));
                state->phase = AFATFS_JOURNAL_RECORD_WRITE;
            }
            break;
        case AFATFS_JOURNAL_RECORD_WRITE:
            if (afatfs_jio(true, afatfs_jsector(2 + 2 * state->count))) {
                state->count++;
                state->phase = AFATFS_JOURNAL_RECORD_HEADER;
            }
            break;
        case AFATFS_JOURNAL_RECORD_HEADER:
            if (afatfs_jwriteHeader()) {
                state->phase = AFATFS_JOURNAL_CAPTURE_DONE;
            }
            break;
        case AFATFS_JOURNAL_CAPTURE_DONE:
            afatfs.cacheDescriptor[state->pendingCache].journalEpoch = state->epoch;
            afatfs.cacheDescriptor[state->pendingCache].retainCount--;
            state->pendingCache = -1;
            state->phase = AFATFS_JOURNAL_IDLE;
            break;
        case AFATFS_JOURNAL_COMMIT:
            if (afatfs_jwriteHeader()) {
                state->blockAllocation = false;
                state->phase = AFATFS_JOURNAL_IDLE;
            }
            break;
        default:
            break;
    }
}

// Validate every published image before the first undo write. Keep the active
// header unchanged throughout replay so another power loss can repeat it safely.
static afatfsOperationStatus_e afatfs_journalRecover(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    switch (state->mount.recoveryPhase) {
        case 0:
            state->progress = state->active ? state->count : 0;
            state->mount.recoveryPhase = 1;
            break;
        case 1:
            if (!state->progress) {
                state->progress = state->active ? state->count : 0;
                state->mount.recoveryPhase = 3;
            } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * (state->progress - 1)))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                state->imageCRC = afatfs_jget(state->io + 16);
                state->mount.recoveryPhase = 2;
            }
            break;
        case 2:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (state->progress - 1)))) {
                if (afatfs_jcrc(state->io, 512) != state->imageCRC) {
                    afatfs_jfail();
                } else {
                    state->progress--;
                    state->mount.recoveryPhase = 1;
                }
            }
            break;
        case 3:
            if (!state->progress) {
                state->active = false;
                state->count = 0;
                return AFATFS_OPERATION_SUCCESS;
            }
            if (afatfs_jio(false, afatfs_jsector(2 + 2 * (state->progress - 1)))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                state->target = afatfs_jget(state->io + 12);
                state->imageCRC = afatfs_jget(state->io + 16);
                state->mount.recoveryPhase = 4;
            }
            break;
        case 4:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (state->progress - 1)))) {
                if (afatfs_jcrc(state->io, 512) != state->imageCRC) {
                    afatfs_jfail();
                } else {
                    state->mount.recoveryPhase = 5;
                }
            }
            break;
        case 5:
            if (afatfs_jio(true, state->target)) {
                state->progress--;
                state->mount.recoveryPhase = 3;
            }
            break;
        default:
            afatfs_jfail();
            break;
    }
    return state->phase == AFATFS_JOURNAL_ERROR ? AFATFS_OPERATION_FAILURE : AFATFS_OPERATION_IN_PROGRESS;
}
