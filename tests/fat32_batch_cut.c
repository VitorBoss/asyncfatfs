// Diagnostic: count durable EOCs left unreachable by a cut before the link phase.
#define main batch_cut_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

#ifndef BATCH_CUT_FILES
#define BATCH_CUT_FILES 1
#endif

/** @brief Reboot after durable EOCs, without running links or a clean shutdown. */
int main(void)
{
    exerciseFat(true, 1);
    mount();
    const char *names[] = {"CUTONE.BIN", "CUTTWO.BIN"};
    afatfsFile_t *files[BATCH_CUT_FILES];
    for (unsigned index = 0; index < BATCH_CUT_FILES; index++) {
        files[index] = openFile(names[index], "w");
        completed = false;
        assert(afatfs_fsync(files[index], fileDone, &callbackContext));
        waitDone();
    }
    afatfs.lastClusterAllocated = 5000;
    uint32_t primary = afatfs_fatSectorToPhysical(0, 5000 / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t mirror = afatfs_fatSectorToPhysical(1, 5000 / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t offset = 5000 % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4;
    assert(!get32(media + primary * SECTOR + offset));
    uint8_t payload[4096];
    memset(payload, 0x6D, sizeof(payload));
    for (unsigned index = 0; index < BATCH_CUT_FILES; index++) {
        assert(afatfs_fwrite(files[index], payload, sizeof(payload)) == 0);
    }
    for (unsigned index = 0; index < BATCH_CUT_FILES; index++) {
        assert(afatfs_appendRegularFreeClusterContinue(files[index]) == AFATFS_OPERATION_IN_PROGRESS);
    }
    for (unsigned index = 0; index < BATCH_CUT_FILES; index++) {
#ifndef BATCH_CUT_PROTOTYPE
        if (index != 0) {
            break;
        }
#endif
        for (unsigned retry = 0;
             files[index]->operation.state.appendFreeCluster.phase != AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
             retry++) {
            assert(retry < 10000);
            sdcard_poll();
            afatfs_appendRegularFreeClusterContinue(files[index]);
        }
    }
    for (unsigned retry = 0; !afatfs_flushAllocationCache(files[0]); retry++) {
        assert(retry < 10000);
        sdcard_poll();
    }
    assert(!io.pending);
    unsigned unlinked = 0;
    for (unsigned index = 0; index < 20; index++) {
        uint32_t first = get32(media + primary * SECTOR + offset + index * 4) & 0x0FFFFFFF;
        uint32_t second = get32(media + mirror * SECTOR + offset + index * 4) & 0x0FFFFFFF;
        assert(first == second);
        if (first) {
            assert(fat32_isEndOfChainMarker(first));
            unlinked++;
        }
    }
    assert(unlinked == EXPECTED_UNLINKED);
    // Simulate a power cut: discard all library RAM, retaining only the medium.
    mount();
    for (unsigned index = 0; index < BATCH_CUT_FILES; index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        assert(file->firstCluster == 0 && file->logicalSize == 0);
        closeFile(file);
    }
    for (unsigned index = 0; index < unlinked; index++) {
        assert(fat32_isEndOfChainMarker(get32(media + primary * SECTOR + offset + index * 4)));
        assert(fat32_isEndOfChainMarker(get32(media + mirror * SECTOR + offset + index * 4)));
    }
    unmount();
    printf("Cut after EOC ACKs: %u writer(s), %u durable unlinked clusters remain after reboot.\n",
        BATCH_CUT_FILES, unlinked);
    return 0;
}
