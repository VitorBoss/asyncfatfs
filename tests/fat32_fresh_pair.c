// Diagnostic: merge already requested EOCs into an entirely dirty pair, retaining one reserve.
#define main fresh_pair_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

/** @brief Drain all eligible writes without advancing file operations. */
static void drainFreshFixture(void)
{
    for (unsigned retry = 0; !afatfs_flushCache(); retry++) {
        assert(retry < 10000);
        pump();
    }
}

/** @brief Verify reserve uniqueness, EOC-before-link and ACK barriers for a pending triple. */
static void checkFreshPair(bool failMirror)
{
    const char *names[3] = {"PRIONE.BIN", "PRITWO.BIN", "PRITHREE.BIN"};
    if (failMirror) {
        names[0] = "MIRONE.BIN";
        names[1] = "MIRTWO.BIN";
        names[2] = "MIRTHREE.BIN";
    }
    afatfsFile_t *files[3];
    uint8_t payload[SECTOR + 1];
    memset(payload, 0x6B, sizeof(payload));
    afatfs.lastClusterAllocated = 6148 + (failMirror ? 128 : 0);
    for (unsigned index = 0; index < 3; index++) {
        files[index] = openFile(names[index], "w");
        writeBytes(files[index], payload, SECTOR);
        afatfs_fileUnlockCacheSector(files[index]);
    }
    assert(afatfs_releaseReservedCluster() == AFATFS_OPERATION_SUCCESS);
    drainFreshFixture();
    uint32_t cluster = 6160 + (failMirror ? 128 : 0);
    while (afatfs_FATSetNextCluster(cluster, 0xFFFFFFFF) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    drainFreshFixture();
    for (unsigned index = 0; index < 3; index++) {
        afatfsFile_t *file = files[index];
        file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
        afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster, file->firstCluster);
        file->operation.state.appendFreeCluster.searchCluster = cluster + 1;
    }
    files[0]->operation.state.appendFreeCluster.searchCluster = cluster;
    files[0]->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
    files[0]->cursorCluster = cluster;
    files[0]->cursorPreviousCluster = files[0]->firstCluster;
    files[0]->physicalSize += SECTOR;
    // Freeze the cohort immediately after the first ready link, before its FAT barrier.
    assert(afatfs_FATSetNextCluster(files[0]->firstCluster, cluster) == AFATFS_OPERATION_SUCCESS);
    afatfs_reserveNextCluster(files[0]);
    // Pending peers suppress new speculation. Model a reservation created
    // before those peers queued, so the existing dirty-pair path stays covered.
    assert(!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER));
    assert(afatfs_FATSetNextCluster(cluster + 1, 0xFFFFFFFF) == AFATFS_OPERATION_SUCCESS);
    afatfs.lastClusterAllocated = (cluster + 1) | AFATFS_RESERVED_CLUSTER;
    files[0]->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
    assert(afatfs.lastClusterAllocated == ((cluster + 1) | AFATFS_RESERVED_CLUSTER));
    assert(afatfs_reservationPairDirty(cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR));
    // An in-flight pair, then a half-acknowledged pair, must retain the original wait path.
    assert(!afatfs_flushAllocationCache(files[0]));
    assert(io.pending && io.write);
    assert(afatfs_appendRegularFreeClusterContinue(files[1]) == AFATFS_OPERATION_IN_PROGRESS);
    assert(files[1]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE);
    assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
    for (unsigned retry = 0; io.pending; retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
    assert(!afatfs_reservationPairDirty(cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR));
    assert(afatfs_appendRegularFreeClusterContinue(files[1]) == AFATFS_OPERATION_IN_PROGRESS);
    assert(files[1]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE);
    assert(afatfs.lastClusterAllocated == ((cluster + 1) | AFATFS_RESERVED_CLUSTER));
    for (unsigned retry = 0; io.pending; retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
    assert(!afatfs_reservationPairDirty(cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR));
    // Reestablish the entirely dirty cohort with the same already durable first link.
    assert(afatfs_FATSetNextCluster(files[0]->firstCluster, cluster) == AFATFS_OPERATION_SUCCESS);
    assert(afatfs_reservationPairDirty(cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR));
    for (unsigned index = 1; index < 3; index++) {
        assert(afatfs_appendRegularFreeClusterContinue(files[index]) == AFATFS_OPERATION_IN_PROGRESS);
        assert(files[index]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE);
        assert(files[index]->cursorCluster == cluster + 1 + index);
        assert(afatfs.lastClusterAllocated == ((cluster + 1) | AFATFS_RESERVED_CLUSTER));
    }
    assert(!io.pending);
    uint32_t primary = afatfs_fatSectorToPhysical(0, cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t mirror = afatfs_fatSectorToPhysical(1, cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = failMirror ? mirror : primary;
    backendFailure.hits = 0;
    for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
        assert(retry < 10000);
        if (io.pending && io.write) {
            assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
        }
        pump();
        assert(files[0]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE);
        for (unsigned index = 1; index < 3; index++) {
            assert(files[index]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE);
            uint32_t offset = files[index]->firstCluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4;
            assert(fat32_isEndOfChainMarker(get32(media + primary * SECTOR + offset)));
            assert(fat32_isEndOfChainMarker(get32(media + mirror * SECTOR + offset)));
        }
        const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)(media +
            files[0]->directoryEntryPos.sectorNumberPhysical * SECTOR + files[0]->directoryEntryPos.entryIndex * 32);
        assert(entry->fileSize == SECTOR);
        assert(!afatfs_canPowerOff());
    }
    backendFailure.kind = FAIL_NONE;
    for (unsigned retry = 0;; retry++) {
        assert(retry < 10000);
        bool pending = false;
        for (unsigned index = 0; index < 3; index++) {
            pending |= afatfs_fileIsBusy(files[index]);
        }
        if (!pending) {
            break;
        }
        if (io.pending && io.write) {
            assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
        }
        pump();
    }
    for (unsigned index = 0; index < 3; index++) {
        writeBytes(files[index], payload + SECTOR, 1);
        closeFile(files[index]);
    }
    uint32_t reserveOffset = (cluster + 1) % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4;
    assert(!get32(media + primary * SECTOR + reserveOffset));
    assert(!get32(media + mirror * SECTOR + reserveOffset));
    assert(!memcmp(media + afatfs.fatStartSector * SECTOR,
        media + (afatfs.fatStartSector + afatfs.fatSectors) * SECTOR, afatfs.fatSectors * SECTOR));
}

/** @brief Fall back to the unique reserve when a fresh search reaches the end of the FAT. */
static void checkFreshSearchEnd(void)
{
    afatfsFile_t *files[2];
    const char *names[] = {"ENDONE.BIN", "ENDTWO.BIN"};
    uint8_t payload[SECTOR + 1];
    memset(payload, 0x6B, sizeof(payload));
    uint32_t reserved = afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER - 1;
    afatfs.lastClusterAllocated = reserved - 12;
    for (unsigned index = 0; index < 2; index++) {
        files[index] = openFile(names[index], "w");
        writeBytes(files[index], payload, SECTOR);
        afatfs_fileUnlockCacheSector(files[index]);
    }
    assert(afatfs_releaseReservedCluster() == AFATFS_OPERATION_SUCCESS);
    drainFreshFixture();
    uint32_t cluster = reserved - 1;
    while (afatfs_FATSetNextCluster(cluster, 0xFFFFFFFF) != AFATFS_OPERATION_SUCCESS) {
        pump();
    }
    drainFreshFixture();
    for (unsigned index = 0; index < 2; index++) {
        files[index]->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
        afatfs_appendRegularFreeClusterInitOperationState(&files[index]->operation.state.appendFreeCluster,
            files[index]->firstCluster);
        files[index]->operation.state.appendFreeCluster.searchCluster = index ? reserved : cluster;
    }
    files[0]->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
    files[0]->cursorCluster = cluster;
    files[0]->cursorPreviousCluster = files[0]->firstCluster;
    files[0]->physicalSize += SECTOR;
    assert(afatfs_FATSetNextCluster(files[0]->firstCluster, cluster) == AFATFS_OPERATION_SUCCESS);
    afatfs_reserveNextCluster(files[0]);
    // This fixture needs a pre-existing reservation, created before the peer queued.
    assert(!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER));
    assert(afatfs_FATSetNextCluster(reserved, 0xFFFFFFFF) == AFATFS_OPERATION_SUCCESS);
    afatfs.lastClusterAllocated = reserved | AFATFS_RESERVED_CLUSTER;
    files[0]->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
    assert(afatfs.lastClusterAllocated == (reserved | AFATFS_RESERVED_CLUSTER));
    assert(afatfs_reservationPairDirty(cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR));
    assert(afatfs_appendRegularFreeClusterContinue(files[1]) == AFATFS_OPERATION_IN_PROGRESS);
    assert(files[1]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE);
    assert(!afatfs_isFull());
    for (unsigned retry = 0; afatfs_fileIsBusy(files[0]) || afatfs_fileIsBusy(files[1]); retry++) {
        assert(retry < 10000);
        pump();
    }
    assert(files[1]->cursorCluster == reserved && !afatfs_isFull());
    for (unsigned index = 0; index < 2; index++) {
        writeBytes(files[index], payload + SECTOR, 1);
        closeFile(files[index]);
    }
}

/** @brief Exercise each failed FAT ACK and exhausted fresh search, then remount every file. */
int main(void)
{
    exerciseFat(true, 1);
    mount();
    checkFreshPair(false);
    checkFreshPair(true);
    checkFreshSearchEnd();
    unmount();
    mount();
    const char *names[] = {"PRIONE.BIN", "PRITWO.BIN", "PRITHREE.BIN", "MIRONE.BIN", "MIRTWO.BIN", "MIRTHREE.BIN",
        "ENDONE.BIN", "ENDTWO.BIN"};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        uint8_t payload[SECTOR + 1];
        assert(file->logicalSize == sizeof(payload));
        readBytes(file, payload, sizeof(payload));
        for (unsigned position = 0; position < sizeof(payload); position++) {
            assert(payload[position] == 0x6B);
        }
        closeFile(file);
    }
    unmount();
    puts("Fresh dirty pair: unique reserve, owned EOCs, both ACK barriers, immutable I/O, cleanup and remount passed.");
    return 0;
}
