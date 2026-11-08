/* Persistent exFAT metadata undo log. Included here to share the original async
 * cache/operation machines, without allocation or a second storage abstraction.
 * On-disk integers are little endian; every header/record has an independent CRC.
 */
static uint32_t afatfs_jget(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void afatfs_jput(uint8_t *p, uint32_t v)
{
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(v >> (8 * n));
}
static uint32_t afatfs_jcrc(const uint8_t *p, unsigned length)
{
    return ~afatfs_crc32Update(UINT32_MAX, p, length);
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
static void afatfs_jcomplete(sdcardBlockOperation_e op, uint32_t block, uint8_t *buffer, uint32_t user)
{
    (void)op; (void)block;
    if (user != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) return;
    afatfs.journal.ioPending = false;
    afatfs.journal.ioDone = true;
    afatfs.journal.ioFailed = buffer == NULL;
}
// 0 = yield/retry, 1 = durable completion. Failed I/O never advances the phase:
// write buffers, prepared headers and before-images remain available for retry.
static int afatfs_jio(bool write, uint32_t sector)
{
    afatfsJournal_t *j = &afatfs.journal;
    if (j->ioDone) {
        j->ioDone = false;
        if (j->ioFailed) {
            j->ioFailed = false;
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            return 0;
        }
        return 1;
    }
    if (j->ioPending) return 0;
    if (!write) {
        if (sdcard_readBlock(sector, j->io, afatfs_jcomplete, afatfs_mediaGeneration)) j->ioPending = true;
    } else {
        switch (sdcard_writeBlock(sector, j->io, afatfs_jcomplete, afatfs_mediaGeneration)) {
        case SDCARD_OPERATION_SUCCESS: return 1;
        case SDCARD_OPERATION_IN_PROGRESS: j->ioPending = true; break;
        case SDCARD_OPERATION_FAILURE: afatfs.lastError = AFATFS_ERROR_BACKEND_IO; break;
        default: break;
        }
    }
    return 0;
}
static bool afatfs_jvalidHeader(const uint8_t *h)
{
    afatfsJournal_t *j = &afatfs.journal;
    return !memcmp(h, AFATFS_JOURNAL_HEADER_MAGIC, 8)
        && afatfs_jget(h + 508) == afatfs_jcrc(h, 508)
        && afatfs_jget(h + 28) == afatfs.volumeSerial
        && afatfs_jget(h + 32) == j->firstCluster && afatfs_jget(h + 36) == j->sectors
        && afatfs_jget(h + 40) == j->capacity
        && afatfs_jget(h + 44) == afatfs.partitionStartSector
        && afatfs_jget(h + 48) == afatfs.fatStartSector
        && afatfs_jget(h + 52) == afatfs.clusterStartSector
        && afatfs_jget(h + 56) == afatfs.sectorsPerCluster
        && afatfs_jget(h + 60) == afatfs.numClusters
        && afatfs_jget(h + 16) <= j->capacity
        && (afatfs_jget(h + 20) || !afatfs_jget(h + 16))
        && afatfs_jget(h + 20) <= 1 && afatfs_jget(h + 24) <= 1;
}
static void afatfs_jheader(void)
{
    afatfsJournal_t *j = &afatfs.journal;
    memset(j->io, 0, 512);
    memcpy(j->io, AFATFS_JOURNAL_HEADER_MAGIC, 8);
    afatfs_jput(j->io + 8, ++j->sequence);
    afatfs_jput(j->io + 12, j->epoch);
    afatfs_jput(j->io + 16, j->count);
    afatfs_jput(j->io + 20, j->active);
    afatfs_jput(j->io + 24, j->owned);
    afatfs_jput(j->io + 28, afatfs.volumeSerial);
    afatfs_jput(j->io + 32, j->firstCluster);
    afatfs_jput(j->io + 36, j->sectors);
    afatfs_jput(j->io + 40, j->capacity);
    afatfs_jput(j->io + 44, afatfs.partitionStartSector);
    afatfs_jput(j->io + 48, afatfs.fatStartSector);
    afatfs_jput(j->io + 52, afatfs.clusterStartSector);
    afatfs_jput(j->io + 56, afatfs.sectorsPerCluster);
    afatfs_jput(j->io + 60, afatfs.numClusters);
    afatfs_jput(j->io + 508, afatfs_jcrc(j->io, 508));
    j->headerSlot ^= 1;
}
static int afatfs_jwriteHeader(void)
{
    afatfsJournal_t *j = &afatfs.journal;
    if (!j->headerPrepared) { afatfs_jheader(); j->headerPrepared = true; }
    int result = afatfs_jio(true, afatfs_jsector(j->headerSlot));
    if (result) j->headerPrepared = false;
    return result;
}
static bool afatfs_jrecord(void)
{
    afatfsJournal_t *j = &afatfs.journal;
    uint32_t target = afatfs_jget(j->io + 12);
    return !memcmp(j->io, AFATFS_JOURNAL_RECORD_MAGIC, 8)
        && afatfs_jget(j->io + 8) == j->epoch
        && afatfs_jget(j->io + 508) == afatfs_jcrc(j->io, 508)
        && target >= afatfs.fatStartSector
        && (uint64_t)target < (uint64_t)afatfs.partitionStartSector + afatfs.partitionSectors
        && !(target >= afatfs_jsector(0) && target - afatfs_jsector(0) < j->sectors);
}
static bool afatfs_jfileQuiescent(const afatfsFile_t *f)
{
    switch (f->operation.active) {
    case AFATFS_FILE_OPERATION_CREATE_FILE:
        return afatfs.journal.blockAllocation &&
            (f->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_INITIAL
            || (f->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE
                && afatfs.currentDirectory.operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY));
    case AFATFS_FILE_OPERATION_NONE: return true;
    case AFATFS_FILE_OPERATION_SYNC: return f->operation.state.syncFile.phase >= 3;
    case AFATFS_FILE_OPERATION_CLOSE: return f->operation.state.closeFile.phase >= 3;
    case AFATFS_FILE_OPERATION_UNLINK:
    case AFATFS_FILE_OPERATION_TRUNCATE: return f->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_SUCCESS
        || (afatfs.journal.blockAllocation && f->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_INITIAL);
    case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
    case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
        return afatfs.journal.blockAllocation
            && f->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
    default: return f->type == AFATFS_FILE_TYPE_NONE;
    }
}
static bool afatfs_jquiescent(void)
{
    if (afatfs_request && !afatfs_request->checkpoint) return false;
    if (afatfs.cacheFlushInProgress || (afatfs.cwdWalkMutex && !afatfs.journal.blockAllocation) || afatfs.allocationOwner) return false;
    for (int n = 0; n < AFATFS_NUM_CACHE_SECTORS; n++) {
        afatfsCacheBlockDescriptor_t *d = &afatfs.cacheDescriptor[n];
        if (d->state == AFATFS_CACHE_STATE_WRITING || (d->state == AFATFS_CACHE_STATE_DIRTY && !d->locked)) return false;
    }
    if (!afatfs_jfileQuiescent(&afatfs.currentDirectory)) return false;
    for (int n = 0; n < AFATFS_MAX_OPEN_FILES; n++) if (!afatfs_jfileQuiescent(&afatfs.openFiles[n])) return false;
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (!afatfs_jfileQuiescent(&afatfs.introSpecLog)) return false;
#endif
    return true;
}
static bool afatfs_journalCommitted(void)
{
    return !afatfs.journal.ready || (!afatfs.journal.active && afatfs.journal.phase == AFATFS_JOURNAL_IDLE);
}
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index)
{
    afatfsJournal_t *j = &afatfs.journal;
    afatfsCacheBlockDescriptor_t *d = &afatfs.cacheDescriptor[index];
    if (!j->ready) return AFATFS_OPERATION_SUCCESS;
    if (j->phase == AFATFS_JOURNAL_ERROR) return AFATFS_OPERATION_FAILURE;
    if (j->phase == AFATFS_JOURNAL_COMMIT) return AFATFS_OPERATION_IN_PROGRESS;
    if (j->active && d->journalEpoch == j->epoch) return AFATFS_OPERATION_SUCCESS;
    if (j->phase != AFATFS_JOURNAL_IDLE || d->state == AFATFS_CACHE_STATE_WRITING) return AFATFS_OPERATION_IN_PROGRESS;
    if (!j->active) {
        j->active = true; j->count = 0;
        if (++j->epoch == 0) { afatfs_jfail(); return AFATFS_OPERATION_FAILURE; }
        j->phase = AFATFS_JOURNAL_START;
    } else j->phase = AFATFS_JOURNAL_INDEX_READ;
    j->pendingCache = index; j->pendingSector = d->sectorIndex; j->scan = 0;
    d->retainCount++;
    memcpy(j->before, afatfs_cacheSectorGetMemory(index), 512);
    return AFATFS_OPERATION_IN_PROGRESS;
}
static void afatfs_journalPoll(void)
{
    afatfsJournal_t *j = &afatfs.journal;
    if (!j->ready || j->ioPending) return;
    switch (j->phase) {
    case AFATFS_JOURNAL_IDLE:
        if (j->active && afatfs_jquiescent()) { j->active = false; j->count = 0; j->phase = AFATFS_JOURNAL_COMMIT; }
        break;
    case AFATFS_JOURNAL_START:
        if (afatfs_jwriteHeader() == 1) j->phase = AFATFS_JOURNAL_INDEX_READ;
        break;
    case AFATFS_JOURNAL_INDEX_READ:
        // Linear on-media lookup bounds RAM independently of volume size. Cache epoch
        // tags avoid this scan while a sector remains resident.
        if (j->scan == j->count) {
            if (j->count == j->capacity) { afatfs_jfail(); break; }
            memcpy(j->io, j->before, 512); j->phase = AFATFS_JOURNAL_IMAGE_WRITE;
        } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * j->scan)) == 1) {
            if (!afatfs_jrecord()) { afatfs_jfail(); break; }
            if (afatfs_jget(j->io + 12) == j->pendingSector) j->phase = AFATFS_JOURNAL_CAPTURE_DONE;
            else j->scan++;
        }
        break;
    case AFATFS_JOURNAL_IMAGE_WRITE:
        if (afatfs_jio(true, afatfs_jsector(3 + 2 * j->count)) == 1) {
            memset(j->io, 0, 512); memcpy(j->io, AFATFS_JOURNAL_RECORD_MAGIC, 8);
            afatfs_jput(j->io + 8, j->epoch); afatfs_jput(j->io + 12, j->pendingSector);
            afatfs_jput(j->io + 16, afatfs_jcrc(j->before, 512));
            afatfs_jput(j->io + 508, afatfs_jcrc(j->io, 508));
            j->phase = AFATFS_JOURNAL_RECORD_WRITE;
        }
        break;
    case AFATFS_JOURNAL_RECORD_WRITE:
        if (afatfs_jio(true, afatfs_jsector(2 + 2 * j->count)) == 1) { j->count++; j->phase = AFATFS_JOURNAL_RECORD_HEADER; }
        break;
    case AFATFS_JOURNAL_RECORD_HEADER:
        if (afatfs_jwriteHeader() == 1) j->phase = AFATFS_JOURNAL_CAPTURE_DONE;
        break;
    case AFATFS_JOURNAL_CAPTURE_DONE:
        afatfs.cacheDescriptor[j->pendingCache].journalEpoch = j->epoch;
        afatfs.cacheDescriptor[j->pendingCache].retainCount--;
        j->pendingCache = -1; j->phase = AFATFS_JOURNAL_IDLE;
        break;
    case AFATFS_JOURNAL_COMMIT:
        if (afatfs_jwriteHeader() == 1) { j->blockAllocation = false; j->phase = AFATFS_JOURNAL_IDLE; }
        break;
    default: break;
    }
}

// Mount/recovery/provisioning uses the same finite state machine and private handle.
// No application operation is accepted until this routine has completed.
static afatfsOperationStatus_e afatfs_journalMount(void)
{
    // GPT write protection forbids provisioning and recovery writes, including dirty-flag changes.
    if (afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY) return AFATFS_OPERATION_SUCCESS;
    afatfsJournal_t *j = &afatfs.journal;
    afatfsFile_t *f = &afatfs.journalFile;
    afatfsOperationStatus_e status;
    uint8_t *sector;
    if (j->phase == AFATFS_JOURNAL_ERROR) return AFATFS_OPERATION_SUCCESS; // protected read-only mount
    switch (j->initPhase) {
    case 0: {
        uint64_t capacity = (uint64_t)afatfs.fatSectors + ((uint64_t)afatfs.bitmapLength + 511) / 512 + 64;
        uint64_t bytes = (2 + 2 * capacity) * 512;
        bytes = (bytes + afatfs_clusterSize() - 1) / afatfs_clusterSize() * afatfs_clusterSize();
        if (bytes > UINT32_MAX) { afatfs_jfail(); break; }
        j->capacity = (uint32_t)capacity; j->sectors = (uint32_t)(bytes / 512);
        j->clusters = j->sectors / afatfs.sectorsPerCluster;
        if (j->found) {
            f->exfatEntryCount = 3;
            if (afatfs_exfatLoadEntrySet(f, AFATFS_JOURNAL_NAME) != 1 || !f->exfatNoFatChain
                || f->logicalSize != bytes || f->validDataLength != bytes
                || (f->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) { afatfs_jfail(); break; }
            j->firstCluster = f->firstCluster; j->initPhase = 1;
        } else if (j->startupDirty) { afatfs_jfail(); break; }
        else {
            j->provision = true; j->candidate = 2; j->runLength = 0;
            j->initPhase = 10;
        }
        break;
    }
    case 1:
        if (afatfs_jio(false, afatfs_jsector(0)) == 1) {
            memcpy(j->header, j->io, 512); j->initPhase = 2;
        }
        break;
    case 2:
        if (afatfs_jio(false, afatfs_jsector(1)) == 1) {
            bool a = afatfs_jvalidHeader(j->header), b = afatfs_jvalidHeader(j->io);
            if (!a && !b) { afatfs_jfail(); break; }
            if (b && (!a || (int32_t)(afatfs_jget(j->io + 8) - afatfs_jget(j->header + 8)) > 0)) {
                memcpy(j->header, j->io, 512); j->headerSlot = 1;
            } else j->headerSlot = 0;
            j->sequence = afatfs_jget(j->header + 8); j->epoch = afatfs_jget(j->header + 12);
            j->count = afatfs_jget(j->header + 16); j->active = afatfs_jget(j->header + 20) != 0;
            j->owned = afatfs_jget(j->header + 24) != 0;
            // An owned empty log is also the durable checkpoint of an interrupted
            // clean-marker write. Never infer ownership from an older header when
            // the newest valid header says unowned: an external writer may have
            // dirtied that volume. Recovery must precede any external writer.
            if ((j->startupDirty && !j->owned) || (!j->startupDirty && j->active)) { afatfs_jfail(); break; }
            j->progress = j->active ? j->count : 0; j->initPhase = 21;
        }
        break;
    case 3:
        if (!j->progress) { j->active = false; j->count = 0; j->initPhase = 6; break; }
        if (afatfs_jio(false, afatfs_jsector(2 + 2 * (j->progress - 1))) == 1) {
            if (!afatfs_jrecord()) { afatfs_jfail(); break; }
            j->target = afatfs_jget(j->io + 12); j->imageCRC = afatfs_jget(j->io + 16); j->initPhase = 4;
        }
        break;
    case 4:
        if (afatfs_jio(false, afatfs_jsector(3 + 2 * (j->progress - 1))) == 1) {
            if (afatfs_jcrc(j->io, 512) != j->imageCRC) { afatfs_jfail(); break; }
            j->initPhase = 5;
        }
        break;
    case 5:
        if (afatfs_jio(true, j->target) == 1) { j->progress--; j->initPhase = 3; }
        break;
    case 6:
        if (!j->startupDirty || afatfs_jwriteHeader() == 1) {
            // Cached root/FAT pages may describe the interrupted state.
            memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
            afatfs.cacheDirtyEntries = 0; afatfs.cacheFlushInProgress = false;
            afatfs.bitmapCursorCluster = afatfs.bitmapCursorIndex = 0;
            afatfs.readOnly = false; afatfs.lastError = AFATFS_ERROR_NONE;
            afatfs_chdir(NULL); j->initPhase = 7;
        }
        break;
    case 7: // Mark ownership only after the dirty boot marker is durable.
        status = afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        sector[106] |= 2; sector[112] = 0xFF; j->initPhase = 8;
        break;
    case 8:
        if (!afatfs_flushCache()) break;
        j->owned = true; j->initPhase = 9;
        break;
    case 9:
        if (afatfs_jwriteHeader() == 1) {
            j->ready = true; j->phase = AFATFS_JOURNAL_IDLE; j->pendingCache = -1;
            return AFATFS_OPERATION_SUCCESS;
        }
        break;
    case 10: // First mount of a clean volume: establish the dirty guard before bootstrap.
        status = afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        sector[106] |= 2; sector[112] = 0xFF; j->initPhase = 11;
        break;
    case 11:
        if (!afatfs_flushCache()) break;
        afatfs_createFile(f, AFATFS_JOURNAL_NAME,
            FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY,
            AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
        j->initPhase = 12;
        break;
    case 12:
        afatfs_fileOperationContinue(&afatfs.currentDirectory);
        afatfs_fileOperationContinue(f);
        if (f->operation.active != AFATFS_FILE_OPERATION_NONE) break;
        if (f->type != AFATFS_FILE_TYPE_NORMAL || f->firstCluster) { afatfs_jfail(); break; }
        j->initPhase = 13;
        break;
    case 13: {
        // Locate a contiguous extent. The journal locator never needs the FAT at recovery.
        bool allocated;
        if (j->candidate - 2 >= afatfs.numClusters) { afatfs_jfail(); break; }
        status = afatfs_exfatBitmap(j->candidate, false, false, &allocated);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        j->runLength = allocated ? 0 : j->runLength + 1;
        j->candidate++;
        if (j->runLength == j->clusters) {
            j->firstCluster = j->candidate - j->clusters; j->progress = 0; j->initPhase = 14;
        }
        break;
    }
    case 14:
        if (j->progress == j->sectors) { j->progress = 0; j->initPhase = 15; break; }
        if (!j->ioPending && !j->ioDone) memset(j->io, 0, 512);
        if (afatfs_jio(true, afatfs_jsector(j->progress)) == 1) j->progress++;
        break;
    case 15:
        if (j->progress == j->clusters) { j->initPhase = 16; break; }
        status = afatfs_exfatBitmap(j->firstCluster + j->progress, true, true, NULL);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        j->progress++;
        break;
    case 16:
        if (!afatfs_flushCache()) break;
        f->firstCluster = j->firstCluster; f->exfatNoFatChain = true;
        f->physicalSize = f->logicalSize = f->validDataLength = f->committedDataLength = j->sectors * 512;
        j->initPhase = 17;
        break;
    case 17:
        status = afatfs_saveDirectoryEntry(f, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        j->initPhase = 18;
        break;
    case 18:
        if (!afatfs_flushCache()) break;
        j->epoch = 1; j->owned = true; j->active = false; j->count = 0; j->initPhase = 19;
        break;
    case 19:
        if (afatfs_jwriteHeader() == 1) j->initPhase = 20;
        break;
    case 20:
        if (afatfs_jwriteHeader() == 1) {
            j->ready = true; j->provision = false; j->phase = AFATFS_JOURNAL_IDLE; j->pendingCache = -1;
            afatfs_chdir(NULL);
            return AFATFS_OPERATION_SUCCESS;
        }
        break;
    case 21: // Validate the ENTIRE log before the first recovery write.
        if (!j->progress) { j->progress = j->active ? j->count : 0; j->initPhase = 3; break; }
        if (afatfs_jio(false, afatfs_jsector(2 + 2 * (j->progress - 1))) == 1) {
            if (!afatfs_jrecord()) { afatfs_jfail(); break; }
            j->imageCRC = afatfs_jget(j->io + 16); j->initPhase = 22;
        }
        break;
    case 22:
        if (afatfs_jio(false, afatfs_jsector(3 + 2 * (j->progress - 1))) == 1) {
            if (afatfs_jcrc(j->io, 512) != j->imageCRC) { afatfs_jfail(); break; }
            j->progress--; j->initPhase = 21;
        }
        break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
