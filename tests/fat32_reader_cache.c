// Released reader data should be reused before another reader's future data.
#define main reader_cache_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

static void installReaderFixture(afatfsFile_t *file, uint32_t physical)
{
    assert(!io.pending);
    memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
    afatfs.cacheDirtyEntries = 0;
    afatfs.cacheFlushInProgress = false;
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[index], physical + index, false);
        afatfs.cacheDescriptor[index].state = AFATFS_CACHE_STATE_IN_SYNC;
    }
    memcpy(afatfs_cacheSectorGetMemory(0), media + physical * SECTOR, SECTOR);
    afatfs.cacheDescriptor[0].retainCount = 1;
    afatfs.cacheDescriptor[0].accessTimestamp = 100;
    afatfs.cacheDescriptor[1].accessTimestamp = 1; // Older, but not consumed yet.
    afatfs.cacheDescriptor[2].locked = true;
    afatfs.cacheDescriptor[3].retainCount = 1;
    file->readRetainCacheIndex = 0;
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    uint8_t payload[SECTOR + 1];
    for (unsigned index = 0; index < sizeof(payload); index++) {
        payload[index] = (uint8_t)(index * 13);
    }
    afatfsFile_t *file = openFile("READER.BIN", "w");
    assert(file);
    writeBytes(file, payload, sizeof(payload));
    closeFile(file);
    unmount();
    mount();
#if ENABLE_AFATFS_DMA_CACHE
    assert(afatfs.cache == afatfs_cache);
    assert((uintptr_t)afatfs.cache % 4 == 0);
#endif
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        uint8_t *sectorStart = afatfs_cacheSectorGetMemory((int)index);
        assert(afatfs_getCacheDescriptorIndexForBuffer(sectorStart) == (int)index);
        assert(afatfs_getCacheDescriptorIndexForBuffer(sectorStart + SECTOR - 1) == (int)index);
    }
#ifndef AFATFS_DEBUG
    afatfsFilesystemState_e savedState = afatfs.fileSystemState;
    afatfsError_e savedError = afatfs.lastError;
    assert(afatfs_getCacheDescriptorIndexForBuffer(afatfs.cache + AFATFS_NUM_CACHE_SECTORS * SECTOR) == -1);
    assert(afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_FATAL);
    afatfs.fileSystemState = savedState;
    afatfs.lastError = savedError;
#endif
    file = openFile("READER.BIN", "r");
    assert(file);
    uint32_t savedClusterSectors = afatfs.sectorsPerCluster;
    uint32_t savedClusterMask = afatfs.byteInClusterMask;
    uint32_t savedCursorCluster = file->cursorCluster;
    uint32_t savedCursorOffset = file->cursorOffset;
    afatfsFileType_e savedType = file->type;
    afatfs.sectorsPerCluster = 1;
    afatfs.byteInClusterMask = 511;
    file->cursorCluster = 5;
    file->cursorOffset = 511;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.clusterStartSector + 3);
    file->cursorOffset = 512;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.clusterStartSector + 3);
    afatfs.sectorsPerCluster = 8;
    afatfs.byteInClusterMask = 4095;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.clusterStartSector + 25);
    file->cursorOffset = 4095;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.clusterStartSector + 31);
    file->cursorOffset = 4096;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.clusterStartSector + 24);
    afatfs.sectorsPerCluster = 1;
    file->type = AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY;
    file->cursorCluster = 0;
    file->cursorOffset = 512;
    assert(afatfs_fileGetCursorPhysicalSector(file) == afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + 1);
    file->type = savedType;
    file->cursorOffset = savedCursorOffset;
    file->cursorCluster = savedCursorCluster;
    afatfs.byteInClusterMask = savedClusterMask;
    afatfs.sectorsPerCluster = savedClusterSectors;
    uint32_t physical = afatfs_fileGetCursorPhysicalSector(file);
    installReaderFixture(file, physical);
    afatfs_fileUnlockCacheSector(file);
    assert(afatfs.cacheDescriptor[0].discardable && !afatfs.cacheDescriptor[0].retainCount);
    assert(afatfs_allocateCacheSector(physical + 20, NULL) == 0);
    assert(afatfs.cacheDescriptor[1].sectorIndex == physical + 1);

    installReaderFixture(file, physical);
    afatfs_fileUnlockCacheSector(file);
    assert(afatfs_fileRetainCursorSectorForRead(file) == afatfs_cacheSectorGetMemory(0));
    assert(!afatfs.cacheDescriptor[0].discardable && afatfs.cacheDescriptor[0].retainCount == 1);
    assert(afatfs_allocateCacheSector(physical + 20, NULL) == 1); // Retention still wins over replacement.

    installReaderFixture(file, physical);
    afatfs_fileUnlockCacheSector(file);
    afatfs.cacheDescriptor[0].retainCount = 1;
    assert(afatfs_allocateCacheSector(physical + 20, NULL) == 1);
    installReaderFixture(file, physical);
    afatfs_fileUnlockCacheSector(file);
    afatfs.cacheDescriptor[0].locked = true;
    assert(afatfs_allocateCacheSector(physical + 20, NULL) == 1);

    const uint8_t protectedStates[] = {
        AFATFS_CACHE_STATE_DIRTY, AFATFS_CACHE_STATE_WRITING, AFATFS_CACHE_STATE_READING};
    for (unsigned index = 0; index < sizeof(protectedStates) / sizeof(protectedStates[0]); index++) {
        installReaderFixture(file, physical);
        afatfs_fileUnlockCacheSector(file);
        afatfs.cacheDescriptor[0].state = protectedStates[index];
        assert(afatfs_allocateCacheSector(physical + 20, NULL) == 1);
        assert(afatfs.cacheDescriptor[0].state == protectedStates[index]);
    }
    const uint8_t readableStates[] = {
        AFATFS_CACHE_STATE_IN_SYNC, AFATFS_CACHE_STATE_DIRTY, AFATFS_CACHE_STATE_WRITING};
    for (unsigned index = 0; index < sizeof(readableStates) / sizeof(readableStates[0]); index++) {
        installReaderFixture(file, physical);
        afatfs.cacheDescriptor[0].state = readableStates[index];
        afatfs.cacheDescriptor[0].locked = true;
        afatfs.cacheDescriptor[0].discardable = true;
        uint32_t previousTimer = afatfs.cacheTimer;
        unsigned previousReads = reads;
        uint8_t *sectorBuffer = NULL;
        assert(afatfs_cacheSector(physical, &sectorBuffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_SUCCESS);
        assert(sectorBuffer == afatfs_cacheSectorGetMemory(0));
        assert(afatfs.cacheDescriptor[0].state == readableStates[index]);
        assert(afatfs.cacheDescriptor[0].locked && afatfs.cacheDescriptor[0].discardable);
        assert(afatfs.cacheDescriptor[0].retainCount == 1);
        assert(afatfs.cacheTimer == previousTimer + 1);
        assert(afatfs.cacheDescriptor[0].accessTimestamp == afatfs.cacheTimer);
        assert(!afatfs.cacheDirtyEntries && !afatfs.cacheFlushInProgress && reads == previousReads);
        assert(afatfs_cacheSector(physical, &sectorBuffer, AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN, 0) ==
            AFATFS_OPERATION_SUCCESS);
        assert(sectorBuffer == afatfs_cacheSectorGetMemory(0));
        assert(afatfs.cacheDescriptor[0].state == readableStates[index]);
        assert(afatfs.cacheDescriptor[0].retainCount == 2);
        assert(afatfs.cacheDescriptor[0].locked && afatfs.cacheDescriptor[0].discardable);
        assert(afatfs.cacheTimer == previousTimer + 2);
        assert(afatfs.cacheDescriptor[0].accessTimestamp == afatfs.cacheTimer);
        assert(!afatfs.cacheDirtyEntries && !afatfs.cacheFlushInProgress && reads == previousReads);
    }
    installReaderFixture(file, physical);
    afatfs.cacheDescriptor[0].state = AFATFS_CACHE_STATE_READING;
    uint32_t pendingTimer = afatfs.cacheTimer;
    unsigned pendingReads = reads;
    uint8_t *pendingBuffer = NULL;
    assert(afatfs_cacheSector(physical, &pendingBuffer, AFATFS_CACHE_READ, 0) == AFATFS_OPERATION_IN_PROGRESS);
    assert(afatfs_cacheSector(physical, &pendingBuffer, AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN, 0) ==
        AFATFS_OPERATION_IN_PROGRESS);
    assert(afatfs.cacheDescriptor[0].retainCount == 1);
    assert(!pendingBuffer && !io.pending && reads == pendingReads);
    assert(afatfs.cacheTimer == pendingTimer + 2);
    assert(afatfs.cacheDescriptor[0].accessTimestamp == afatfs.cacheTimer);
    installReaderFixture(file, physical);
    file->mode |= AFATFS_FILE_MODE_WRITE;
    afatfs_fileUnlockCacheSector(file);
    assert(!afatfs.cacheDescriptor[0].discardable);
    file->mode &= (uint8_t)~AFATFS_FILE_MODE_WRITE;
    // Discard synthetic descriptor states before any real IO is allowed again.
    file->readRetainCacheIndex = -1;
    memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
    readBytes(file, payload, 257);
    assert(file->cursorOffset == 257 && !afatfs_fileIsBusy(file));
    int retainedIndex = file->readRetainCacheIndex;
    assert(retainedIndex >= 0);
    unsigned readsBefore = reads;
    assert(afatfs_fread(file, payload + 257, 254) == 254);
    assert(file->cursorOffset == 511 && !afatfs_fileIsBusy(file));
    assert(file->readRetainCacheIndex == retainedIndex);
    assert(afatfs.cacheDescriptor[retainedIndex].retainCount == 1);
    assert(reads == readsBefore && file->error == AFATFS_ERROR_NONE);
    assert(afatfs_fseek(file, 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_SUCCESS);
    readBytes(file, payload, sizeof(payload));
    assert(afatfs_fseek(file, 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_SUCCESS);
    readBytes(file, payload, sizeof(payload));
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == (uint8_t)(index * 13));
    }
    closeFile(file);
    unmount();
    afatfs_notifyMediaRemoved();
#if ENABLE_AFATFS_DMA_CACHE
    assert(afatfs.cache == afatfs_cache);
#endif
    mount();
    unmount();
    puts(
        "Reader cache: consumed/active priority, retain/lock/dirty/inflight protection, writer exclusion, rewind payload and DMA reset passed.");
    return 0;
}
