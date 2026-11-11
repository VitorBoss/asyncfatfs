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
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsFilePtr_t file = &afatfs.openFiles[0];
    uint8_t *buffer;
    uint32_t value;
    afatfsOperationStatus_e status;
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        afatfs_findLast(&afatfs.currentDirectory);
        afatfs_initFileHandle(file);
        afatfs_chdir(NULL);
        return AFATFS_OPERATION_SUCCESS;
    }
    switch (journalState->lifecycle.initPhase) {
        case AFATFS_JOURNAL_INIT_PREPARE:
            status = afatfs_cacheSector(afatfs.partitionStartSector, &buffer, AFATFS_CACHE_READ, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->serial = ((fatVolumeID_t *)buffer)->fat.fatDescriptor.fat32.volumeID;
            uint64_t capacity = (uint64_t)2 * afatfs.fatSectors + 64;
            uint64_t sectors =
                (2 + 2 * capacity + afatfs.sectorsPerCluster - 1) / afatfs.sectorsPerCluster * afatfs.sectorsPerCluster;
            if (sectors * 512 > UINT32_MAX || sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            journalState->capacity = (uint32_t)capacity;
            journalState->sectors = (uint32_t)sectors;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS;
            break;
        case AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS:
        case AFATFS_JOURNAL_INIT_FAT_READ_MIRROR_STATUS:
            status = afatfs_FATGetNextCluster(
                journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS ? 0 : 1, 1, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->startupDirty |= !(value & 0x08000000U);
            if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_READ_MIRROR_STATUS;
            } else {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_FIND_FILE;
                afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
            }
            break;
        case AFATFS_JOURNAL_INIT_FAT_FIND_FILE: {
            fatDirectoryEntry_t *entry;
            status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (entry && !fat_isDirectoryEntryEmpty(entry) && !memcmp(entry->fileName, "AFATFS  JNL", 11)) {
                afatfs_fileLoadDirectoryEntry(file, entry);
                if (file->type != AFATFS_FILE_TYPE_NORMAL || file->logicalSize != journalState->sectors * 512 ||
                    (file->attrib & 7) != 7 || file->firstCluster < 2 ||
                    file->firstCluster - 2 + journalState->sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                    afatfs_jfail();
                    break;
                }
                journalState->firstCluster = file->firstCluster;
                journalState->found = true;
                afatfs_findLast(&afatfs.currentDirectory);
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN;
            } else if (!entry || fat_isDirectoryEntryTerminator(entry)) {
                afatfs_findLast(&afatfs.currentDirectory);
                if (journalState->startupDirty) {
                    afatfs_jfail();
                } else {
                    journalState->provision = true;
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY;
                }
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN:
        case AFATFS_JOURNAL_INIT_FAT_VALIDATE_MIRROR_CHAIN:
            status = afatfs_FATGetNextCluster(
                journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN ? 0 : 1,
                journalState->firstCluster + journalState->work.setup.progress, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (journalState->work.setup.progress + 1 == journalState->sectors / afatfs.sectorsPerCluster
                    ? !afatfs_FATIsEndOfChainMarker(value)
                    : value != journalState->firstCluster + journalState->work.setup.progress + 1) {
                afatfs_jfail();
                break;
            }
            if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_MIRROR_CHAIN;
            } else {
                journalState->work.setup.progress++;
                if (journalState->work.setup.progress == journalState->sectors / afatfs.sectorsPerCluster) {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER;
                } else {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN;
                }
            }
            break;
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
        case AFATFS_JOURNAL_INIT_RECOVER:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT;
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT:
            if (afatfs_jwriteHeader()) {
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_MARK_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_MARK_DIRTY:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FLUSH_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_FLUSH_DIRTY:
            if (afatfs_flushCache()) {
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP;
            }
            break;
        case AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP:
            if (afatfs_jwriteHeader()) {
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(
                file, AFATFS_JOURNAL_NAME, 7, AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(file);
            if (file->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (file->type != AFATFS_FILE_TYPE_NORMAL || file->firstCluster) {
                afatfs_jfail();
                break;
            }
            journalState->work.setup.candidate = 2;
            journalState->work.setup.runLength = 0;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT:
            if (journalState->work.setup.candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_FATGetNextCluster(0, journalState->work.setup.candidate, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.runLength = value ? 0 : journalState->work.setup.runLength + 1;
            journalState->work.setup.candidate++;
            if (journalState->work.setup.runLength == journalState->sectors / afatfs.sectorsPerCluster) {
                journalState->firstCluster = journalState->work.setup.candidate - journalState->work.setup.runLength;
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors) {
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT;
            } else {
                if (!journalState->ioPending && !journalState->ioDone) {
                    memset(journalState->io, 0, 512);
                }
                if (afatfs_jio(true, afatfs_jsector(journalState->work.setup.progress))) {
                    journalState->work.setup.progress++;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors / afatfs.sectorsPerCluster) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION;
            } else {
                value = journalState->firstCluster + journalState->work.setup.progress;
                status = afatfs_FATSetNextCluster(value,
                    journalState->work.setup.progress + 1 == journalState->sectors / afatfs.sectorsPerCluster
                        ? UINT32_MAX
                        : value + 1);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                journalState->work.setup.progress++;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION:
            if (afatfs_flushCache()) {
                file->firstCluster = journalState->firstCluster;
                file->physicalSize = journalState->sectors * 512;
                file->logicalSize = file->physicalSize;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE:
            status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE:
            if (afatfs_flushCache()) {
                journalState->epoch = 1;
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER:
            if (afatfs_jwriteHeader()) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER:
            if (afatfs_jwriteHeader()) {
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
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
    afatfsJournal_t *journalState = &afatfs.journal;
    if (afatfs.readOnly || !journalState->firstCluster) {
        return true;
    }
    switch (journalState->lifecycle.shutdownPhase) {
        case AFATFS_JOURNAL_SHUTDOWN_WAIT_COMMIT:
            if (!afatfs_journalCommitted()) {
                return false;
            }
            journalState->ready = false;
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_MARK_CLEAN;
            // Fall through
        case AFATFS_JOURNAL_SHUTDOWN_MARK_CLEAN:
            if (afatfs_fjDirty(false) != AFATFS_OPERATION_SUCCESS) {
                return false;
            }
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_FLUSH_CLEAN;
            return false;
        case AFATFS_JOURNAL_SHUTDOWN_FLUSH_CLEAN:
            if (!afatfs_flushCache()) {
                return false;
            }
            journalState->owned = false;
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_RELEASE_OWNERSHIP;
            // Fall through
        case AFATFS_JOURNAL_SHUTDOWN_RELEASE_OWNERSHIP:
            if (!afatfs_jwriteHeader()) {
                return false;
            }
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_COMPLETE;
            return true;
        case AFATFS_JOURNAL_SHUTDOWN_COMPLETE:
        default:
            return true;
    }
}
