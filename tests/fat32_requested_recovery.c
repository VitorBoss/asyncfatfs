// Synthetic batch power cuts. All large images/queues below belong to the host harness, not the library.
#define main requested_recovery_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

/** @brief Keep the backend-owned source immutable across every polled transition. */
static void pumpChecked(void)
{
    if (io.pending && io.write) {
        assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
    }
    pump();
    if (io.pending && io.write) {
        assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
    }
}
#define pump pumpChecked

/** @brief Mount with diagnostic failure output and enough polls for a dirty-volume scan. */
static void mountRecovered(void)
{
    afatfs_init();
    for (unsigned retry = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; retry++) {
        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL || retry == 2000000) {
            fprintf(stderr, "Recovery stopped: init=%u phase=%u window=%u candidate=%u sector=%u copy=%u\n",
                afatfs.initPhase, afatfs.currentDirectory.operation.state.batchRecovery.phase,
                afatfs.currentDirectory.operation.state.batchRecovery.windowStart,
                afatfs.currentDirectory.operation.state.batchRecovery.candidate,
                afatfs.currentDirectory.operation.state.batchRecovery.tableSector,
                afatfs.currentDirectory.operation.state.batchRecovery.tableCopy);
            abort();
        }
        assert(!afatfs_canPowerOff());
        pump();
    }
}

/** @brief Read a FAT32 entry directly from the synthetic media. */
static uint32_t mediaLink(unsigned copy, uint32_t cluster)
{
    return get32(media + afatfs_fatSectorToPhysical(copy, cluster / 128) * SECTOR + cluster % 128 * 4) & 0x0FFFFFFF;
}

/** @brief Mark a complete allocated chain in the host-only reachability map. */
static void markChain(uint8_t *visited, uint32_t cluster)
{
    for (unsigned count = 0; cluster >= 2 && cluster < afatfs.numClusters + 2; count++) {
        assert(count < afatfs.numClusters);
        if (visited[cluster]) {
            return;
        }
        visited[cluster] = 1;
        uint32_t next = mediaLink(0, cluster);
        assert(next);
        if (fat32_isEndOfChainMarker(next)) {
            return;
        }
        cluster = next;
    }
    assert(false);
}

/** @brief Check directory reachability, mirrored FATs and absence of remaining allocated orphan heads. */
static void auditRecovered(void)
{
    assert(!afatfs_isReadOnly());
    assert(!memcmp(media + afatfs.fatStartSector * SECTOR,
        media + (afatfs.fatStartSector + afatfs.fatSectors) * SECTOR, afatfs.fatSectors * SECTOR));
    assert(get32(media + afatfs.fatStartSector * SECTOR + 4) & AFATFS_BATCH_CLEAN_BIT);
    uint8_t *visited = calloc(afatfs.numClusters + 2, 1);
    uint32_t *directories = calloc(afatfs.numClusters + 2, sizeof(uint32_t));
    assert(visited && directories);
    unsigned count = 1;
    directories[0] = afatfs.rootDirectoryCluster;
    for (unsigned position = 0; position < count; position++) {
        uint32_t first = directories[position];
        markChain(visited, first);
        for (uint32_t current = first;;) {
            const fatDirectoryEntry_t *entries = (const fatDirectoryEntry_t *)(media + afatfs_fileClusterToPhysical(current, 0) * SECTOR);
            bool finished = false;
            for (unsigned index = 0; index < SECTOR / 32; index++) {
                const fatDirectoryEntry_t *entry = &entries[index];
                if (!entry->fileName[0]) {
                    finished = true;
                    break;
                }
                if (entry->fileName[0] == FAT_DELETED_FILE_MARKER || (entry->attrib & FAT_FILE_ATTRIBUTE_VOLUME_ID)) {
                    continue;
                }
                uint32_t cluster = (((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow) & 0x0FFFFFFF;
                if (cluster) {
                    markChain(visited, cluster);
                }
                if ((entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) && entry->fileName[0] != '.') {
                    assert(cluster && count < afatfs.numClusters);
                    directories[count++] = cluster;
                }
            }
            uint32_t next = mediaLink(0, current);
            if (finished || fat32_isEndOfChainMarker(next)) {
                break;
            }
            assert(next >= 2 && next < afatfs.numClusters + 2);
            current = next;
        }
    }
    for (uint32_t cluster = 2; cluster < afatfs.numClusters + 2; cluster++) {
        uint32_t value = mediaLink(0, cluster);
        if (value && !(value >= 0x0FFFFFF0 && value < 0x0FFFFFF8)) {
            if (!visited[cluster]) {
                fprintf(stderr, "Unrecovered orphan %u -> %08X\n", cluster, value);
                abort();
            }
        }
    }
    free(directories);
    free(visited);
}

/** @brief Verify committed data in both the root and a nested directory. */
static void checkCommitted(void)
{
#ifdef TEST_SMALL_COLLECTION
    afatfsFile_t *prefix = openFile("BATCHA.BIN", "r");
    uint8_t committed[SECTOR];
    assert(prefix && prefix->logicalSize >= SECTOR);
    readBytes(prefix, committed, sizeof(committed));
    for (unsigned index = 0; index < sizeof(committed); index++) {
        assert(committed[index] == 0x52);
    }
    closeFile(prefix);
#endif
    afatfsFile_t *file = openFile("KEEP.BIN", "r");
    uint8_t payload[SECTOR];
    assert(file && file->logicalSize == SECTOR);
    readBytes(file, payload, sizeof(payload));
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == 0x52);
    }
    closeFile(file);
    afatfsFile_t *directory = openFile("KEEPDIR", "r");
    assert(directory && afatfs_chdir(directory));
    file = openFile("INNER.BIN", "r");
    assert(file && file->logicalSize == SECTOR);
    readBytes(file, payload, sizeof(payload));
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == 0x52);
    }
    closeFile(file);
    assert(afatfs_chdir(NULL));
    closeFile(directory);
}

/** @brief Interleave small requests with batches while checking backend-owned buffers and exact synced sizes. */
static void checkMixedWriters(void)
{
    afatfsFile_t *files[2];
    uint8_t payload[8192];
    unsigned offsets[2] = {0};
    memset(payload, 0xA7, sizeof(payload));
    files[0] = openFile("MIXBIG.BIN", "w");
    files[1] = openFile("MIXSMALL.BIN", "w");
    for (unsigned retry = 0; offsets[0] < sizeof(payload) || offsets[1] < sizeof(payload); retry++) {
        assert(retry < 200000);
        for (unsigned index = 0; index < 2; index++) {
            unsigned request = index ? 257 : 4096;
            unsigned remaining = sizeof(payload) - offsets[index];
            if (request > remaining) {
                request = remaining;
            }
            if (request) {
                offsets[index] += afatfs_fwrite(files[index], payload + offsets[index], request);
            }
        }
        pump();
    }
    for (unsigned index = 0; index < 2; index++) {
#ifndef TEST_SMALL_COLLECTION
        while (afatfs_fileIsBusy(files[index])) {
            pump();
        }
#endif
        closeFile(files[index]);
    }
    unmount();
    mountRecovered();
    const char *names[2] = {"MIXBIG.BIN", "MIXSMALL.BIN"};
    for (unsigned index = 0; index < 2; index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        assert(file && file->logicalSize == sizeof(payload));
        memset(payload, 0, sizeof(payload));
        readBytes(file, payload, sizeof(payload));
        for (unsigned position = 0; position < sizeof(payload); position++) {
            assert(payload[position] == 0xA7);
        }
        closeFile(file);
    }
    auditRecovered();
    unmount();
}

/** @brief Queue a new batch during clean-marker publication and require a fresh durable dirty marker. */
static void checkCleanMarkerInterlock(void)
{
    mountRecovered();
    afatfsFile_t *first = openFile("SYNCA.BIN", "w");
    afatfsFile_t *second = openFile("SYNCB.BIN", "w");
    uint8_t payload[4096];
    memset(payload, 0x39, sizeof(payload));
    writeBytes(first, payload, sizeof(payload));
    while (afatfs_fileIsBusy(first)) {
        pump();
    }
    completed = false;
    assert(afatfs_fsync(first, fileDone, &callbackContext));
    for (unsigned retry = 0; !afatfs.batchMarkingClean; retry++) {
        assert(retry < 200000 && !completed);
        pump();
    }
    assert(afatfs_fwrite(second, payload, sizeof(payload)) == 0);
    assert(second->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER);
    for (unsigned retry = 0; afatfs.batchMarkingClean; retry++) {
        assert(retry < 200000 && second->physicalSize == 0);
        pump();
    }
    for (unsigned retry = 0; !second->physicalSize; retry++) {
        assert(retry < 200000);
        pump();
    }
    for (unsigned copy = 0; copy < 2; copy++) {
        assert(!(get32(media + afatfs_fatSectorToPhysical(copy, 0) * SECTOR + 4) & AFATFS_BATCH_CLEAN_BIT));
    }
    writeBytes(second, payload, sizeof(payload));
    while (afatfs_fileIsBusy(second) || afatfs_fileIsBusy(first)) {
        pump();
    }
    closeFile(first);
    closeFile(second);
    auditRecovered();
    unmount();
}

/** @brief Replay every successful sector boundary and both half-sector tears of a two-writer batch. */
int main(void)
{
    exerciseFat(true, 1);
    mount();
    uint8_t payload[4096];
    memset(payload, 0x52, sizeof(payload));
    afatfsFile_t *file = openFile("KEEP.BIN", "w");
    writeBytes(file, payload, SECTOR);
    closeFile(file);
    completed = false;
    assert(afatfs_mkdir("KEEPDIR", fileDone, &callbackContext));
    waitDone();
    afatfsFile_t *directory = opened;
    assert(directory && afatfs_chdir(directory));
    file = openFile("INNER.BIN", "w");
    writeBytes(file, payload, SECTOR);
    closeFile(file);
    assert(afatfs_chdir(NULL));
    closeFile(directory);
    afatfsFile_t *files[2];
    files[0] = openFile("BATCHA.BIN", "w");
    files[1] = openFile("BATCHB.BIN", "w");
#ifdef TEST_SMALL_COLLECTION
    writeBytes(files[0], payload, 257);
    writeBytes(files[0], payload + 257, SECTOR - 257);
#endif
    for (unsigned index = 0; index < 2; index++) {
        completed = false;
        assert(afatfs_fsync(files[index], fileDone, &callbackContext));
        waitDone();
    }
    uint8_t *baseline = malloc(sizeof(media));
    uint8_t *interrupted = malloc(sizeof(media));
    assert(baseline && interrupted);
    memcpy(baseline, media, sizeof(media));
    uint32_t primarySector = afatfs_fatSectorToPhysical(0, 5000 / 128);
    uint32_t mirrorSector = afatfs_fatSectorToPhysical(1, 5000 / 128);
    uint32_t directorySector = files[0]->directoryEntryPos.sectorNumberPhysical;
    unsigned directoryOffset = files[0]->directoryEntryPos.entryIndex * 32;
    bool savedInterruption = false;
    static durableWrite_t mutation[TRACE_CAPACITY];
    captureWrites = mutation;
    captureCount = 0;
    afatfs.lastClusterAllocated = 5000;
    memset(payload, 0x6D, sizeof(payload));
    unsigned offsets[2] = {0};
    for (unsigned retry = 0; offsets[0] < sizeof(payload) || offsets[1] < sizeof(payload); retry++) {
        assert(retry < 200000);
        for (unsigned index = 0; index < 2; index++) {
            if (offsets[index] < sizeof(payload)) {
                unsigned request = sizeof(payload) - offsets[index];
#ifdef TEST_SMALL_COLLECTION
                request = MIN(request, 257);
#endif
                offsets[index] += afatfs_fwrite(files[index], payload + offsets[index], request);
            }
        }
        pump();
    }
    for (unsigned index = 0; index < 2; index++) {
#ifndef TEST_SMALL_COLLECTION
        while (afatfs_fileIsBusy(files[index])) {
            pump();
        }
#endif
        closeFile(files[index]);
    }
    unsigned mutations = captureCount;
    captureWrites = NULL;
    assert(mutations > 8);
    unmount();
    unsigned cuts = 0;
    for (unsigned prefix = 0; prefix <= mutations; prefix++) {
        for (unsigned mode = 0; mode < (prefix < mutations ? 3U : 1U); mode++) {
            memcpy(media, baseline, sizeof(media));
            for (unsigned index = 0; index < prefix; index++) {
                memcpy(media + mutation[index].sector * SECTOR, mutation[index].bytes, SECTOR);
            }
            if (mode) {
                unsigned offset = mode == 1 ? 0 : SECTOR / 2;
                memcpy(media + mutation[prefix].sector * SECTOR + offset, mutation[prefix].bytes + offset, SECTOR / 2);
            }
            uint32_t next = get32(media + primarySector * SECTOR + (5000 % 128) * 4) & 0x0FFFFFFF;
            const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)(media + directorySector * SECTOR + directoryOffset);
            if (!savedInterruption && next >= 2 && next < 0x0FFFFFF0 &&
#ifdef TEST_SMALL_COLLECTION
                entry->fileSize == SECTOR &&
#else
                !entry->firstClusterLow && !entry->firstClusterHigh &&
#endif
                memcmp(media + primarySector * SECTOR, media + mirrorSector * SECTOR, SECTOR)) {
                memcpy(interrupted, media, sizeof(media));
                savedInterruption = true;
            }
            mountRecovered();
            checkCommitted();
            auditRecovered();
            unmount();
            cuts++;
        }
    }
    assert(savedInterruption);
    static durableWrite_t replay[TRACE_CAPACITY];
    memcpy(media, interrupted, sizeof(media));
    captureWrites = replay;
    captureCount = 0;
    mountRecovered();
    captureWrites = NULL;
    unsigned replays = captureCount;
    assert(replays >= 4);
    checkCommitted();
    auditRecovered();
    unmount();
    unsigned recoveryCuts = 0;
    for (unsigned prefix = 0; prefix <= replays; prefix++) {
        for (unsigned mode = 0; mode < (prefix < replays ? 3U : 1U); mode++) {
            memcpy(media, interrupted, sizeof(media));
            for (unsigned index = 0; index < prefix; index++) {
                memcpy(media + replay[index].sector * SECTOR, replay[index].bytes, SECTOR);
            }
            if (mode) {
                unsigned offset = mode == 1 ? 0 : SECTOR / 2;
                memcpy(media + replay[prefix].sector * SECTOR + offset, replay[prefix].bytes + offset, SECTOR / 2);
            }
            mountRecovered();
            checkCommitted();
            auditRecovered();
            unmount();
            recoveryCuts++;
        }
    }
    const backendFailure_e failures[] = {FAIL_READ_SUBMIT, FAIL_READ_CALLBACK, FAIL_WRITE_SUBMIT, FAIL_WRITE_CALLBACK};
    for (unsigned index = 0; index < sizeof(failures) / sizeof(failures[0]); index++) {
        for (unsigned copy = 0; copy < 2; copy++) {
            memcpy(media, interrupted, sizeof(media));
            backendFailure.kind = failures[index];
            backendFailure.target = TARGET_PREFETCH;
            backendFailure.sector = copy ? mirrorSector : primarySector;
            backendFailure.hits = 0;
            afatfs_init();
            for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
                assert(retry < 2000000 && !afatfs_canPowerOff());
                assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
                pump();
            }
            backendFailure.kind = FAIL_NONE;
            for (unsigned retry = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; retry++) {
                assert(retry < 2000000 && !afatfs_canPowerOff());
                assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
                pump();
            }
            checkCommitted();
            auditRecovered();
            unmount();
        }
    }
    memcpy(media, baseline, sizeof(media));
    mountRecovered();
    checkMixedWriters();
    checkCleanMarkerInterlock();
    free(interrupted);
    free(baseline);
    printf("Requested batch recovery: %u write cuts, %u recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.\n",
        cuts, recoveryCuts);
    return 0;
}
