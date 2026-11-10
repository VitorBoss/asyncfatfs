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
            journalState->serial = afatfs.volumeSerial;
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
        case 2:
            if (afatfs_jio(false, afatfs_jsector(journalState->initPhase - 1))) {
                afatfs_jselectHeader(journalState->initPhase - 1);
                if (++journalState->initPhase == 3) {
                    if (!journalState->validHeader || (journalState->startupDirty && !journalState->owned) ||
                        (!journalState->startupDirty && journalState->active)) {
                        afatfs_jfail();
                    } else {
                        journalState->initPhase = 21;
                    }
                }
            }
            break;
        case 6:
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
            if (journalState->runLength == (journalState->sectors / afatfs.sectorsPerCluster)) {
                journalState->firstCluster =
                    journalState->candidate - (journalState->sectors / afatfs.sectorsPerCluster);
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
            if (journalState->progress == (journalState->sectors / afatfs.sectorsPerCluster)) {
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
            fileHandle->physicalSize = journalState->sectors * 512;
            fileHandle->logicalSize = fileHandle->physicalSize;
            fileHandle->validDataLength = fileHandle->physicalSize;
            fileHandle->committedDataLength = fileHandle->physicalSize;
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
        case 21:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                journalState->initPhase = 6;
            }
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
