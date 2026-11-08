// Establish the original reachable bound: one pending EOC per writer plus one look-ahead EOC.
#define main pending_bound_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

/** @brief Reach the original pending-allocation bound using valid fragmentation and public writes. */
int main(void)
{
    exerciseFat(true, 1);
    mount();
    uint32_t start = 10000;
    uint8_t payload[SECTOR];
    memset(payload, 0x3D, sizeof(payload));
    afatfsFile_t *gaps = openFile("GAPS.BIN", "w");
    for (unsigned index = 1; index < BOUND_FILES; index++) {
        afatfs.lastClusterAllocated = start + 2 * index - 1;
        writeBytes(gaps, payload, SECTOR);
        completed = false;
        assert(afatfs_fsync(gaps, fileDone, &callbackContext));
        waitDone();
    }
    closeFile(gaps);
    const char *names[] = {"PENDA.BIN", "PENDB.BIN", "PENDC.BIN"};
    afatfsFile_t *files[BOUND_FILES];
    for (unsigned index = 0; index < BOUND_FILES; index++) {
        files[index] = openFile(names[index], "w");
        completed = false;
        assert(afatfs_fsync(files[index], fileDone, &callbackContext));
        waitDone();
    }
    // Actual clean-cache eviction, so all public calls queue behind the same FAT read.
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS + 2; index++) {
        uint8_t *buffer;
        for (unsigned retry = 0; afatfs_cacheSector(afatfs.clusterStartSector + 20000 + index,
                 &buffer, AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS; retry++) {
            assert(retry < 10000);
            pump();
        }
    }
    afatfs.lastClusterAllocated = start;
    for (unsigned index = 0; index < BOUND_FILES; index++) {
        assert(afatfs_fwrite(files[index], payload, 1) == 0);
    }
    for (unsigned retry = 0; io.pending; retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
    for (unsigned index = 0; index < BOUND_FILES; index++) {
        assert(afatfs_appendRegularFreeClusterContinue(files[index]) == AFATFS_OPERATION_IN_PROGRESS);
        assert(files[index]->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE);
        assert(files[index]->cursorCluster == start + 2 * index);
    }
    uint32_t reserved = start + 2 * BOUND_FILES - 1;
    assert(afatfs.lastClusterAllocated == (reserved | AFATFS_RESERVED_CLUSTER));
    for (unsigned retry = 0; !afatfs_flushAllocationCache(files[0]); retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
    assert(!io.pending);
    uint32_t primary = afatfs_fatSectorToPhysical(0, start / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t mirror = afatfs_fatSectorToPhysical(1, start / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    for (unsigned index = 0; index <= BOUND_FILES; index++) {
        uint32_t cluster = index == BOUND_FILES ? reserved : start + 2 * index;
        uint32_t offset = cluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4;
        assert(fat32_isEndOfChainMarker(get32(media + primary * SECTOR + offset)));
        assert(fat32_isEndOfChainMarker(get32(media + mirror * SECTOR + offset)));
    }
    mount();
    for (unsigned index = 0; index < BOUND_FILES; index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        assert(file->firstCluster == 0 && file->logicalSize == 0);
        closeFile(file);
    }
    printf("Reachable original bound: %u pending writers plus one unique reserve = %u durable unlinked EOCs.\n",
        BOUND_FILES, BOUND_FILES + 1);
    unmount();
    return 0;
}
