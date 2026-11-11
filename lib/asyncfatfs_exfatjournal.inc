// Mount/recovery/provisioning temporarily borrows the first public handle.
// No application operation is accepted until this routine has completed.
static afatfsOperationStatus_e afatfs_journalMount(void)
{
    // GPT write protection forbids provisioning and recovery writes, including dirty-flag changes.
    if (afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY) {
        afatfs_initFileHandle(&afatfs.openFiles[0]);
        return AFATFS_OPERATION_SUCCESS;
    }
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsFile_t *fileHandle = &afatfs.openFiles[0];
    afatfsOperationStatus_e status;
    uint8_t *sector;
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        afatfs_initFileHandle(fileHandle);
        return AFATFS_OPERATION_SUCCESS; // protected read-only mount
    }
    switch (journalState->lifecycle.initPhase) {
        case AFATFS_JOURNAL_INIT_PREPARE: {
            uint64_t capacity = (uint64_t)afatfs.fatSectors + ((uint64_t)afatfs.bitmapLength + 511) / 512 + 64;
            uint64_t bytes = (2 + 2 * capacity) * 512;
            bytes = (bytes + afatfs_clusterSize() - 1) / afatfs_clusterSize() * afatfs_clusterSize();
            if (bytes > UINT32_MAX) {
                afatfs_jfail();
                break;
            }
            journalState->capacity = (uint32_t)capacity;
            journalState->sectors = (uint32_t)(bytes / 512);
            if (journalState->found) {
                fileHandle->exfatEntryCount = 3;
                if (afatfs_exfatLoadEntrySet(fileHandle, AFATFS_JOURNAL_NAME) != 1 || !fileHandle->exfatNoFatChain ||
                    fileHandle->logicalSize != bytes || fileHandle->validDataLength != bytes ||
                    (fileHandle->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) {
                    afatfs_jfail();
                    break;
                }
                journalState->firstCluster = fileHandle->firstCluster;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER;
            } else if (journalState->startupDirty) {
                afatfs_jfail();
                break;
            } else {
                journalState->provision = true;
                journalState->work.setup.candidate = 2;
                journalState->work.setup.runLength = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY;
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER:
        case AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER:
            if (afatfs_jio(false,
                    afatfs_jsector(
                        journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1))) {
                afatfs_jselectHeader(
                    journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1);
                if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER) {
                    if (!journalState->validHeader || (journalState->startupDirty && !journalState->owned) ||
                        (!journalState->startupDirty && journalState->active)) {
                        afatfs_jfail();
                    } else {
                        journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVER;
                    }
                } else {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT:
            if (!journalState->startupDirty || afatfs_jwriteHeader() == 1) {
                // Cached root/FAT pages may describe the interrupted state.
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs.bitmapCursorCluster = 0;
                afatfs.bitmapCursorIndex = 0;
                afatfs.readOnly = false;
                afatfs.lastError = AFATFS_ERROR_NONE;
                afatfs_chdir(NULL);
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_MARK_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_MARK_DIRTY: // Mark ownership only after the dirty boot marker is durable.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FLUSH_DIRTY;
            break;
        case AFATFS_JOURNAL_INIT_FLUSH_DIRTY:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->owned = true;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP;
            break;
        case AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP:
            if (afatfs_jwriteHeader() == 1) {
                afatfs_initFileHandle(fileHandle);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY: // First mount of a clean volume: establish the dirty guard before bootstrap.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(fileHandle, AFATFS_JOURNAL_NAME,
                FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY,
                AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(fileHandle);
            if (fileHandle->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (fileHandle->type != AFATFS_FILE_TYPE_NORMAL || fileHandle->firstCluster) {
                afatfs_jfail();
                break;
            }
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT: {
            // Locate a contiguous extent. The journal locator never needs the FAT at recovery.
            bool allocated;
            if (journalState->work.setup.candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_exfatBitmap(journalState->work.setup.candidate, false, false, &allocated);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.runLength = allocated ? 0 : journalState->work.setup.runLength + 1;
            journalState->work.setup.candidate++;
            if (journalState->work.setup.runLength == (journalState->sectors / afatfs.sectorsPerCluster)) {
                journalState->firstCluster =
                    journalState->work.setup.candidate - (journalState->sectors / afatfs.sectorsPerCluster);
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT;
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors) {
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT;
                break;
            }
            if (!journalState->ioPending && !journalState->ioDone) {
                memset(journalState->io, 0, 512);
            }
            if (afatfs_jio(true, afatfs_jsector(journalState->work.setup.progress)) == 1) {
                journalState->work.setup.progress++;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT:
            if (journalState->work.setup.progress == (journalState->sectors / afatfs.sectorsPerCluster)) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION;
                break;
            }
            status =
                afatfs_exfatBitmap(journalState->firstCluster + journalState->work.setup.progress, true, true, NULL);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.progress++;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION:
            if (!afatfs_flushCache()) {
                break;
            }
            fileHandle->firstCluster = journalState->firstCluster;
            fileHandle->exfatNoFatChain = true;
            fileHandle->physicalSize = journalState->sectors * 512;
            fileHandle->logicalSize = fileHandle->physicalSize;
            fileHandle->validDataLength = fileHandle->physicalSize;
            fileHandle->committedDataLength = fileHandle->physicalSize;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE:
            status = afatfs_saveDirectoryEntry(fileHandle, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->epoch = 1;
            journalState->owned = true;
            journalState->active = false;
            journalState->count = 0;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER:
            if (afatfs_jwriteHeader() == 1) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER:
            if (afatfs_jwriteHeader() == 1) {
                afatfs_initFileHandle(fileHandle);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                afatfs_chdir(NULL);
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVER:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT;
            }
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
