/* Persistent exFAT metadata undo log. Included here to share the original async
 * cache/operation machines, without allocation or a second storage abstraction.
 * On-disk integers are little endian; every header/record has an independent CRC.
 */
static uint32_t afatfs_jget(const uint8_t *buffer)
{
    return buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}
static void afatfs_jput(uint8_t *buffer, uint32_t value)
{
    for (unsigned n = 0; n < 4; n++) {
        buffer[n] = (uint8_t)(value >> (8 * n));
    }
}
static uint32_t afatfs_jcrc(const uint8_t *buffer, unsigned length)
{
    return ~afatfs_crc32Update(UINT32_MAX, buffer, length);
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
static void afatfs_jcomplete(sdcardBlockOperation_e operation, uint32_t block, uint8_t *buffer, uint32_t user)
{
    (void)operation;
    (void)block;
    if (user != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) {
        return;
    }
    afatfs.journal.ioPending = false;
    afatfs.journal.ioDone = true;
    afatfs.journal.ioFailed = buffer == NULL;
}
// 0 = yield/retry, 1 = durable completion. Failed I/O never advances the phase:
// write buffers, prepared headers and before-images remain available for retry.
static int afatfs_jio(bool write, uint32_t sector)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    if (journalState->ioDone) {
        journalState->ioDone = false;
        if (journalState->ioFailed) {
            journalState->ioFailed = false;
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            return 0;
        }
        return 1;
    }
    if (journalState->ioPending) {
        return 0;
    }
    if (!write) {
        if (sdcard_readBlock(sector, journalState->io, afatfs_jcomplete, afatfs_mediaGeneration)) {
            journalState->ioPending = true;
        }
    } else {
        switch (sdcard_writeBlock(sector, journalState->io, afatfs_jcomplete, afatfs_mediaGeneration)) {
            case SDCARD_OPERATION_SUCCESS:
                return 1;
            case SDCARD_OPERATION_IN_PROGRESS:
                journalState->ioPending = true;
                break;
            case SDCARD_OPERATION_FAILURE:
                afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
                break;
            default:
                break;
        }
    }
    return 0;
}
static bool afatfs_jvalidHeader(const uint8_t *header)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    return !memcmp(header, AFATFS_JOURNAL_HEADER_MAGIC, 8) && afatfs_jget(header + 508) == afatfs_jcrc(header, 508) &&
        afatfs_jget(header + 28) == afatfs.volumeSerial && afatfs_jget(header + 32) == journalState->firstCluster &&
        afatfs_jget(header + 36) == journalState->sectors && afatfs_jget(header + 40) == journalState->capacity &&
        afatfs_jget(header + 44) == afatfs.partitionStartSector && afatfs_jget(header + 48) == afatfs.fatStartSector &&
        afatfs_jget(header + 52) == afatfs.clusterStartSector && afatfs_jget(header + 56) == afatfs.sectorsPerCluster &&
        afatfs_jget(header + 60) == afatfs.numClusters && afatfs_jget(header + 16) <= journalState->capacity &&
        (afatfs_jget(header + 20) || !afatfs_jget(header + 16)) && afatfs_jget(header + 20) <= 1 &&
        afatfs_jget(header + 24) <= 1;
}
static void afatfs_jheader(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    memset(journalState->io, 0, 512);
    memcpy(journalState->io, AFATFS_JOURNAL_HEADER_MAGIC, 8);
    afatfs_jput(journalState->io + 8, ++journalState->sequence);
    afatfs_jput(journalState->io + 12, journalState->epoch);
    afatfs_jput(journalState->io + 16, journalState->count);
    afatfs_jput(journalState->io + 20, journalState->active);
    afatfs_jput(journalState->io + 24, journalState->owned);
    afatfs_jput(journalState->io + 28, afatfs.volumeSerial);
    afatfs_jput(journalState->io + 32, journalState->firstCluster);
    afatfs_jput(journalState->io + 36, journalState->sectors);
    afatfs_jput(journalState->io + 40, journalState->capacity);
    afatfs_jput(journalState->io + 44, afatfs.partitionStartSector);
    afatfs_jput(journalState->io + 48, afatfs.fatStartSector);
    afatfs_jput(journalState->io + 52, afatfs.clusterStartSector);
    afatfs_jput(journalState->io + 56, afatfs.sectorsPerCluster);
    afatfs_jput(journalState->io + 60, afatfs.numClusters);
    afatfs_jput(journalState->io + 508, afatfs_jcrc(journalState->io, 508));
    journalState->headerSlot ^= 1;
}
static int afatfs_jwriteHeader(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    if (!journalState->headerPrepared) {
        afatfs_jheader();
        journalState->headerPrepared = true;
    }
    int result = afatfs_jio(true, afatfs_jsector(journalState->headerSlot));
    if (result) {
        journalState->headerPrepared = false;
    }
    return result;
}
static bool afatfs_jrecord(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    uint32_t target = afatfs_jget(journalState->io + 12);
    return !memcmp(journalState->io, AFATFS_JOURNAL_RECORD_MAGIC, 8) &&
        afatfs_jget(journalState->io + 8) == journalState->epoch &&
        afatfs_jget(journalState->io + 508) == afatfs_jcrc(journalState->io, 508) && target >= afatfs.fatStartSector &&
        (uint64_t)target < (uint64_t)afatfs.partitionStartSector + afatfs.partitionSectors &&
        !(target >= afatfs_jsector(0) && target - afatfs_jsector(0) < journalState->sectors);
}
static bool afatfs_jfileQuiescent(const afatfsFile_t *fileHandle)
{
    switch (fileHandle->operation.active) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            return afatfs.journal.blockAllocation &&
                (fileHandle->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_INITIAL ||
                    (fileHandle->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE &&
                        afatfs.currentDirectory.operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY));
        case AFATFS_FILE_OPERATION_NONE:
            return true;
        case AFATFS_FILE_OPERATION_SYNC:
            return fileHandle->operation.state.syncFile.phase >= 3;
        case AFATFS_FILE_OPERATION_CLOSE:
            return fileHandle->operation.state.closeFile.phase >= 3;
        case AFATFS_FILE_OPERATION_UNLINK:
        case AFATFS_FILE_OPERATION_TRUNCATE:
            return fileHandle->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_SUCCESS ||
                (afatfs.journal.blockAllocation &&
                    fileHandle->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_INITIAL);
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            return afatfs.journal.blockAllocation &&
                fileHandle->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
        default:
            return fileHandle->type == AFATFS_FILE_TYPE_NONE;
    }
}
static bool afatfs_jquiescent(void)
{
    if (afatfs_request && !afatfs_request->checkpoint) {
        return false;
    }
    if (afatfs.cacheFlushInProgress || (afatfs.cwdWalkMutex && !afatfs.journal.blockAllocation) ||
        afatfs.allocationOwner) {
        return false;
    }
    for (int n = 0; n < AFATFS_NUM_CACHE_SECTORS; n++) {
        afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[n];
        if (cacheDescriptor->state == AFATFS_CACHE_STATE_WRITING ||
            (cacheDescriptor->state == AFATFS_CACHE_STATE_DIRTY && !cacheDescriptor->locked)) {
            return false;
        }
    }
    if (!afatfs_jfileQuiescent(&afatfs.currentDirectory)) {
        return false;
    }
    for (int n = 0; n < AFATFS_MAX_OPEN_FILES; n++) {
        if (!afatfs_jfileQuiescent(&afatfs.openFiles[n])) {
            return false;
        }
    }
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
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[index];
    if (!journalState->ready) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (journalState->phase == AFATFS_JOURNAL_COMMIT) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (journalState->active && cacheDescriptor->journalEpoch == journalState->epoch) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (journalState->phase != AFATFS_JOURNAL_IDLE || cacheDescriptor->state == AFATFS_CACHE_STATE_WRITING) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (!journalState->active) {
        journalState->active = true;
        journalState->count = 0;
        if (++journalState->epoch == 0) {
            afatfs_jfail();
            return AFATFS_OPERATION_FAILURE;
        }
        journalState->phase = AFATFS_JOURNAL_START;
    } else {
        journalState->phase = AFATFS_JOURNAL_INDEX_READ;
    }
    journalState->pendingCache = index;
    journalState->pendingSector = cacheDescriptor->sectorIndex;
    journalState->scan = 0;
    cacheDescriptor->retainCount++;
    memcpy(journalState->before, afatfs_cacheSectorGetMemory(index), 512);
    return AFATFS_OPERATION_IN_PROGRESS;
}
static void afatfs_journalPoll(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    if (!journalState->ready || journalState->ioPending) {
        return;
    }
    switch (journalState->phase) {
        case AFATFS_JOURNAL_IDLE:
            if (journalState->active && afatfs_jquiescent()) {
                journalState->active = false;
                journalState->count = 0;
                journalState->phase = AFATFS_JOURNAL_COMMIT;
            }
            break;
        case AFATFS_JOURNAL_START:
            if (afatfs_jwriteHeader() == 1) {
                journalState->phase = AFATFS_JOURNAL_INDEX_READ;
            }
            break;
        case AFATFS_JOURNAL_INDEX_READ:
            // Linear on-media lookup bounds RAM independently of volume size. Cache epoch
            // tags avoid this scan while a sector remains resident.
            if (journalState->scan == journalState->count) {
                if (journalState->count == journalState->capacity) {
                    afatfs_jfail();
                    break;
                }
                memcpy(journalState->io, journalState->before, 512);
                journalState->phase = AFATFS_JOURNAL_IMAGE_WRITE;
            } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * journalState->scan)) == 1) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                if (afatfs_jget(journalState->io + 12) == journalState->pendingSector) {
                    journalState->phase = AFATFS_JOURNAL_CAPTURE_DONE;
                } else {
                    journalState->scan++;
                }
            }
            break;
        case AFATFS_JOURNAL_IMAGE_WRITE:
            if (afatfs_jio(true, afatfs_jsector(3 + 2 * journalState->count)) == 1) {
                memset(journalState->io, 0, 512);
                memcpy(journalState->io, AFATFS_JOURNAL_RECORD_MAGIC, 8);
                afatfs_jput(journalState->io + 8, journalState->epoch);
                afatfs_jput(journalState->io + 12, journalState->pendingSector);
                afatfs_jput(journalState->io + 16, afatfs_jcrc(journalState->before, 512));
                afatfs_jput(journalState->io + 508, afatfs_jcrc(journalState->io, 508));
                journalState->phase = AFATFS_JOURNAL_RECORD_WRITE;
            }
            break;
        case AFATFS_JOURNAL_RECORD_WRITE:
            if (afatfs_jio(true, afatfs_jsector(2 + 2 * journalState->count)) == 1) {
                journalState->count++;
                journalState->phase = AFATFS_JOURNAL_RECORD_HEADER;
            }
            break;
        case AFATFS_JOURNAL_RECORD_HEADER:
            if (afatfs_jwriteHeader() == 1) {
                journalState->phase = AFATFS_JOURNAL_CAPTURE_DONE;
            }
            break;
        case AFATFS_JOURNAL_CAPTURE_DONE:
            afatfs.cacheDescriptor[journalState->pendingCache].journalEpoch = journalState->epoch;
            afatfs.cacheDescriptor[journalState->pendingCache].retainCount--;
            journalState->pendingCache = -1;
            journalState->phase = AFATFS_JOURNAL_IDLE;
            break;
        case AFATFS_JOURNAL_COMMIT:
            if (afatfs_jwriteHeader() == 1) {
                journalState->blockAllocation = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
            }
            break;
        default:
            break;
    }
}

// Mount/recovery/provisioning uses the same finite state machine and private handle.
// No application operation is accepted until this routine has completed.
static afatfsOperationStatus_e afatfs_journalMount(void)
{
    // GPT write protection forbids provisioning and recovery writes, including dirty-flag changes.
    if (afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY) {
        return AFATFS_OPERATION_SUCCESS;
    }
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsFile_t *fileHandle = &afatfs.journalFile;
    afatfsOperationStatus_e status;
    uint8_t *sector;
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        return AFATFS_OPERATION_SUCCESS; // protected read-only mount
    }
    switch (journalState->initPhase) {
        case 0: {
            uint64_t capacity = (uint64_t)afatfs.fatSectors + ((uint64_t)afatfs.bitmapLength + 511) / 512 + 64;
            uint64_t bytes = (2 + 2 * capacity) * 512;
            bytes = (bytes + afatfs_clusterSize() - 1) / afatfs_clusterSize() * afatfs_clusterSize();
            if (bytes > UINT32_MAX) {
                afatfs_jfail();
                break;
            }
            journalState->capacity = (uint32_t)capacity;
            journalState->sectors = (uint32_t)(bytes / 512);
            journalState->clusters = journalState->sectors / afatfs.sectorsPerCluster;
            if (journalState->found) {
                fileHandle->exfatEntryCount = 3;
                if (afatfs_exfatLoadEntrySet(fileHandle, AFATFS_JOURNAL_NAME) != 1 || !fileHandle->exfatNoFatChain ||
                    fileHandle->logicalSize != bytes || fileHandle->validDataLength != bytes ||
                    (fileHandle->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) {
                    afatfs_jfail();
                    break;
                }
                journalState->firstCluster = fileHandle->firstCluster;
                journalState->initPhase = 1;
            } else if (journalState->startupDirty) {
                afatfs_jfail();
                break;
            } else {
                journalState->provision = true;
                journalState->candidate = 2;
                journalState->runLength = 0;
                journalState->initPhase = 10;
            }
            break;
        }
        case 1:
            if (afatfs_jio(false, afatfs_jsector(0)) == 1) {
                memcpy(journalState->header, journalState->io, 512);
                journalState->initPhase = 2;
            }
            break;
        case 2:
            if (afatfs_jio(false, afatfs_jsector(1)) == 1) {
                bool a = afatfs_jvalidHeader(journalState->header);
                bool b = afatfs_jvalidHeader(journalState->io);
                if (!a && !b) {
                    afatfs_jfail();
                    break;
                }
                if (b &&
                    (!a || (int32_t)(afatfs_jget(journalState->io + 8) - afatfs_jget(journalState->header + 8)) > 0)) {
                    memcpy(journalState->header, journalState->io, 512);
                    journalState->headerSlot = 1;
                } else {
                    journalState->headerSlot = 0;
                }
                journalState->sequence = afatfs_jget(journalState->header + 8);
                journalState->epoch = afatfs_jget(journalState->header + 12);
                journalState->count = afatfs_jget(journalState->header + 16);
                journalState->active = afatfs_jget(journalState->header + 20) != 0;
                journalState->owned = afatfs_jget(journalState->header + 24) != 0;
                // An owned empty log is also the durable checkpoint of an interrupted
                // clean-marker write. Never infer ownership from an older header when
                // the newest valid header says unowned: an external writer may have
                // dirtied that volume. Recovery must precede any external writer.
                if ((journalState->startupDirty && !journalState->owned) ||
                    (!journalState->startupDirty && journalState->active)) {
                    afatfs_jfail();
                    break;
                }
                journalState->progress = journalState->active ? journalState->count : 0;
                journalState->initPhase = 21;
            }
            break;
        case 3:
            if (!journalState->progress) {
                journalState->active = false;
                journalState->count = 0;
                journalState->initPhase = 6;
                break;
            }
            if (afatfs_jio(false, afatfs_jsector(2 + 2 * (journalState->progress - 1))) == 1) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                journalState->target = afatfs_jget(journalState->io + 12);
                journalState->imageCRC = afatfs_jget(journalState->io + 16);
                journalState->initPhase = 4;
            }
            break;
        case 4:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (journalState->progress - 1))) == 1) {
                if (afatfs_jcrc(journalState->io, 512) != journalState->imageCRC) {
                    afatfs_jfail();
                    break;
                }
                journalState->initPhase = 5;
            }
            break;
        case 5:
            if (afatfs_jio(true, journalState->target) == 1) {
                journalState->progress--;
                journalState->initPhase = 3;
            }
            break;
        case 6:
            if (!journalState->startupDirty || afatfs_jwriteHeader() == 1) {
                // Cached root/FAT pages may describe the interrupted state.
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs.bitmapCursorCluster = afatfs.bitmapCursorIndex = 0;
                afatfs.readOnly = false;
                afatfs.lastError = AFATFS_ERROR_NONE;
                afatfs_chdir(NULL);
                journalState->initPhase = 7;
            }
            break;
        case 7: // Mark ownership only after the dirty boot marker is durable.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->initPhase = 8;
            break;
        case 8:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->owned = true;
            journalState->initPhase = 9;
            break;
        case 9:
            if (afatfs_jwriteHeader() == 1) {
                journalState->ready = true;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                journalState->pendingCache = -1;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case 10: // First mount of a clean volume: establish the dirty guard before bootstrap.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->initPhase = 11;
            break;
        case 11:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(fileHandle, AFATFS_JOURNAL_NAME,
                FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY,
                AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            journalState->initPhase = 12;
            break;
        case 12:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(fileHandle);
            if (fileHandle->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (fileHandle->type != AFATFS_FILE_TYPE_NORMAL || fileHandle->firstCluster) {
                afatfs_jfail();
                break;
            }
            journalState->initPhase = 13;
            break;
        case 13: {
            // Locate a contiguous extent. The journal locator never needs the FAT at recovery.
            bool allocated;
            if (journalState->candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_exfatBitmap(journalState->candidate, false, false, &allocated);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->runLength = allocated ? 0 : journalState->runLength + 1;
            journalState->candidate++;
            if (journalState->runLength == journalState->clusters) {
                journalState->firstCluster = journalState->candidate - journalState->clusters;
                journalState->progress = 0;
                journalState->initPhase = 14;
            }
            break;
        }
        case 14:
            if (journalState->progress == journalState->sectors) {
                journalState->progress = 0;
                journalState->initPhase = 15;
                break;
            }
            if (!journalState->ioPending && !journalState->ioDone) {
                memset(journalState->io, 0, 512);
            }
            if (afatfs_jio(true, afatfs_jsector(journalState->progress)) == 1) {
                journalState->progress++;
            }
            break;
        case 15:
            if (journalState->progress == journalState->clusters) {
                journalState->initPhase = 16;
                break;
            }
            status = afatfs_exfatBitmap(journalState->firstCluster + journalState->progress, true, true, NULL);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->progress++;
            break;
        case 16:
            if (!afatfs_flushCache()) {
                break;
            }
            fileHandle->firstCluster = journalState->firstCluster;
            fileHandle->exfatNoFatChain = true;
            fileHandle->physicalSize = fileHandle->logicalSize = fileHandle->validDataLength =
                fileHandle->committedDataLength = journalState->sectors * 512;
            journalState->initPhase = 17;
            break;
        case 17:
            status = afatfs_saveDirectoryEntry(fileHandle, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->initPhase = 18;
            break;
        case 18:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->epoch = 1;
            journalState->owned = true;
            journalState->active = false;
            journalState->count = 0;
            journalState->initPhase = 19;
            break;
        case 19:
            if (afatfs_jwriteHeader() == 1) {
                journalState->initPhase = 20;
            }
            break;
        case 20:
            if (afatfs_jwriteHeader() == 1) {
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                journalState->pendingCache = -1;
                afatfs_chdir(NULL);
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case 21: // Validate the ENTIRE log before the first recovery write.
            if (!journalState->progress) {
                journalState->progress = journalState->active ? journalState->count : 0;
                journalState->initPhase = 3;
                break;
            }
            if (afatfs_jio(false, afatfs_jsector(2 + 2 * (journalState->progress - 1))) == 1) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                journalState->imageCRC = afatfs_jget(journalState->io + 16);
                journalState->initPhase = 22;
            }
            break;
        case 22:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (journalState->progress - 1))) == 1) {
                if (afatfs_jcrc(journalState->io, 512) != journalState->imageCRC) {
                    afatfs_jfail();
                    break;
                }
                journalState->progress--;
                journalState->initPhase = 21;
            }
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
