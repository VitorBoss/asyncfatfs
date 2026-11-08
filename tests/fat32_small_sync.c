// Frequent small durability points; the image and trace belong to the host harness.
#define main small_sync_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

/** @brief Verify each synchronized byte directly on media, without reading the library cache. */
static void checkSmallSyncMedia(const afatfsFile_t *file, unsigned length, uint8_t expected)
{
    const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)(media +
        file->directoryEntryPos.sectorNumberPhysical * SECTOR) + file->directoryEntryPos.entryIndex;
    assert(entry->fileSize == length);
    uint32_t current = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
    for (unsigned position = 0; position < length; position++) {
        if (position && position % SECTOR == 0) {
            current = get32(media + afatfs_fatSectorToPhysical(0, current / 128) * SECTOR + current % 128 * 4) & 0x0FFFFFFF;
        }
        assert(current >= 2 && current < afatfs.numClusters + 2);
        assert(media[afatfs_fileClusterToPhysical(current, 0) * SECTOR + position % SECTOR] == expected);
    }
    assert(!memcmp(media + afatfs.fatStartSector * SECTOR,
        media + (afatfs.fatStartSector + afatfs.fatSectors) * SECTOR, afatfs.fatSectors * SECTOR));
}

/** @brief Count durable data/FAT/directory writes with a sync after every 257-byte request. */
static void checkSmallSync(unsigned writers)
{
    exerciseFat(true, 1);
    mount();
    afatfsFile_t *files[2];
    const char *names[2] = {"SYNCA.BIN", "SYNCB.BIN"};
    uint8_t payload[257];
    for (unsigned index = 0; index < writers; index++) {
        files[index] = openFile(names[index], "w");
        completed = false;
        assert(afatfs_fsync(files[index], fileDone, &callbackContext));
        waitDone();
    }
    unsigned fatWrites = 0;
    unsigned directoryWrites = 0;
    unsigned dataWrites = 0;
    static durableWrite_t trace[TRACE_CAPACITY];
    for (unsigned round = 0; round < 16; round++) {
        captureWrites = trace;
        captureCount = 0;
        for (unsigned index = 0; index < writers; index++) {
            memset(payload, 0x35 + index, sizeof(payload));
            unsigned accepted = 0;
            for (unsigned retry = 0; accepted < sizeof(payload); retry++) {
                assert(retry < 200000);
                accepted += afatfs_fwrite(files[index], payload + accepted, sizeof(payload) - accepted);
                if (accepted < sizeof(payload)) {
                    pump();
                }
            }
            assert(afatfs_getFileFreeBufferSpace(files[index]) >= SECTOR - files[index]->cursorOffset % SECTOR);
        }
        for (unsigned index = 0; index < writers; index++) {
            if (round == 8 && index == 0) {
                backendFailure.kind = FAIL_WRITE_CALLBACK;
                backendFailure.target = TARGET_PREFETCH;
                backendFailure.sector = files[index]->directoryEntryPos.sectorNumberPhysical;
                backendFailure.hits = 0;
            }
            completed = false;
            assert(afatfs_fsync(files[index], fileDone, &callbackContext));
            if (backendFailure.kind != FAIL_NONE) {
                for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
                    assert(retry < 10000 && !completed && !afatfs_canPowerOff());
                    if (io.pending && io.write) {
                        assert(!memcmp(io.buffer, io.writeSnapshot, SECTOR));
                    }
                    pump();
                }
                backendFailure.kind = FAIL_NONE;
            }
            waitDone();
            checkSmallSyncMedia(files[index], (round + 1) * sizeof(payload), 0x35 + index);
        }
        captureWrites = NULL;
        for (unsigned index = 0; index < captureCount; index++) {
            uint32_t sector = trace[index].sector;
            if (sector >= afatfs.fatStartSector && sector < afatfs.clusterStartSector) {
                fatWrites++;
            } else if (sector == files[0]->directoryEntryPos.sectorNumberPhysical ||
                (writers == 2 && sector == files[1]->directoryEntryPos.sectorNumberPhysical)) {
                directoryWrites++;
            } else {
                dataWrites++;
            }
        }
    }
    printf("Small sync: writers=%u requests=16 chunk=257 FAT=%u directory=%u data=%u\n",
        writers, fatWrites, directoryWrites, dataWrites);
    // Lose every handle and cached byte immediately after fsync, without fclose.
    // This also checks recovery when the open-session dirty marker stays set.
    assert(!io.pending);
    mount();
    for (unsigned index = 0; index < writers; index++) {
        afatfsFile_t *file = openFile(names[index], "r");
        uint8_t contents[16 * 257];
        readBytes(file, contents, sizeof(contents));
        for (unsigned position = 0; position < sizeof(contents); position++) {
            assert(contents[position] == 0x35 + index);
        }
        closeFile(file);
    }
    unmount();
}

/** @brief Verify one and two writers, every sync's media image, retries and cold-cache reads. */
int main(void)
{
    checkSmallSync(1);
    checkSmallSync(2);
    return 0;
}
