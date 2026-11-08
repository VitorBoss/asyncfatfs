// Focused cache-image tests for the minimal FAT32 build.
#define main mirror_cache_regression_main
#include "test_exfat.c"
#undef main

static unsigned fullImageCopies;
static unsigned entryImageCopies;

static void expectPairedEntryCopy(void)
{
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    assert(fullImageCopies == 0 && entryImageCopies == 1);
#else
    assert(fullImageCopies == 1 && entryImageCopies == 0);
#endif
}

static void *trackMirrorCopy(void *destination, const void *source, size_t length)
{
    if (length == SECTOR) {
        fullImageCopies++;
    } else if (length == sizeof(uint32_t)) {
        entryImageCopies++;
    }
    return memcpy(destination, source, length);
}

#define memcpy trackMirrorCopy
#include "../lib/asyncfatfs.c"
#undef memcpy

static void resetCacheFixture(void)
{
    assert(!io.pending);
    memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
    afatfs.cacheDirtyEntries = 0;
    afatfs.cacheFlushInProgress = false;
    afatfs.lastError = AFATFS_ERROR_NONE;
}

static void installMirrorFixture(void)
{
    resetCacheFixture();
    afatfs_cacheSectorInit(&afatfs.cacheDescriptor[0], afatfs_fatSectorToPhysical(1, 400), false);
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_IN_SYNC;
    memcpy(afatfs_cacheSectorGetMemory(0), media + afatfs_fatSectorToPhysical(0, 400) * SECTOR, SECTOR);
}

static void completeFixtureIO(void)
{
    for (unsigned retry = 0; io.pending; retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
}

static void expectDiskRead(uint32_t primarySector)
{
    uint8_t *buffer;
    unsigned initialReads = reads;
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_IN_PROGRESS);
    assert(io.pending && !io.write);
    completeFixtureIO();
    assert(reads == initialReads + 1);
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_SUCCESS);
    assert(!memcmp(buffer, media + primarySector * SECTOR, SECTOR));
}

static void checkPairedEntryUpdates(uint32_t primarySector)
{
    uint32_t cluster = 400 * AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR + 7;
    uint8_t expected[SECTOR];

    // A poisoned EMPTY mirror must receive all neighboring entries before use.
    resetCacheFixture();
    afatfs_cacheSectorInit(&afatfs.cacheDescriptor[0], primarySector, false);
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_IN_SYNC;
    memcpy(afatfs_cacheSectorGetMemory(0), media + primarySector * SECTOR, SECTOR);
    memcpy(expected, afatfs_cacheSectorGetMemory(0), SECTOR);
    for (unsigned index = 1; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        memset(afatfs_cacheSectorGetMemory(index), 0xA5, SECTOR);
    }
    fullImageCopies = 0;
    entryImageCopies = 0;
    assert(afatfs_FATSetNextCluster(cluster, 0xFFFFFFFF) == AFATFS_OPERATION_SUCCESS);
    put32(expected + 7 * sizeof(uint32_t), 0x8FFFFFFF);
    afatfsCacheBlockDescriptor_t *mirrorDescriptor =
        afatfs_findCacheSector(afatfs_fatSectorToPhysical(1, 400));
    assert(mirrorDescriptor && mirrorDescriptor->state == AFATFS_CACHE_STATE_DIRTY);
    uint8_t *mirror = afatfs_cacheSectorGetMemory((int)(mirrorDescriptor - afatfs.cacheDescriptor));
    assert(!memcmp(mirror, expected, SECTOR));
    assert(fullImageCopies == 1 && entryImageCopies == 0);

    // Both copy strategies preserve neighbors and FAT32 high bits.
    fullImageCopies = 0;
    entryImageCopies = 0;
    assert(afatfs_FATSetNextCluster(cluster + 1, cluster) == AFATFS_OPERATION_SUCCESS);
    put32(expected + 8 * sizeof(uint32_t), 0x90000000 | cluster);
    assert(!memcmp(mirror, expected, SECTOR));
    assert(!memcmp(afatfs_cacheSectorGetMemory(0), expected, SECTOR));
    expectPairedEntryCopy();

    // A clean older mirror is repaired in full rather than trusted as a pair.
    mirrorDescriptor->state = AFATFS_CACHE_STATE_IN_SYNC;
    afatfs.cacheDirtyEntries--;
    memset(mirror, 0xB6, SECTOR);
    fullImageCopies = 0;
    assert(afatfs_FATSetNextCluster(cluster + 2, 0) == AFATFS_OPERATION_SUCCESS);
    put32(expected + 9 * sizeof(uint32_t), 0xA0000000);
    assert(!memcmp(mirror, expected, SECTOR));
    assert(fullImageCopies == 1);

    // A failed mirror ACK remains dirty and retryable. Entry-sized edits must
    // preserve the complete image through a subsequent successful submission.
    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = afatfs_fatSectorToPhysical(1, 400);
    afatfs_cacheFlushSector((int)(mirrorDescriptor - afatfs.cacheDescriptor));
    completeFixtureIO();
    assert(mirrorDescriptor->state == AFATFS_CACHE_STATE_DIRTY);
    backendFailure.kind = FAIL_NONE;
    fullImageCopies = 0;
    entryImageCopies = 0;
    assert(afatfs_FATSetNextCluster(cluster + 3, cluster + 2) == AFATFS_OPERATION_SUCCESS);
    put32(expected + 10 * sizeof(uint32_t), 0xB0000000 | (cluster + 2));
    expectPairedEntryCopy();
    assert(!memcmp(mirror, expected, SECTOR));
    afatfs_cacheFlushSector(0);
    completeFixtureIO();
    afatfs_cacheFlushSector((int)(mirrorDescriptor - afatfs.cacheDescriptor));
    completeFixtureIO();
    assert(!memcmp(media + primarySector * SECTOR, expected, SECTOR));
    assert(!memcmp(media + afatfs_fatSectorToPhysical(1, 400) * SECTOR, expected, SECTOR));

    // After the primary ACK it may be evicted before the dirty mirror is sent.
    // Reloading that primary must still permit a coherent entry-sized update.
    resetCacheFixture();
    afatfs_cacheSectorInit(&afatfs.cacheDescriptor[0], afatfs_fatSectorToPhysical(1, 400), false);
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_DIRTY;
    afatfs.cacheDirtyEntries = 1;
    memcpy(afatfs_cacheSectorGetMemory(0), expected, SECTOR);
    fullImageCopies = 0;
    entryImageCopies = 0;
    unsigned initialReads = reads;
    for (unsigned retry = 0;; retry++) {
        assert(retry < 10000);
        afatfsOperationStatus_e status = afatfs_FATSetNextCluster(cluster + 4, cluster + 3);
        if (status == AFATFS_OPERATION_SUCCESS) {
            break;
        }
        assert(status == AFATFS_OPERATION_IN_PROGRESS);
        completeFixtureIO();
    }
    put32(expected + 11 * sizeof(uint32_t), 0xC0000000 | (cluster + 3));
    assert(reads == initialReads + 1);
    expectPairedEntryCopy();
    assert(!memcmp(afatfs_cacheSectorGetMemory(0), expected, SECTOR));
    afatfsCacheBlockDescriptor_t *primaryDescriptor = afatfs_findCacheSector(primarySector);
    assert(primaryDescriptor);
    assert(!memcmp(afatfs_cacheSectorGetMemory((int)(primaryDescriptor - afatfs.cacheDescriptor)), expected, SECTOR));

}

/**
 * @brief Exercise dirty-pair collection, access aging, copy size and backend ownership.
 * @param primaryIndex Cache slot containing the primary FAT image.
 * @param mirrorIndex Distinct cache slot containing the mirror FAT image.
 */
static void checkCollectedPair(int primaryIndex, int mirrorIndex)
{
    unsigned initialReads = reads;
    unsigned initialWrites = submittedWrites;
    uint32_t cluster = 400 * AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR + 7;
    uint32_t primarySector = afatfs_fatSectorToPhysical(0, 400);
    uint8_t expected[SECTOR];
    for (unsigned scenario = 0; scenario < 9; scenario++) {
        resetCacheFixture();
        memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));
        afatfsFile_t *file = &afatfs.openFiles[0];
        file->type = AFATFS_FILE_TYPE_NORMAL;
        file->mode = AFATFS_FILE_MODE_WRITE;
        file->writeLockedCacheIndex = -1;
        file->readRetainCacheIndex = -1;
        file->firstCluster = cluster;
        file->physicalSize = SECTOR;
        file->cursorOffset = SECTOR;
        file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
        file->operation.state.appendFreeCluster.collecting = true;
        file->operation.state.appendFreeCluster.searchCluster = cluster;
        file->operation.state.appendFreeCluster.progress.zeroSector = cluster + 1;
        file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
        afatfs.lastClusterAllocated = cluster;
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[primaryIndex], primarySector, false);
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[mirrorIndex], primarySector + afatfs.fatSectors, false);
        afatfs.cacheDescriptor[primaryIndex].state = AFATFS_CACHE_STATE_DIRTY;
        afatfs.cacheDescriptor[mirrorIndex].state = AFATFS_CACHE_STATE_DIRTY;
        afatfs.cacheDescriptor[primaryIndex].writeTimestamp = 11;
        afatfs.cacheDescriptor[mirrorIndex].writeTimestamp = 12;
        afatfs.cacheDescriptor[primaryIndex].retainCount = 2;
        afatfs.cacheDescriptor[mirrorIndex].retainCount = 3;
        afatfs.cacheDirtyEntries = 2;
        memcpy(expected, media + primarySector * SECTOR, SECTOR);
        put32(expected + 8 * sizeof(uint32_t), scenario == 6 ? 0xAFFFFFFF : 0xA0000000);
        memcpy(afatfs_cacheSectorGetMemory(primaryIndex), expected, SECTOR);
        memcpy(afatfs_cacheSectorGetMemory(mirrorIndex), expected, SECTOR);
        if (scenario == 2) {
            // An older clean mirror must receive the complete primary image.
            afatfs.cacheDescriptor[mirrorIndex].state = AFATFS_CACHE_STATE_IN_SYNC;
            afatfs.cacheDirtyEntries--;
            memset(afatfs_cacheSectorGetMemory(mirrorIndex), 0xB6, SECTOR);
        } else if (scenario == 3) {
            afatfs.cacheDescriptor[primaryIndex].state = AFATFS_CACHE_STATE_IN_SYNC;
            afatfs.cacheDirtyEntries--;
        } else if (scenario == 4 || scenario == 5) {
            // A FAT page owned by the backend must remain immutable.
            afatfs.cacheDescriptor[scenario == 4 ? primaryIndex : mirrorIndex].state = AFATFS_CACHE_STATE_WRITING;
            afatfs.cacheDirtyEntries--;
            afatfs.cacheFlushInProgress = true;
        } else if (scenario == 7) {
            afatfsFile_t *peer = &afatfs.openFiles[1];
            peer->type = AFATFS_FILE_TYPE_NORMAL;
            peer->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
            peer->operation.state.appendFreeCluster.searchCluster = cluster + 1;
            peer->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
        } else if (scenario == 8) {
            // Unrelated in-flight data does not own either dirty FAT image.
            afatfs_cacheSectorInit(&afatfs.cacheDescriptor[2], afatfs.clusterStartSector + 100, false);
            afatfs.cacheDescriptor[2].state = AFATFS_CACHE_STATE_WRITING;
            memset(afatfs_cacheSectorGetMemory(2), 0x65, SECTOR);
            afatfs.cacheFlushInProgress = true;
        }
        uint32_t initialTimer = scenario == 1 ? UINT32_MAX - 1 : 100;
        afatfs.cacheTimer = initialTimer;
        afatfs.fsInfoDirty = false;
        fullImageCopies = 0;
        entryImageCopies = 0;
        bool result = afatfs_collectNext(file);
        if (scenario >= 4 && scenario <= 7) {
            assert(!result);
            assert(file->physicalSize == SECTOR);
            assert(file->operation.state.appendFreeCluster.progress.zeroSector == cluster + 1);
            assert(afatfs.cacheTimer == initialTimer + 1);
            assert(!fullImageCopies && !entryImageCopies && !afatfs.fsInfoDirty);
        } else {
            assert(result);
            put32(expected + 8 * sizeof(uint32_t), 0xAFFFFFFF);
            assert(file->physicalSize == 2 * SECTOR && file->cursorCluster == cluster + 1);
            assert(file->operation.state.appendFreeCluster.progress.zeroSector == cluster + 2);
            assert(afatfs.fsInfoDirty && afatfs.cacheDirtyEntries == 2);
            assert(afatfs.cacheTimer == initialTimer + (scenario == 2 || scenario == 3 ? 4 : 3));
            assert(afatfs.cacheDescriptor[primaryIndex].accessTimestamp == initialTimer + 2);
            assert(afatfs.cacheDescriptor[mirrorIndex].accessTimestamp == initialTimer + 3);
            if (scenario == 2 || scenario == 3) {
                assert(fullImageCopies == 1 && entryImageCopies == 0);
            } else {
                assert(fullImageCopies == 0 && entryImageCopies == 1);
                assert(afatfs.cacheDescriptor[primaryIndex].writeTimestamp == 11);
                assert(afatfs.cacheDescriptor[mirrorIndex].writeTimestamp == 12);
            }
        }
        assert(!memcmp(afatfs_cacheSectorGetMemory(primaryIndex), expected, SECTOR));
        assert(!memcmp(afatfs_cacheSectorGetMemory(mirrorIndex), expected, SECTOR));
        assert(afatfs.cacheDescriptor[primaryIndex].retainCount == 2 && afatfs.cacheDescriptor[mirrorIndex].retainCount == 3);
        assert(reads == initialReads && submittedWrites == initialWrites && !io.pending);
        if (scenario == 8) {
            for (unsigned offset = 0; offset < SECTOR; offset++) {
                assert(afatfs_cacheSectorGetMemory(2)[offset] == 0x65);
            }
        }
    }
    memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));
}

/** @brief Keep FAT range rejection correct at normal, wrapped and large LBA boundaries. */
static void checkCollectPageRange(void)
{
    LBA_t savedStart = afatfs.fatStartSector;
    LBA_t savedLength = afatfs.fatSectors;
    const LBA_t starts[] = {32, UINT32_MAX - 3, 12, 12};
    const LBA_t lengths[] = {100, 16, UINT32_MAX / 2 + 3, UINT32_MAX};
    for (unsigned index = 0; index < 2; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
        file->type = AFATFS_FILE_TYPE_NORMAL;
        file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
        file->operation.state.appendFreeCluster.collecting = true;
        file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
        file->operation.state.appendFreeCluster.searchCluster = (1 + 2 * index) * 128 + 7;
    }
    for (unsigned scenario = 0; scenario < sizeof(starts) / sizeof(starts[0]); scenario++) {
        afatfs.fatStartSector = starts[scenario];
        afatfs.fatSectors = lengths[scenario];
        for (unsigned copy = 0; copy < 2; copy++) {
            assert(afatfs_collectFatPage(afatfs_fatSectorToPhysical(copy, 1)));
            assert(afatfs_collectFatPage(afatfs_fatSectorToPhysical(copy, 3)));
            assert(!afatfs_collectFatPage(afatfs_fatSectorToPhysical(copy, 5)));
        }
        assert(!afatfs_collectFatPage(afatfs.fatStartSector - 1));
        assert(!afatfs_collectFatPage(afatfs_fatSectorToPhysical(1, afatfs.fatSectors)));
    }
    afatfs.fatStartSector = savedStart;
    afatfs.fatSectors = savedLength;
    memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));
}

/** @brief Preserve every idle tick, backend wait and cohort sealing boundary. */
static void checkCollectionIdlePoll(void)
{
    resetCacheFixture();
    memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));
    afatfsFile_t *file = &afatfs.openFiles[0];
    afatfsFile_t *peer = &afatfs.openFiles[1];
    file->type = AFATFS_FILE_TYPE_NORMAL;
    file->writeLockedCacheIndex = -1;
    file->readRetainCacheIndex = -1;
    file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster, 0);
    assert(file->operation.state.appendFreeCluster.collectIdle == 0);
    assert(!file->operation.state.appendFreeCluster.wrapped && !file->operation.state.appendFreeCluster.collecting);
    file->operation.state.appendFreeCluster.wrapped = true;
    file->operation.state.appendFreeCluster.collecting = true;
    file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
    unsigned initialReads = reads;
    unsigned initialWrites = submittedWrites;
    uint32_t initialTimer = afatfs.cacheTimer;
    for (unsigned idle = 0; idle < 63; idle++) {
        file->operation.state.appendFreeCluster.collectIdle = (uint8_t)idle;
        afatfs_fileOperationsPoll();
        assert(file->operation.state.appendFreeCluster.collectIdle == idle + 1);
        assert(file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT);
        assert(file->operation.state.appendFreeCluster.wrapped && file->operation.state.appendFreeCluster.collecting);
    }
    afatfs.cacheFlushInProgress = true;
    for (unsigned idle = 0; idle <= 63; idle++) {
        file->operation.state.appendFreeCluster.collectIdle = (uint8_t)idle;
        afatfs_fileOperationsPoll();
        assert(file->operation.state.appendFreeCluster.collectIdle == idle);
        assert(file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT);
    }
    afatfs.cacheFlushInProgress = false;
    file->operation.state.appendFreeCluster.collectIdle = 12;
    file->type = AFATFS_FILE_TYPE_NONE;
    afatfs_fileOperationsPoll();
    assert(file->operation.state.appendFreeCluster.collectIdle == 12);
    file->type = AFATFS_FILE_TYPE_NORMAL;
    file->operation.active = AFATFS_FILE_OPERATION_NONE;
    afatfs_fileOperationsPoll();
    assert(file->operation.state.appendFreeCluster.collectIdle == 12);
    assert(afatfs.cacheTimer == initialTimer);
    assert(reads == initialReads && submittedWrites == initialWrites && !io.pending);

    // Two consecutive passes must seal at the same tick and wait for data ACK.
    file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    file->operation.state.appendFreeCluster.collectIdle = 62;
    peer->type = AFATFS_FILE_TYPE_NORMAL;
    peer->writeLockedCacheIndex = -1;
    peer->readRetainCacheIndex = -1;
    peer->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    peer->operation.state.appendFreeCluster.collecting = true;
    peer->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
    peer->operation.state.appendFreeCluster.collectIdle = 7;
    afatfs_fileOperationsPoll();
    assert(file->operation.state.appendFreeCluster.collectIdle == 63);
    assert(peer->operation.state.appendFreeCluster.collectIdle == 8);
    uint32_t dataSector = afatfs.clusterStartSector + 100;
    afatfs_cacheSectorInit(&afatfs.cacheDescriptor[0], dataSector, false);
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_DIRTY;
    afatfs.cacheDirtyEntries = 1;
    backendFailure.kind = FAIL_WRITE_SUBMIT;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = dataSector;
    backendFailure.hits = 0;
    afatfs_fileOperationsPoll();
    assert(file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE);
    assert(peer->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE);
    assert(file->operation.state.appendFreeCluster.collectIdle == 63);
    assert(peer->operation.state.appendFreeCluster.collectIdle == 8);
    assert(backendFailure.hits == 2 && !io.pending);
    assert(afatfs.cacheDescriptor[0].state == AFATFS_CACHE_STATE_DIRTY);
    assert(reads == initialReads && submittedWrites == initialWrites);
    backendFailure.kind = FAIL_NONE;
    memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    uint32_t primarySector = afatfs_fatSectorToPhysical(0, 400);
    uint8_t *buffer;
    unsigned initialReads = reads;
    for (unsigned index = 0; index < SECTOR / 4; index++) {
        uint32_t value = (uint32_t)(1 + index % 15) << 28;
        put32(media + primarySector * SECTOR + index * 4, value);
        put32(media + afatfs_fatSectorToPhysical(1, 400) * SECTOR + index * 4, value);
    }

    // A second page is available; the original mirror remains independently cached.
    installMirrorFixture();
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_SUCCESS);
    assert(buffer != afatfs_cacheSectorGetMemory(0));
    assert(!memcmp(buffer, afatfs_cacheSectorGetMemory(0), SECTOR));
    assert(afatfs.cacheDescriptor[0].sectorIndex == afatfs_fatSectorToPhysical(1, 400));
    assert(reads == initialReads && !io.pending);

    // Only the mirror can be reused. Its buffer survives descriptor reinitialization.
    installMirrorFixture();
    for (unsigned index = 1; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[index], afatfs.clusterStartSector + 100 + index, true);
        afatfs.cacheDescriptor[index].state = AFATFS_CACHE_STATE_IN_SYNC;
    }
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_SUCCESS);
    assert(buffer == afatfs_cacheSectorGetMemory(0));
    assert(afatfs.cacheDescriptor[0].sectorIndex == primarySector);
    assert(!memcmp(buffer, media + primarySector * SECTOR, SECTOR));
    assert(reads == initialReads && !io.pending);

    // A newer cached primary always wins over an older clean mirror.
    installMirrorFixture();
    afatfs_cacheSectorInit(&afatfs.cacheDescriptor[1], primarySector, false);
    afatfs.cacheDescriptor[1].state = AFATFS_CACHE_STATE_DIRTY;
    afatfs.cacheDirtyEntries = 1;
    memset(afatfs_cacheSectorGetMemory(1), 0xB0, SECTOR);
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_SUCCESS);
    assert(buffer == afatfs_cacheSectorGetMemory(1) && buffer[0] == 0xB0);

    // Metadata acquisition with retention must also preserve the newer primary.
    assert(afatfs_cacheSector(primarySector, &buffer,
               AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN | AFATFS_CACHE_METADATA, 0) == AFATFS_OPERATION_SUCCESS);
    assert(buffer == afatfs_cacheSectorGetMemory(1) && buffer[0] == 0xB0);
    assert(afatfs.cacheDescriptor[1].retainCount == 1);
    assert(afatfs.cacheDescriptor[1].state == AFATFS_CACHE_STATE_DIRTY);
    assert(reads == initialReads && !io.pending);

    // Dirty and locked mirrors cannot provide a confirmed cache image.
    installMirrorFixture();
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_DIRTY;
    afatfs.cacheDirtyEntries = 1;
    memset(afatfs_cacheSectorGetMemory(0), 0xA0, SECTOR);
    expectDiskRead(primarySector);
    installMirrorFixture();
    afatfs.cacheDescriptor[0].locked = true;
    expectDiskRead(primarySector);

    // While the mirror write is in flight, a primary miss must still wait for I/O.
    installMirrorFixture();
    afatfs_cacheSectorMarkDirty(&afatfs.cacheDescriptor[0]);
    afatfs_cacheFlushSector(0);
    assert(io.pending && io.write);
    assert(afatfs_cacheSector(primarySector, &buffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_IN_PROGRESS);
    completeFixtureIO();

    // A failed ACK leaves the mirror dirty; its unconfirmed image is not reused.
    installMirrorFixture();
    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = afatfs_fatSectorToPhysical(1, 400);
    afatfs_cacheSectorMarkDirty(&afatfs.cacheDescriptor[0]);
    afatfs_cacheFlushSector(0);
    completeFixtureIO();
    assert(afatfs.cacheDescriptor[0].state == AFATFS_CACHE_STATE_DIRTY);
    backendFailure.kind = FAIL_NONE;
    expectDiskRead(primarySector);

    checkPairedEntryUpdates(primarySector);
    checkCollectedPair(0, 1);
    checkCollectedPair(1, 0);
    checkCollectedPair(6, 7);
    checkCollectPageRange();
    checkCollectionIdlePoll();
    resetCacheFixture();
    unmount();
    puts(
        "FAT mirror cache: recovery, selected paired copies, clean repair and failed ACK retry passed.");
    return 0;
}
