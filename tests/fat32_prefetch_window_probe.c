// Isolated experiment only: compile with the generated window/lib include path.
#define main prefetch_window_regression_main
#include "test_exfat.c"
#undef main
#include "asyncfatfs.c"

static uint32_t diskNextCluster(uint32_t cluster)
{
    uint32_t sector = afatfs_fatSectorToPhysical(0, cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t offset = cluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR * 4;
    return get32(media + sector * SECTOR + offset) & 0x0FFFFFFF;
}

static bool cachedDataSector(uint32_t sector)
{
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        if (afatfs.cacheDescriptor[index].sectorIndex == sector &&
            afatfs.cacheDescriptor[index].state == AFATFS_CACHE_STATE_IN_SYNC) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    afatfsFile_t *file = openFile("WINDOW.BIN", "w");
    uint8_t payload[SECTOR * 5];
    memset(payload, 0x6D, sizeof(payload));
    writeBytes(file, payload, sizeof(payload));
    closeFile(file);
    unmount();
    mount();
    file = openFile("WINDOW.BIN", "r");
    for (unsigned iteration = 0; iteration < 12; iteration++) {
        afatfs_fprefetch(file);
        while (io.pending) {
            pump();
        }
        assert(file->cursorOffset == 0);
        assert(file->operation.active == AFATFS_FILE_OPERATION_NONE);
    }
    uint32_t cluster = file->firstCluster;
    for (unsigned index = 0; index < AFATFS_PREFETCH_WINDOW_SIZE; index++) {
        assert(cachedDataSector(afatfs_fileClusterToPhysical(cluster, 0)));
        cluster = diskNextCluster(cluster);
    }
    readBytes(file, payload, sizeof(payload));
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == 0x6D);
    }
    closeFile(file);
    unmount();
    puts("Experimental window: three 512-byte clusters cached without moving cursor; payload/remount passed.");
    return 0;
}
