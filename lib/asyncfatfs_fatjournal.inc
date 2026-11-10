/* FAT32 journal discovery, FAT-chain provisioning and clean markers. */
static afatfsOperationStatus_e afatfs_fjDirty(bool dirty)
{
    afatfsFATSector_t primary;
    uint8_t *mirror;
    afatfsOperationStatus_e status =
        afatfs_FATWriteSector(0, &primary, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
    if (status == AFATFS_OPERATION_SUCCESS) {
        primary.fat32[1] = dirty ? primary.fat32[1] & ~0x08000000U : primary.fat32[1] | 0x08000000U;
        memcpy(mirror, primary.bytes, 512);
    }
    return status;
}

static afatfsOperationStatus_e afatfs_fatJournalMount(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    afatfsFilePtr_t file = &afatfs.openFiles[0];
    uint8_t *buffer;
    uint32_t value;
    afatfsOperationStatus_e status;
    if (state->phase == AFATFS_JOURNAL_ERROR) {
        afatfs_findLast(&afatfs.currentDirectory);
        afatfs_initFileHandle(file);
        afatfs_chdir(NULL);
        return AFATFS_OPERATION_SUCCESS;
    }
    switch (state->initPhase) {
        case 0:
            status = afatfs_cacheSector(afatfs.partitionStartSector, &buffer, AFATFS_CACHE_READ, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            state->serial = ((fatVolumeID_t *)buffer)->fat.fatDescriptor.fat32.volumeID;
            uint64_t capacity = (uint64_t)2 * afatfs.fatSectors + 64;
            uint64_t sectors =
                (2 + 2 * capacity + afatfs.sectorsPerCluster - 1) / afatfs.sectorsPerCluster * afatfs.sectorsPerCluster;
            if (sectors * 512 > UINT32_MAX || sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            state->capacity = (uint32_t)capacity;
            state->sectors = (uint32_t)sectors;
            state->pendingCache = -1;
            state->initPhase = 1;
            break;
        case 1:
        case 2:
            status = afatfs_FATGetNextCluster(state->initPhase - 1, 1, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            state->startupDirty |= !(value & 0x08000000U);
            if (++state->initPhase == 3) {
                afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
            }
            break;
        case 3: {
            fatDirectoryEntry_t *entry;
            status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (entry && !fat_isDirectoryEntryEmpty(entry) && !memcmp(entry->fileName, "AFATFS  JNL", 11)) {
                afatfs_fileLoadDirectoryEntry(file, entry);
                if (file->type != AFATFS_FILE_TYPE_NORMAL || file->logicalSize != state->sectors * 512 ||
                    (file->attrib & 7) != 7 || file->firstCluster < 2 ||
                    file->firstCluster - 2 + state->sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                    afatfs_jfail();
                    break;
                }
                state->firstCluster = file->firstCluster;
                state->found = true;
                afatfs_findLast(&afatfs.currentDirectory);
                state->progress = 0;
                state->initPhase = 50;
            } else if (!entry || fat_isDirectoryEntryTerminator(entry)) {
                afatfs_findLast(&afatfs.currentDirectory);
                if (state->startupDirty) {
                    afatfs_jfail();
                } else {
                    state->provision = true;
                    state->initPhase = 30;
                }
            }
            break;
        }
        case 50:
        case 51:
            status = afatfs_FATGetNextCluster(state->initPhase - 50, state->firstCluster + state->progress, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (state->progress + 1 == state->sectors / afatfs.sectorsPerCluster
                    ? !afatfs_FATIsEndOfChainMarker(value)
                    : value != state->firstCluster + state->progress + 1) {
                afatfs_jfail();
                break;
            }
            if (state->initPhase == 50) {
                state->initPhase = 51;
            } else if (++state->progress == state->sectors / afatfs.sectorsPerCluster) {
                state->initPhase = 4;
            } else {
                state->initPhase = 50;
            }
            break;
        case 4:
        case 5:
            if (afatfs_jio(false, afatfs_jsector(state->initPhase - 4))) {
                afatfs_jselectHeader(state->initPhase - 4);
                if (++state->initPhase == 6) {
                    if (!state->validHeader || (state->startupDirty && !state->owned) ||
                        (!state->startupDirty && state->active)) {
                        afatfs_jfail();
                    } else {
                        state->scan = 0;
                        state->initPhase = 7;
                    }
                }
            }
            break;
        case 7:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                state->owned = true;
                state->initPhase = 13;
            }
            break;
        case 13:
            if (afatfs_jwriteHeader()) {
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                state->initPhase = 14;
            }
            break;
        case 14:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                state->initPhase = 15;
            }
            break;
        case 15:
            if (afatfs_flushCache()) {
                state->owned = true;
                state->initPhase = 16;
            }
            break;
        case 16:
            if (afatfs_jwriteHeader()) {
                state->ready = true;
                state->provision = false;
                state->phase = AFATFS_JOURNAL_IDLE;
                state->pendingCache = -1;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case 30:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                state->initPhase = 31;
            }
            break;
        case 31:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(
                file, AFATFS_JOURNAL_NAME, 7, AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            state->initPhase = 32;
            break;
        case 32:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(file);
            if (file->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (file->type != AFATFS_FILE_TYPE_NORMAL || file->firstCluster) {
                afatfs_jfail();
                break;
            }
            state->candidate = 2;
            state->runLength = 0;
            state->initPhase = 33;
            break;
        case 33:
            if (state->candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_FATGetNextCluster(0, state->candidate, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            state->runLength = value ? 0 : state->runLength + 1;
            state->candidate++;
            if (state->runLength == state->sectors / afatfs.sectorsPerCluster) {
                state->firstCluster = state->candidate - state->runLength;
                state->progress = 0;
                state->initPhase = 34;
            }
            break;
        case 34:
            if (state->progress == state->sectors) {
                state->progress = 0;
                state->initPhase = 35;
            } else {
                if (!state->ioPending && !state->ioDone) {
                    memset(state->io, 0, 512);
                }
                if (afatfs_jio(true, afatfs_jsector(state->progress))) {
                    state->progress++;
                }
            }
            break;
        case 35:
            if (state->progress == state->sectors / afatfs.sectorsPerCluster) {
                state->initPhase = 36;
            } else {
                value = state->firstCluster + state->progress;
                status = afatfs_FATSetNextCluster(
                    value, state->progress + 1 == state->sectors / afatfs.sectorsPerCluster ? UINT32_MAX : value + 1);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                state->progress++;
            }
            break;
        case 36:
            if (afatfs_flushCache()) {
                file->firstCluster = state->firstCluster;
                file->physicalSize = state->sectors * 512;
                file->logicalSize = file->physicalSize;
                state->initPhase = 37;
            }
            break;
        case 37:
            status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status == AFATFS_OPERATION_SUCCESS) {
                state->initPhase = 38;
            }
            break;
        case 38:
            if (afatfs_flushCache()) {
                state->epoch = 1;
                state->owned = true;
                state->initPhase = 39;
            }
            break;
        case 39:
            if (afatfs_jwriteHeader()) {
                state->initPhase = 40;
            }
            break;
        case 40:
            if (afatfs_jwriteHeader()) {
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                state->ready = true;
                state->provision = false;
                state->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        default:
            afatfs_jfail();
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

static bool afatfs_fatJournalShutdown(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (afatfs.readOnly || !state->firstCluster) {
        return true;
    }
    switch (state->shutdownPhase) {
        case 0:
            if (!afatfs_journalCommitted()) {
                return false;
            }
            state->ready = false;
            state->shutdownPhase = 1;
            // Fall through
        case 1:
            if (afatfs_fjDirty(false) != AFATFS_OPERATION_SUCCESS) {
                return false;
            }
            state->shutdownPhase = 2;
            return false;
        case 2:
            if (!afatfs_flushCache()) {
                return false;
            }
            state->owned = false;
            state->shutdownPhase = 3;
            // Fall through
        case 3:
            if (!afatfs_jwriteHeader()) {
                return false;
            }
            state->shutdownPhase = 4;
            return true;
        default:
            return true;
    }
}
