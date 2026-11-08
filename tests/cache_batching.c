// Exercise the real cache against a backend that implements destructive pre-erase.
#define main cache_regression_main
#define sdcard_beginWriteBlocks regressionBeginWriteBlocks
#define sdcard_endWriteBlocks regressionEndWriteBlocks
#define sdcard_readBlock regressionReadBlock
#define sdcard_writeBlock regressionWriteBlock
#define sdcard_reset regressionReset
#include "test_exfat.c"
#undef main
#undef sdcard_beginWriteBlocks
#undef sdcard_endWriteBlocks
#undef sdcard_readBlock
#undef sdcard_writeBlock
#undef sdcard_reset

static uint32_t batchNext;
static uint32_t batchRemaining;
static unsigned batchSessions;
static unsigned batchBlocks;
static unsigned batchSingleWrites;
static unsigned batchBusy;
static unsigned batchFailures;
static bool batchCheck;
static bool batchErase;
#ifndef AFATFS_BATCH_BENCHMARK
static bool batchRangeReady(uint32_t block, uint32_t count);
#endif

sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t block, uint32_t count)
{
    if (io.pending || batchBusy) {
        if (batchBusy) {
            batchBusy--;
        }
        return SDCARD_OPERATION_BUSY;
    }
    if (batchFailures) {
        batchFailures--;
        return SDCARD_OPERATION_FAILURE;
    }
    if (batchRemaining && batchNext == block) {
        return SDCARD_OPERATION_SUCCESS;
    }
    if (batchCheck) {
#ifndef AFATFS_BATCH_BENCHMARK
        assert(batchRangeReady(block, count));
#endif
    }
    batchNext = block;
    batchRemaining = count;
    batchSessions++;
    if (batchErase) {
        memset(media + block * SECTOR, 0xE3, count * SECTOR);
    }
    return SDCARD_OPERATION_SUCCESS;
}

sdcardOperationStatus_e sdcard_endWriteBlocks(void)
{
    sdcardOperationStatus_e status = regressionEndWriteBlocks();
    if (status == SDCARD_OPERATION_SUCCESS) {
        batchRemaining = 0;
    }
    return status;
}

bool sdcard_readBlock(uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    bool accepted = regressionReadBlock(block, buffer, callback, user);
    if (accepted) {
        batchRemaining = 0;
    }
    return accepted;
}

sdcardOperationStatus_e sdcard_writeBlock(
    uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    sdcardOperationStatus_e status = regressionWriteBlock(block, buffer, callback, user);
    if (status == SDCARD_OPERATION_IN_PROGRESS || status == SDCARD_OPERATION_SUCCESS) {
        if (batchRemaining && batchNext == block) {
            batchBlocks++;
            batchRemaining--;
            batchNext++;
        } else {
            batchRemaining = 0;
            batchSingleWrites++;
        }
    }
    return status;
}

void sdcard_reset(void)
{
    regressionReset();
    batchRemaining = 0;
}

#ifndef AFATFS_BATCH_BENCHMARK
#include "../lib/asyncfatfs.c"

static bool batchRangeReady(uint32_t block, uint32_t count)
{
    if (count < 2 || count > AFATFS_NUM_CACHE_SECTORS) {
        return false;
    }
    for (uint32_t offset = 0; offset < count; offset++) {
        afatfsCacheBlockDescriptor_t *descriptor = afatfs_findCacheSector(block + offset);
        if (!descriptor || descriptor->state != AFATFS_CACHE_STATE_DIRTY || descriptor->locked) {
            return false;
        }
    }
    return true;
}

static void batchDrain(void)
{
    for (unsigned retry = 0; !afatfs_isIdle(); retry++) {
        assert(retry < 200000);
        pump();
    }
    assert(afatfs_flush());
}

static void batchPrepare(uint32_t block, unsigned value, bool locked)
{
    uint8_t *buffer;
    assert(afatfs_cacheSector(block, &buffer, AFATFS_CACHE_WRITE | (locked ? AFATFS_CACHE_LOCK : 0), 0) ==
        AFATFS_OPERATION_SUCCESS);
    memset(buffer, value, SECTOR);
}

static void batchResetCounters(void)
{
    batchDrain();
    assert(sdcard_endWriteBlocks() == SDCARD_OPERATION_SUCCESS);
    batchSessions = 0;
    batchBlocks = 0;
}

static void batchFormatFat(void)
{
    memset(media, 0, sizeof(media));
    memset(&io, 0, sizeof(io));
    const uint32_t clusterCount = 65530;
    const uint32_t fatSectors = ((clusterCount + 2) * 4 + SECTOR - 1) / SECTOR;
    mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *)(media + 446);
    partition->type = MBR_PARTITION_TYPE_FAT32;
    partition->lbaBegin = PARTITION;
    partition->numSectors = 1 + 2 * fatSectors + clusterCount;
    media[510] = 0x55;
    media[511] = 0xAA;
    fatVolumeID_t *volume = (fatVolumeID_t *)(media + PARTITION * SECTOR);
    volume->jmpBoot[0] = 0xEB;
    volume->jmpBoot[1] = 0x3C;
    volume->jmpBoot[2] = 0x90;
    volume->fat.bytesPerSector = SECTOR;
    volume->fat.sectorsPerCluster = 1;
    volume->fat.reservedSectorCount = 1;
    volume->fat.numFATs = 2;
    volume->fat.totalSectors32 = partition->numSectors;
    volume->fat.fatDescriptor.fat32.FATSize32 = fatSectors;
    volume->fat.fatDescriptor.fat32.rootCluster = 2;
    media[PARTITION * SECTOR + 510] = 0x55;
    media[PARTITION * SECTOR + 511] = 0xAA;
    for (unsigned index = 0; index < 2; index++) {
        uint8_t *table = media + (PARTITION + 1 + index * fatSectors) * SECTOR;
        put32(table, 0x0FFFFFF8);
        put32(table + 4, 0x0FFFFFFF);
        put32(table + 8, 0x0FFFFFFF);
    }
}

int main(void)
{
    batchFormatFat();
    mount();
    batchDrain();
    batchCheck = true;
    batchErase = true;
    const uint32_t firstBlock = 8000;
    memset(media + (firstBlock + 3) * SECTOR, 0xA5, SECTOR);
    batchResetCounters();
    for (unsigned offset = 0; offset < 3; offset++) {
        batchPrepare(firstBlock + offset, 0x30 + offset, false);
    }
#ifdef AFATFS_ENABLE_JOURNAL
    // FAT traversal and an LFN target must not compete for one reusable page.
    // Otherwise each successful read can evict the other without a cache miss
    // forcing dirty pages out, leaving a publishing create stalled forever.
    afatfs.openFiles[0].operation.active = AFATFS_FILE_OPERATION_CREATE_FILE;
    afatfs.openFiles[0].operation.state.createFile.phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY;
    assert(!afatfs_deferBackgroundFlush());
    // The directory safeguard must not disable existing FAT allocation batching.
    afatfs.openFiles[0].type = AFATFS_FILE_TYPE_NORMAL;
    afatfs.openFiles[0].operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    afatfs.openFiles[0].operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
    assert(afatfs_deferBackgroundFlush());
    afatfs.openFiles[0].type = AFATFS_FILE_TYPE_NONE;
    afatfs.openFiles[0].operation.active = AFATFS_FILE_OPERATION_NONE;
#endif
    assert(!afatfs_canPowerOff());
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    unsigned initialWrites = submittedWrites;
    batchBusy = 1;
    assert(!afatfs_flushCache());
    assert(!io.pending && submittedWrites == initialWrites && !afatfs_canPowerOff());
    batchFailures = 1;
    assert(!afatfs_flushCache());
    assert(!io.pending && submittedWrites == initialWrites && !afatfs_canPowerOff());
    assert(afatfs_getLastError() == AFATFS_ERROR_BACKEND_IO);
#endif
    batchDrain();
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    assert(batchSessions == 1 && batchBlocks == 3);
#else
    assert(!batchSessions && !batchBlocks);
#endif
    for (unsigned offset = 0; offset < 4; offset++) {
        for (unsigned position = 0; position < SECTOR; position++) {
            assert(media[(firstBlock + offset) * SECTOR + position] == (offset < 3 ? 0x30 + offset : 0xA5));
        }
    }
    // Dirty sectors in nonconsecutive timestamp order must not be reordered.
    batchResetCounters();
    batchPrepare(firstBlock + 8, 0x41, false);
    batchPrepare(firstBlock + 10, 0x42, false);
    batchPrepare(firstBlock + 9, 0x43, false);
    batchDrain();
    assert(batchSessions == 0);
    // A locked sector is never included in a pre-erase range.
    batchResetCounters();
    batchPrepare(firstBlock + 16, 0x51, false);
    batchPrepare(firstBlock + 17, 0x52, true);
    batchPrepare(firstBlock + 18, 0x53, false);
    for (unsigned retry = 0; afatfs.cacheDirtyEntries > 1 || io.pending; retry++) {
        assert(retry < 200000);
        pump();
    }
    assert(batchSessions == 0 && !afatfs_canPowerOff());
    afatfs_findCacheSector(firstBlock + 17)->locked = false;
    batchDrain();
    assert(media[(firstBlock + 17) * SECTOR] == 0x52);
    // Reads may interrupt a sequence; the remaining prepared suffix can restart.
    batchResetCounters();
    for (unsigned offset = 0; offset < 3; offset++) {
        batchPrepare(firstBlock + 24 + offset, 0x60 + offset, false);
    }
    assert(!afatfs_flushCache());
    while (!sdcard_poll()) {
    }
    uint8_t readBuffer[SECTOR];
    assert(sdcard_readBlock(firstBlock + 3, readBuffer, NULL, 0));
    while (!sdcard_poll()) {
    }
    batchDrain();
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    assert(batchSessions == 2 && batchBlocks == 3);
#endif
    assert(readBuffer[0] == 0xA5);
    // A failed data submission/ACK after pre-erase retains all pending buffers.
    for (unsigned failure = 0; failure < 2; failure++) {
        batchResetCounters();
        for (unsigned offset = 0; offset < 3; offset++) {
            batchPrepare(firstBlock + 32 + offset, 0x70 + offset, false);
        }
        backendFailure.kind = failure ? FAIL_WRITE_CALLBACK : FAIL_WRITE_SUBMIT;
        backendFailure.target = TARGET_PREFETCH;
        backendFailure.sector = firstBlock + 32;
        backendFailure.hits = 0;
        for (unsigned retry = 0; backendFailure.hits < 2; retry++) {
            assert(retry < 200000 && !afatfs_canPowerOff());
            pump();
        }
        backendFailure.kind = FAIL_NONE;
        batchDrain();
        for (unsigned offset = 0; offset < 3; offset++) {
            for (unsigned position = 0; position < SECTOR; position++) {
                assert(media[(firstBlock + 32 + offset) * SECTOR + position] == 0x70 + offset);
            }
        }
    }
    // A metadata caller's old future-sector hint must never pre-erase its neighbors.
    uint8_t *metadata;
    for (unsigned retry = 0; afatfs_cacheSector(firstBlock + 40, &metadata, AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA,
                                 100) != AFATFS_OPERATION_SUCCESS;
        retry++) {
        assert(retry < 200000);
        pump();
    }
    assert(afatfs_findCacheSector(firstBlock + 40)->consecutiveEraseBlockCount == 0);
    memset(metadata, 0x81, SECTOR);
    batchDrain();
    batchCheck = false;
    batchErase = false;
    unmount();
    mount();
    batchDrain();
    unmount();
    puts(
        "cache batching: consecutive runs, destructive pre-erase, busy/failure retries, locks and interrupted streams passed");
    return 0;
}
#else
static void benchmarkBatch(bool exfat)
{
    if (exfat) {
        format(false);
    } else {
        exerciseFat(true, 1);
    }
    mount();
    afatfsFilePtr_t file = openFile("BATCH.BIN", "w+");
    assert(file);
    afatfsRequest_t request;
    API_TEST(afatfs_expand(&request, file, 65536, true, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    for (unsigned retry = 0; !afatfs_flush(); retry++) {
        assert(retry < 200000);
        pump();
    }
    batchSessions = 0;
    batchBlocks = 0;
    batchSingleWrites = 0;
    unsigned initialReads = reads;
    unsigned initialWrites = writes;
    uint8_t payload[2048];
    memset(payload, 0x79, sizeof(payload));
    for (unsigned offset = 0; offset < 65536; offset += sizeof(payload)) {
        writeBytes(file, payload, sizeof(payload));
    }
    closeFile(file);
    unmount();
    printf("MULTI,%s,%u,%u,%u,%u,%u\n", exfat ? "exfat" : "fat32", reads - initialReads, writes - initialWrites,
        batchSessions, batchBlocks, batchSingleWrites);
    mount();
    file = openFile("BATCH.BIN", "r");
    assert(file);
    for (unsigned offset = 0; offset < 65536; offset += sizeof(payload)) {
        readBytes(file, payload, sizeof(payload));
        for (unsigned position = 0; position < sizeof(payload); position++) {
            assert(payload[position] == 0x79);
        }
    }
    closeFile(file);
    unmount();
}

int main(void)
{
    benchmarkBatch(false);
#ifndef TEST_FAT_ONLY
    benchmarkBatch(true);
#endif
    return 0;
}
#endif
