// Shared EOC/link barriers must not publish either writer before both FAT ACKs.
#define main pending_allocation_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

static void drainPendingCache(void)
{
    for (unsigned retry = 0; !afatfs_flushCache(); retry++) {
        assert(retry < 10000);
        pump();
    }
}

static void preparePendingAllocation(afatfsFile_t *file, uint32_t cluster)
{
    assert(file->operation.active == AFATFS_FILE_OPERATION_NONE);
    assert(file->logicalSize == SECTOR);
    file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster, file->firstCluster);
    file->operation.state.appendFreeCluster.searchCluster = cluster;
    file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
    file->cursorCluster = cluster;
    file->cursorPreviousCluster = file->firstCluster;
    file->physicalSize += SECTOR;
}

static void checkPendingPair(bool failMirror)
{
    afatfsFile_t *first = openFile(failMirror ? "MIRONE.BIN" : "PRI_ONE.BIN", "w");
    afatfsFile_t *second = openFile(failMirror ? "MIRTWO.BIN" : "PRI_TWO.BIN", "w");
    uint8_t payload[SECTOR + 1];
    memset(payload, 0x76, sizeof(payload));
    writeBytes(first, payload, SECTOR);
    writeBytes(second, payload, SECTOR);
    afatfs_fileUnlockCacheSector(first);
    afatfs_fileUnlockCacheSector(second);
    assert(afatfs_releaseReservedCluster() == AFATFS_OPERATION_SUCCESS);
    drainPendingCache();

    uint32_t cluster = failMirror ? 6010 : 6000;
    while (afatfs_FATSetNextCluster(cluster, 0xFFFFFFFF) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    while (afatfs_FATSetNextCluster(cluster + 1, 0xFFFFFFFF) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    drainPendingCache();
    preparePendingAllocation(first, cluster);
    preparePendingAllocation(second, cluster + 1);
    afatfs_groupDurableAllocations();
    assert(first->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2);
    assert(second->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2);
    assert(afatfs_appendRegularFreeClusterContinue(first) == AFATFS_OPERATION_IN_PROGRESS);
    assert(afatfs_appendRegularFreeClusterContinue(second) == AFATFS_OPERATION_IN_PROGRESS);
    assert(first->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE);
    assert(second->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE);

    uint32_t sector =
        afatfs_fatSectorToPhysical(failMirror ? 1 : 0, first->firstCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = sector;
    backendFailure.hits = 0;
    for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
        assert(retry < 10000);
        pump();
        assert(first->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE);
        assert(second->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE);
        assert(!afatfs_isIdle());
        const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)(media +
            first->directoryEntryPos.sectorNumberPhysical * SECTOR + first->directoryEntryPos.entryIndex * 32);
        assert(entry->fileSize <= SECTOR);
    }
    backendFailure.kind = FAIL_NONE;
    for (unsigned retry = 0; afatfs_fileIsBusy(first) || afatfs_fileIsBusy(second); retry++) {
        assert(retry < 10000);
        pump();
    }
    writeBytes(first, payload + SECTOR, 1);
    writeBytes(second, payload + SECTOR, 1);
    closeFile(first);
    closeFile(second);
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    checkPendingPair(false);
    checkPendingPair(true);
    unmount();
    mount();
    const char *names[] = {"PRI_ONE.BIN", "PRI_TWO.BIN", "MIRONE.BIN", "MIRTWO.BIN"};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        uint8_t payload[SECTOR + 1];
        assert(file->logicalSize == sizeof(payload));
        readBytes(file, payload, sizeof(payload));
        for (unsigned offset = 0; offset < sizeof(payload); offset++) {
            assert(payload[offset] == 0x76);
        }
        closeFile(file);
    }
    unmount();
    puts("Pending allocation grouping: both writers held under primary/mirror ACK failures; remount payload passed.");
    return 0;
}
