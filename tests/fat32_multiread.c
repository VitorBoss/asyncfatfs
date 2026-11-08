#define main multiread_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

static uint16_t checkedReadExtent(LBA_t physicalSector)
{
    uint16_t limit = afatfs_readLimit(physicalSector);
    if (limit < AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) {
        return limit;
    }
    return afatfs_readExtent(physicalSector, limit);
}

static void checkReadSessions(void)
{
    uint8_t payload[SECTOR * 8];
    memset(payload, 0x59, sizeof(payload));
    afatfsFile_t *file = openFile("MULTI.BIN", "w");
    writeBytes(file, payload, sizeof(payload));
    closeFile(file);
    unmount();
    mount();
    file = openFile("MULTI.BIN", "r");
    uint32_t next;
    while (afatfs_FATGetNextCluster(0, file->cursorCluster, &next) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    uint32_t physical = afatfs_fileGetCursorPhysicalSector(file);
    assert(checkedReadExtent(physical) >= 2);
    uint32_t originalSize = file->logicalSize;
    file->logicalSize = SECTOR + 1;
    assert(checkedReadExtent(physical) == 2);
    file->logicalSize = originalSize;

    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        if (afatfs.cacheDescriptor[index].sectorIndex ==
                afatfs.fatStartSector + file->cursorCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR &&
            afatfs.cacheDescriptor[index].state == AFATFS_CACHE_STATE_IN_SYNC) {
            uint32_t *table = (uint32_t *)afatfs_cacheSectorGetMemory(index);
            uint32_t offset = file->cursorCluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
            uint32_t saved = table[offset];
            table[offset] = 0x0FFFFFFF;
            assert(checkedReadExtent(physical) == 1);
            table[offset] = file->cursorCluster + 2;
            assert(checkedReadExtent(physical) == 1);
            table[offset] = saved | 0xF0000000U;
            assert(checkedReadExtent(physical) >= 2);
            table[offset] = saved;
            uint32_t savedCluster = file->cursorCluster;
            file->cursorCluster =
                savedCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR +
                AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR - 1;
            assert(checkedReadExtent(afatfs_fileGetCursorPhysicalSector(file)) == 1);
            file->cursorCluster = savedCluster;
        }
    }
    unsigned writesBefore = writes;
    readBatch.beginFailures = 3;
    unsigned sessionsBefore = readBatch.sessions;
    for (unsigned retry = 0; retry < 3; retry++) {
        afatfs_fprefetch(file);
        assert(!io.pending && readBatch.sessions == sessionsBefore);
        assert(afatfs.lastError == AFATFS_ERROR_BACKEND_IO);
    }
    holdIO = true;
    afatfs_fprefetch(file);
    assert(io.pending && !io.write && readBatch.active);
    assert(!sdcard_isReadBlockInSequence(physical + 1));
    assert(file->cursorOffset == 0);
    assert(!afatfs_isIdle() && !afatfs_canPowerOff());
    holdIO = false;
    while (io.pending) {
        pump();
    }
    assert(readBatch.active);
    assert(sdcard_isReadBlockInSequence(physical + 1));
    assert(afatfs_isIdle());
    assert(!readBatch.active); // An idle answer must not leave a card read session open.
    backendFailure.kind = FAIL_READ_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = physical + 1;
    backendFailure.hits = 0;
    readBytes(file, payload, SECTOR);
    for (unsigned retry = 0; !backendFailure.hits; retry++) {
        assert(retry < 10000);
        pump();
    }
    assert(!readBatch.active);
    assert(!sdcard_isReadBlockInSequence(physical + 1));
    backendFailure.kind = FAIL_NONE;
    readBytes(file, payload + SECTOR, SECTOR);
    for (unsigned index = 0; index < SECTOR * 2; index++) {
        assert(payload[index] == 0x59);
    }
    while (io.pending) {
        pump();
    }
    assert(readBatch.active);
    readBatch.stopFailures = 3;
    readBatch.stopBusy = 3;
    completed = false;
    assert(afatfs_fclose(file, done, &callbackContext));
    for (unsigned retry = 0; readBatch.stopFailures || readBatch.stopBusy; retry++) {
        assert(retry < 10000 && !completed && !afatfs_canPowerOff());
        pump();
    }
    waitDone();
    assert(!readBatch.active && writes == writesBefore);

    file = openFile("MULTI.BIN", "r");
    afatfsFile_t *other = openFile("MULTI.BIN", "r");
    sessionsBefore = readBatch.sessions;
    assert(checkedReadExtent(afatfs_fileGetCursorPhysicalSector(file)) == 1);
    readBytes(file, payload, sizeof(payload));
    readBytes(other, payload, sizeof(payload));
    assert(readBatch.sessions == sessionsBefore);
    closeFile(file);
    closeFile(other);
    unmount();
    mount();
    file = openFile("MULTI.BIN", "r");
    while (afatfs_FATGetNextCluster(0, file->cursorCluster, &next) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    holdIO = true;
    afatfs_fprefetch(file);
    assert(io.pending && readBatch.active);
    retiredCompletion_t retired = removeTestCard(0, file);
    assert(!readBatch.active);
    holdIO = false;
    mountReinsertedCard(&retired);
    file = openFile("MULTI.BIN", "r");
    readBytes(file, payload, sizeof(payload));
    assert(!readBatch.active);
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == 0x59);
    }
    closeFile(file);
    unmount();
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    checkReadSessions();
    puts(
        "Multi-read: cached extents, EOF/fragmentation, begin/read/stop failures, two-reader fallback, no writes, removal/stale callbacks and remount passed.");
    return 0;
}
