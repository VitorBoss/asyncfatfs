// Commit the first and sector-boundary look-ahead reservations under failed ACKs.
// Exercise the legacy single-reservation path; collected windows have no speculative EOC.
#define AFATFS_SMALL_WRITE_WINDOW_CLUSTERS 0
#define main reservation_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

static void checkReservationCommit(const char *name, uint32_t firstCluster, uint32_t length)
{
    afatfsFilePtr_t file = openFile(name, "w");
    assert(file);
    afatfs.lastClusterAllocated = firstCluster;
    uint32_t reserved = firstCluster + (length - 1) / SECTOR + 1;
    uint32_t primarySector = afatfs_fatSectorToPhysical(0, reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t mirrorSector = afatfs_fatSectorToPhysical(1, reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    uint32_t entryOffset = (reserved % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR) * 4;
    assert(!get32(media + primarySector * SECTOR + entryOffset));
    uint8_t payload[513];
    memset(payload, 0x6B, sizeof(payload));
    writeBytes(file, payload, length);
    assert(file->firstCluster == firstCluster);
    assert(afatfs.lastClusterAllocated == (reserved | AFATFS_RESERVED_CLUSTER));
    assert(fat32_isEndOfChainMarker(get32(media + primarySector * SECTOR + entryOffset)));
    assert(fat32_isEndOfChainMarker(get32(media + mirrorSector * SECTOR + entryOffset)));

    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = primarySector;
    backendFailure.hits = 0;
    completed = false;
    assert(afatfs_fsync(file, fileDone, &callbackContext));
    for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
        assert(retry < 10000 && !completed && !afatfs_canPowerOff());
        pump();
    }
    assert(!completed);
    backendFailure.kind = FAIL_NONE;
    for (unsigned retry = 0; !completed; retry++) {
        assert(retry < 10000);
        pump();
    }
    assert(!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER));
    assert(!get32(media + primarySector * SECTOR + entryOffset));
    assert(!get32(media + mirrorSector * SECTOR + entryOffset));
    const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)(media +
        file->directoryEntryPos.sectorNumberPhysical * SECTOR + file->directoryEntryPos.entryIndex * 32);
    assert(entry->fileSize == length);
    assert(media[afatfs_fileClusterToPhysical(firstCluster, 0) * SECTOR] == payload[0]);
    closeFile(file);
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    checkReservationCommit("RESFIRST.BIN", 5000, 1);
    checkReservationCommit("RESBOUND.BIN", 4991, 513);
    unmount();
    mount();
    afatfsFilePtr_t file = openFile("RESBOUND.BIN", "r");
    uint8_t payload[513];
    readBytes(file, payload, sizeof(payload));
    for (unsigned index = 0; index < sizeof(payload); index++) {
        assert(payload[index] == 0x6B);
    }
    closeFile(file);
    unmount();
    puts("Reservation commit: first/boundary EOCs, failed ACKs, sync cleanup, exact size and remount passed.");
    return 0;
}
