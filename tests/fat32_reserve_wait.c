// Waiting for a reserved EOC must drain both FATs without sending the old directory.
#define main reserve_wait_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

/** @brief Drain all pending cache writes before constructing the fault fixture. */
static void drainReserveFixture(void)
{
    for (unsigned retry = 0; !afatfs_flushCache(); retry++) {
        assert(retry < 10000);
        pump();
    }
}

/** @brief Verify reservation barriers and directory exclusion under one FAT failure. */
static void checkReserveWait(const char *name, bool failMirror, backendFailure_e failure)
{
    afatfsFile_t *file = openFile(name, "w");
    uint8_t payload[SECTOR + 1];
    memset(payload, 0x59, sizeof(payload));
    writeBytes(file, payload, SECTOR);
    afatfs_fileUnlockCacheSector(file);
    completed = false;
    assert(afatfs_fsync(file, fileDone, &callbackContext));
    waitDone();
    drainReserveFixture();

    uint32_t reserved = (afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK) + 256;
    uint32_t primarySector = afatfs_fatSectorToPhysical(0, reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    assert(!get32(media + primarySector * SECTOR + reserved % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4));
    while (afatfs_FATSetNextCluster(reserved, 0xFFFFFFFF) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    uint8_t *directoryBuffer;
    while (afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &directoryBuffer,
               AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    afatfsCacheBlockDescriptor_t *directoryEntry = afatfs_getCacheDescriptorForBuffer(directoryBuffer);
    directoryEntry->writeTimestamp = 0;
    uint8_t savedDirectory[SECTOR];
    memcpy(savedDirectory, media + file->directoryEntryPos.sectorNumberPhysical * SECTOR, SECTOR);
    // Make the directory the oldest eligible page so an unfiltered flush fails this test.
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[index];
        if (descriptor != directoryEntry && descriptor->state == AFATFS_CACHE_STATE_DIRTY) {
            descriptor->writeTimestamp = 1;
        }
    }
    afatfs.lastClusterAllocated = reserved | AFATFS_RESERVED_CLUSTER;
    file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster, file->firstCluster);
    backendFailure.kind = failure;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = afatfs_fatSectorToPhysical(failMirror ? 1 : 0,
        reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    backendFailure.hits = 0;
    for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
        assert(retry < 10000);
        assert(afatfs_appendRegularFreeClusterContinue(file) == AFATFS_OPERATION_IN_PROGRESS);
        assert(file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE);
        assert(afatfs.lastClusterAllocated == (reserved | AFATFS_RESERVED_CLUSTER));
        assert(directoryEntry->state == AFATFS_CACHE_STATE_DIRTY);
        assert(!io.pending || io.block != file->directoryEntryPos.sectorNumberPhysical);
        sdcard_poll();
        assert(!memcmp(savedDirectory, media + file->directoryEntryPos.sectorNumberPhysical * SECTOR, SECTOR));
        assert(!afatfs_canPowerOff());
    }
    backendFailure.kind = FAIL_NONE;
    for (unsigned retry = 0; afatfs_fileIsBusy(file); retry++) {
        assert(retry < 10000);
        pump();
    }
    writeBytes(file, payload + SECTOR, 1);
    closeFile(file);
}

/** @brief Exercise submission/ACK failures on each FAT and verify remounted payloads. */
int main(void)
{
    exerciseFat(true, 1);
    mount();
    checkReserveWait("WAITPRI.BIN", false, FAIL_WRITE_SUBMIT);
    checkReserveWait("WAITMIR.BIN", true, FAIL_WRITE_SUBMIT);
    checkReserveWait("ACKPRI.BIN", false, FAIL_WRITE_CALLBACK);
    checkReserveWait("ACKMIR.BIN", true, FAIL_WRITE_CALLBACK);
    unmount();
    mount();
    const char *names[] = {"WAITPRI.BIN", "WAITMIR.BIN", "ACKPRI.BIN", "ACKMIR.BIN"};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        uint8_t payload[SECTOR + 1];
        assert(file->logicalSize == sizeof(payload));
        readBytes(file, payload, sizeof(payload));
        for (unsigned offset = 0; offset < sizeof(payload); offset++) {
            assert(payload[offset] == 0x59);
        }
        closeFile(file);
    }
    unmount();
    puts("Reserve wait: directory excluded, both FAT submission/ACK retries and remount passed.");
    return 0;
}
