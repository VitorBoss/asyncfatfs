// Synthetic exFAT media and delayed block I/O; no external disk images are required.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asyncfatfs.h"
#include "sdcard.h"

#define SECTOR 512U
#define CLUSTERS 9000U
#define PARTITION 1U
#define FAT_OFFSET 24U
#define HEAP_OFFSET 96U
#define VOLUME_SECTORS (HEAP_OFFSET + CLUSTERS)
static uint8_t media[40U * 1024 * 1024];
static struct {
    bool pending : 1;
    bool write : 1;
    unsigned delay;
    uint32_t block;
    uint8_t *buffer;
    uint8_t writeSnapshot[SECTOR];
    sdcard_operationCompleteCallback_c callback;
    uint32_t user;
} io;
static unsigned ioDelay = 2;
static unsigned reads;
static unsigned writes;
static unsigned submittedWrites;
static bool holdIO;
static bool cardPresent = true;
typedef enum {
    FAIL_NONE,
    FAIL_READ_CALLBACK,
    FAIL_WRITE_CALLBACK,
    FAIL_READ_SUBMIT,
    FAIL_WRITE_SUBMIT,
    FAIL_END_WRITE,
    FAIL_READ_CORRUPT
} backendFailure_e;
typedef enum {
    TARGET_DATA,
    TARGET_BOOT,
    TARGET_JOURNAL_READ,
    TARGET_UNDO,
    TARGET_RECORD,
    TARGET_PUBLISH,
    TARGET_COMMIT,
    TARGET_RELEASE,
    TARGET_STOP,
    TARGET_PREFETCH
} backendFailureTarget_e;
static struct {
    backendFailure_e kind;
    backendFailureTarget_e target;
    uint32_t journalSector;
    uint32_t journalSectors;
    uint32_t sector;
    unsigned hits;
} backendFailure;
static unsigned failedWriteCompletions;

static bool injectBackendFailure(backendFailure_e kind, uint32_t block, const uint8_t *bytes)
{
    if (backendFailure.kind != kind) {
        return false;
    }
    bool journal =
        block >= backendFailure.journalSector && block - backendFailure.journalSector < backendFailure.journalSectors;
    bool match = false;
    switch (backendFailure.target) {
        case TARGET_DATA:
            match = !journal && bytes && bytes[0] == 0xD3;
            break;
        case TARGET_BOOT:
            match = block == PARTITION;
            break;
        case TARGET_JOURNAL_READ:
            match = journal;
            break;
        case TARGET_UNDO:
            match = journal && bytes && memcmp(bytes, "AFJNL002", 8) && memcmp(bytes, "AFUNDO02", 8);
            break;
        case TARGET_RECORD:
            match = journal && bytes && !memcmp(bytes, "AFUNDO02", 8);
            break;
        case TARGET_PUBLISH:
        case TARGET_COMMIT:
        case TARGET_RELEASE:
            if (journal && bytes && !memcmp(bytes, "AFJNL002", 8)) {
                uint32_t active = bytes[20];
                uint32_t owned = bytes[24];
                match = backendFailure.target == TARGET_PUBLISH ? active && (bytes[16] || bytes[17])
                    : backendFailure.target == TARGET_COMMIT    ? !active && owned
                                                                : !active && !owned;
            }
            break;
        case TARGET_STOP:
            match = true;
            break;
        case TARGET_PREFETCH:
            match = block == backendFailure.sector;
            break;
    }
    if (match) {
        backendFailure.hits++;
    }
    return match;
}
static unsigned endWriteBusy;
#define TRACE_CAPACITY 256
typedef struct {
    uint32_t sector;
    uint8_t bytes[SECTOR];
} durableWrite_t;
static durableWrite_t *captureWrites;
static unsigned captureCount;

static uint8_t *cluster(uint32_t count)
{
    return media + (PARTITION + HEAP_OFFSET + count - 2) * SECTOR;
}
static void put32(uint8_t *buffer, uint32_t count)
{
    for (unsigned i = 0; i < 4; i++) {
        buffer[i] = (uint8_t)(count >> (8 * i));
    }
}
static uint32_t get32(const uint8_t *buffer)
{
    return buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}
static void fat(uint32_t count, uint32_t next)
{
    put32(media + (PARTITION + FAT_OFFSET) * SECTOR + count * 4, next);
}
static uint32_t bitmapClusters[] = {20, 300, 21};
static void occupied(uint32_t count)
{
    unsigned bit = count - 2;
    cluster(bitmapClusters[bit / (SECTOR * 8)])[bit / 8 % SECTOR] |= 1U << (bit & 7);
}
static bool isOccupied(uint32_t count)
{
    unsigned bit = count - 2;
    return (cluster(bitmapClusters[bit / (SECTOR * 8)])[bit / 8 % SECTOR] & (1U << (bit & 7))) != 0;
}
static uint16_t checksum(const uint8_t *buffer, unsigned size)
{
    uint16_t sum = 0;
    for (unsigned n = 0; n < size; n++) {
        if (n == 2 || n == 3) {
            continue;
        }
        sum = (uint16_t)((sum & 1 ? 0x8000 : 0) + (sum >> 1) + buffer[n]);
    }
    return sum;
}
static uint16_t nameHash(const char *text)
{
    uint16_t hash = 0;
    for (; *text; text++) {
        unsigned ch = (unsigned char)*text;
        if (ch >= 'a' && ch <= 'z') {
            ch -= 32;
        }
        hash = (uint16_t)((hash & 1 ? 0x8000 : 0) + (hash >> 1) + ch);
        hash = (uint16_t)((hash & 1 ? 0x8000 : 0) + (hash >> 1));
    }
    return hash;
}
static uint32_t tableChecksum(const uint8_t *buffer, unsigned size)
{
    uint32_t sum = 0;
    for (unsigned n = 0; n < size; n++) {
        sum = (sum >> 1 | sum << 31) + buffer[n];
    }
    return sum;
}
static void bootChecksum(void)
{
    uint8_t *boot = media + PARTITION * SECTOR;
    uint32_t sum = 0;
    for (unsigned n = 0; n < 11 * SECTOR; n++) {
        if (n != 106 && n != 107 && n != 112) {
            sum = (sum >> 1 | sum << 31) + boot[n];
        }
    }
    for (unsigned n = 0; n < SECTOR; n += 4) {
        put32(boot + 11 * SECTOR + n, sum);
    }
}
static void format(bool fullTable)
{
    memset(media, 0, sizeof(media));
    memset(&io, 0, sizeof(io));
    reads = writes = 0;
    submittedWrites = 0;
    holdIO = false;
    cardPresent = true;
    memset(&backendFailure, 0, sizeof(backendFailure));
    failedWriteCompletions = 0;
    mbrPartitionEntry_t *part = (mbrPartitionEntry_t *)(media + 446);
    part->type = MBR_PARTITION_TYPE_EXFAT;
    part->lbaBegin = PARTITION;
    part->numSectors = VOLUME_SECTORS;
    media[510] = 0x55;
    media[511] = 0xAA;
    uint8_t *boot = media + PARTITION * SECTOR;
    fatVolumeID_t *volume = (fatVolumeID_t *)boot;
    volume->jmpBoot[0] = 0xEB;
    volume->jmpBoot[1] = 0x76;
    volume->jmpBoot[2] = 0x90;
    memcpy(volume->oemName, "EXFAT   ", 8);
    volume->exFat.partitionOffset = PARTITION;
    volume->exFat.volumeLength = VOLUME_SECTORS;
    volume->exFat.fatOffset = FAT_OFFSET;
    volume->exFat.fatLength = 71;
    volume->exFat.clusterHeapOffset = HEAP_OFFSET;
    volume->exFat.clusterCount = CLUSTERS;
    volume->exFat.rootDirectoryCluster = 2;
    volume->exFat.fileSystemRevision = 0x100;
    volume->exFat.bytesPerSectorShift = 9;
    volume->exFat.numFATs = 1;
    boot[510] = 0x55;
    boot[511] = 0xAA;
    for (unsigned n = 1; n <= 8; n++) {
        put32(boot + n * SECTOR + 508, 0xAA550000);
    }
    fat(0, 0xFFFFFFF8);
    fat(1, 0xFFFFFFFF);
    fat(2, 5);
    fat(5, 0xFFFFFFFF);
    occupied(2);
    occupied(5);
    for (unsigned n = 0; n < 3; n++) {
        occupied(bitmapClusters[n]);
        fat(bitmapClusters[n], n == 2 ? 0xFFFFFFFF : bitmapClusters[n + 1]);
    }
    // Force mount to follow the root FAT chain to find the mandatory metadata.
    for (unsigned n = 0; n < 16; n++) {
        cluster(2)[n * 32] = 0xA0;
    }
    exFatUpTableEntry_t *map = (exFatUpTableEntry_t *)cluster(5);
    map->type = EXFAT_DIR_BITMAP;
    map->firstCluster = bitmapClusters[0];
    map->dataSize = (CLUSTERS + 7) / 8;
    static uint8_t table[131072];
    unsigned tableSize;
    if (fullTable) {
        tableSize = sizeof(table);
        for (unsigned n = 0; n < 65536; n++) {
            uint16_t upper = n >= 'a' && n <= 'z' ? n - 32 : n;
            table[2 * n] = (uint8_t)upper;
            table[2 * n + 1] = (uint8_t)(upper >> 8);
        }
    } else {
        tableSize = 262;
        for (unsigned n = 0; n < 128; n++) {
            table[2 * n] = n >= 'a' && n <= 'z' ? n - 32 : n;
            table[2 * n + 1] = 0;
        }
        table[256] = 0xFF;
        table[257] = 0xFF; // Identity skip, followed by U+FFFF.
        table[258] = 0x7F;
        table[259] = 0xFF;
        table[260] = 0xFF;
        table[261] = 0xFF;
    }
    exFatUpTableEntry_t *upcase = map + 1;
    upcase->type = EXFAT_DIR_UPCASE;
    upcase->firstCluster = 500;
    upcase->dataSize = tableSize;
    upcase->tableChecksum = tableChecksum(table, tableSize);
    for (unsigned n = 0; n < (tableSize + SECTOR - 1) / SECTOR; n++) {
        uint32_t cur = 500 + n * 2;
        unsigned count = tableSize - n * SECTOR;
        if (count > SECTOR) {
            count = SECTOR;
        }
        memcpy(cluster(cur), table + n * SECTOR, count);
        occupied(cur);
        fat(cur, (n + 1) * SECTOR < tableSize ? cur + 2 : 0xFFFFFFFF);
    }
    // Existing NoFatChain stream: its FAT entries are deliberately zero.
    uint8_t *set = cluster(5) + 64;
    exFatFileEntry_t *file = (exFatFileEntry_t *)set;
    exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(set + 32);
    file->type = EXFAT_DIR_FILEDIR;
    file->numOfEntries = 2;
    file->fileAttirb = FAT_FILE_ATTRIBUTE_ARCHIVE;
    stream->type = EXFAT_DIR_STREAM;
    stream->flags = 3;
    stream->nameLength = 12;
    stream->nameHash = nameHash("existing.bin");
    stream->firstCluster = 1100;
    stream->fileSize = 500;
    stream->dataSize = 700;
    set[64] = EXFAT_DIR_FILENAME;
    for (unsigned n = 0; n < 12; n++) {
        set[66 + 2 * n] = (uint8_t)"existing.bin"[n];
    }
    file->checksum = checksum(set, 96);
    // Data after the first end marker is undefined and must not become visible on insertion.
    for (unsigned n = 6; n < 16; n++) {
        cluster(5)[n * 32] = EXFAT_DIR_FILENAME;
    }
    occupied(1100);
    occupied(1101);
    memset(cluster(1100), 0xAB, 2 * SECTOR);
    bootChecksum();
    memcpy(boot + 12 * SECTOR, boot, 12 * SECTOR);
}

static bool queueBackendIO(uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    if (!cardPresent || io.pending) {
        return false;
    }
    assert(block < sizeof(media) / SECTOR);
    io.pending = true;
    io.write = false;
    io.delay = ioDelay;
    io.block = block;
    io.buffer = buffer;
    io.callback = callback;
    io.user = user;
    return true;
}
bool sdcard_readBlock(uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    if (io.pending || injectBackendFailure(FAIL_READ_SUBMIT, block, NULL)) {
        return false;
    }
    return queueBackendIO(block, buffer, callback, user);
}
sdcardOperationStatus_e sdcard_writeBlock(
    uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    if (!cardPresent) {
        return SDCARD_OPERATION_FAILURE;
    }
    if (io.pending) {
        return SDCARD_OPERATION_BUSY;
    }
    if (injectBackendFailure(FAIL_WRITE_SUBMIT, block, buffer)) {
        return SDCARD_OPERATION_FAILURE;
    }
    if (!queueBackendIO(block, buffer, callback, user)) {
        return SDCARD_OPERATION_BUSY;
    }
    io.write = true;
    memcpy(io.writeSnapshot, buffer, SECTOR);
    submittedWrites++;
    return SDCARD_OPERATION_IN_PROGRESS;
}
bool sdcard_poll(void)
{
    if (!cardPresent) {
        return false;
    }
    if (io.pending && holdIO) {
        return false;
    }
    if (io.pending && --io.delay == 0) {
        bool failed = injectBackendFailure(
            io.write ? FAIL_WRITE_CALLBACK : FAIL_READ_CALLBACK, io.block, io.write ? io.writeSnapshot : NULL);
        if (failed && io.write) {
            // A failed ACK does not imply that the medium was left untouched.
            memcpy(media + io.block * SECTOR, io.writeSnapshot, SECTOR / 2);
            failedWriteCompletions++;
        } else if (failed) {
            memset(io.buffer, 0xE7, SECTOR / 2); // Invalid partial read, never usable.
        }
        if (!failed && io.write) {
            memcpy(media + io.block * SECTOR, io.writeSnapshot, SECTOR);
            writes++;
            if (captureWrites) {
                assert(captureCount < TRACE_CAPACITY);
                captureWrites[captureCount].sector = io.block;
                memcpy(captureWrites[captureCount++].bytes, io.writeSnapshot, SECTOR);
            }
        } else if (!failed) {
            memcpy(io.buffer, media + io.block * SECTOR, SECTOR);
            reads++;
            if (injectBackendFailure(FAIL_READ_CORRUPT, io.block, NULL)) {
                io.buffer[200] ^= 0x5A;
            }
        }
        sdcard_operationCompleteCallback_c callback = io.callback;
        io.pending = false;
        if (callback) {
            callback(io.write ? SDCARD_BLOCK_OPERATION_WRITE : SDCARD_BLOCK_OPERATION_READ, io.block,
                failed ? NULL : io.buffer, io.user);
        }
    }
    return !io.pending;
}
sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t block, uint32_t count)
{
    (void)block;
    (void)count;
    return SDCARD_OPERATION_SUCCESS;
}
sdcardOperationStatus_e sdcard_endWriteBlocks(void)
{
    if (!cardPresent) {
        return SDCARD_OPERATION_FAILURE;
    }
    if (injectBackendFailure(FAIL_END_WRITE, 0, NULL)) {
        return SDCARD_OPERATION_FAILURE;
    }
    // Reserve this delay for the final shutdown barrier; close now also ends writes.
    if (endWriteBusy && afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_STOPPING) {
        endWriteBusy--;
        return SDCARD_OPERATION_BUSY;
    }
    return io.pending ? SDCARD_OPERATION_BUSY : SDCARD_OPERATION_SUCCESS;
}
void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback)
{
    (void)callback;
}
void sdcard_reset(void)
{
    memset(&io, 0, sizeof(io));
}

static bool completed;
static afatfsFilePtr_t opened;
static int callbackContext;
static void fileDone(afatfsFilePtr_t file, void *user)
{
    assert(user == &callbackContext);
    opened = file;
    completed = true;
}
static void done(void *user)
{
    assert(user == &callbackContext);
    completed = true;
}
static void pump(void)
{
    afatfs_poll();
    assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
    if (io.pending) {
        assert(!afatfs_isIdle());
    }
    assert(!afatfs_canPowerOff());
}
static void waitDone(void)
{
    for (unsigned n = 0; !completed; n++) {
        assert(n < 200000);
        pump();
    }
}
static void mount(void)
{
    afatfs_init();
    assert(!afatfs_isIdle() && !afatfs_canPowerOff());
    assert(!afatfs_destroy(false) && afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_INITIALIZATION);
    assert(afatfs_getFreeBufferSpace() == 0);
    assert(afatfs_getFileFreeBufferSpace(NULL) == 0);
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; n++) {
        assert(n < 200000);
        pump();
    }
}
static afatfsFilePtr_t openFile(const char *name, const char *mode)
{
    completed = false;
    opened = NULL;
    assert(afatfs_fopen(name, mode, fileDone, &callbackContext));
    waitDone();
    return opened;
}
static void closeFile(afatfsFilePtr_t file)
{
    completed = false;
    assert(afatfs_fclose(file, done, &callbackContext));
    waitDone();
}
static void writeBytes(afatfsFilePtr_t file, const uint8_t *buffer, unsigned count)
{
    unsigned offset = 0;
    for (unsigned n = 0; offset < count; n++) {
        assert(n < 200000);
        offset += afatfs_fwrite(file, buffer + offset, count - offset);
        pump();
    }
    // The final sector can leave an append or seek operation pending.
    uint32_t size;
    for (unsigned n = 0; !afatfs_fGetSize(file, &size); n++) {
        assert(n < 200000);
        pump();
    }
}
static void readBytes(afatfsFilePtr_t file, uint8_t *buffer, unsigned count)
{
    unsigned offset = 0;
    for (unsigned n = 0; offset < count; n++) {
        assert(n < 200000);
        offset += afatfs_fread(file, buffer + offset, count - offset);
        pump();
    }
}
static void unmount(void)
{
    for (unsigned n = 0; !afatfs_destroy(false); n++) {
        assert(n < 200000);
    }
    assert(!io.pending);
    assert(afatfs_isIdle() && afatfs_canPowerOff());
    assert(afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_STOPPED);
    assert(afatfs_destroy(false) && afatfs_canPowerOff()); // Idempotent clean shutdown.
    assert((media[PARTITION * SECTOR + 106] & 2) == 0);
    assert(afatfs_getFreeBufferSpace() == 0);
}
static void exercise(bool fullTable)
{
    format(fullTable);
    mount();
    afatfsFilePtr_t file = openFile("EXISTING.BIN", "r");
    assert(file);
    const exFatFileEntry_t *info;
    const exFatStreamEntry_t *streamInfo;
    assert(afatfs_fGetExFatFileInfo(file, &info, &streamInfo));
    assert(info->type == EXFAT_DIR_FILEDIR && streamInfo->dataSize == 700 && streamInfo->firstCluster == 1100);
    uint8_t data[2000];
    readBytes(file, data, 700);
    for (unsigned n = 0; n < 700; n++) {
        assert(data[n] == (n < 500 ? 0xAB : 0));
    }
    completed = false;
    assert(afatfs_fseek(file, 0, AFATFS_SEEK_END, fileDone, &callbackContext) == AFATFS_OPERATION_SUCCESS);
    assert(completed); // Even a seek to the current position must invoke the callback.
    completed = false;
    assert(afatfs_fseek(file, -5000, AFATFS_SEEK_CUR, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    uint32_t position;
    assert(afatfs_ftell(file, &position) && position == 0);
    closeFile(file);
    file = openFile("short.txt", "w");
    assert(file);
    closeFile(file);
    // Extend beyond the contiguous allocation and require conversion to a FAT chain.
    file = openFile("existing.bin", "a");
    assert(file);
    memset(data, 0x5A, sizeof(data));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    mount();
    file = openFile("Existing.bin", "r");
    assert(file);
    readBytes(file, data, 700);
    for (unsigned n = 0; n < 700; n++) {
        assert(data[n] == (n < 500 ? 0xAB : 0));
    }
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x5A);
    }
    closeFile(file);
    // A maximum-length ASCII name needs nineteen entries, crossing sectors and clusters.
    char longName[256];
    memset(longName, 'x', 255);
    longName[255] = 0;
    file = openFile(longName, "w");
    assert(file);
    for (unsigned n = 0; n < sizeof(data); n++) {
        data[n] = (uint8_t)n;
    }
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    mount();
    file = openFile(longName, "r");
    assert(file);
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == (uint8_t)n);
    }
    closeFile(file);
    file = openFile("existing.bin", "w");
    assert(file);
    closeFile(file);
    unmount();
    mount();
    file = openFile("existing.bin", "r");
    assert(file);
    uint32_t truncatedSize;
    assert(afatfs_fGetSize(file, &truncatedSize) && truncatedSize == 0);
    assert(afatfs_feof(file));
    closeFile(file);
    assert(!isOccupied(1100) && !isOccupied(1101));
    for (unsigned n = 0; n < 12; n++) {
        char name[32];
        sprintf(name, "file number %u.txt", n);
        file = openFile(name, "w");
        assert(file);
        closeFile(file);
    }
    file = openFile(longName, "r+");
    assert(file);
    completed = false;
    assert(afatfs_funlink(file, done, &callbackContext));
    waitDone();
    assert(!openFile(longName, "r"));
    assert(!openFile("bad/name", "w"));
    assert(!openFile("\xC3\xA1", "w")); // The public API remains ASCII.
    completed = false;
    assert(afatfs_mkdir("sub directory", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    afatfsFilePtr_t dir = opened;
    assert(afatfs_chdir(dir));
    closeFile(dir);
    file = openFile("child.txt", "w");
    assert(file);
    writeBytes(file, (const uint8_t *)"hello", 5);
    closeFile(file);
    assert(afatfs_chdir(NULL));
    unmount();
    mount();
    completed = false;
    assert(afatfs_mkdir("sub directory", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    dir = opened;
    assert(afatfs_chdir(dir));
    closeFile(dir);
    file = openFile("CHILD.TXT", "r");
    assert(file);
    readBytes(file, data, 5);
    assert(memcmp(data, "hello", 5) == 0);
    closeFile(file);
    assert(afatfs_chdir(NULL));
    unmount();
    assert(!isOccupied(1100) && !isOccupied(1101));
    printf("exFAT %s up-case table: passed (%u reads, %u writes)\n", fullTable ? "fragmented" : "compressed", reads,
        writes);
}
static void rejectCorruption(unsigned offset, uint8_t value, bool fixChecksum)
{
    format(false);
    media[PARTITION * SECTOR + offset] = value;
    if (fixChecksum) {
        bootChecksum();
    }
    afatfs_init();
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL; n++) {
        assert(n < 10000);
        afatfs_poll();
    }
    assert(afatfs_getLastError() == AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
    while (io.pending) {
        sdcard_poll();
    }
    assert(afatfs_destroy(true));
}
static void exerciseBitmapAndContiguousDelete(void)
{
    format(false);
    mount();
    afatfsFilePtr_t truncateFile = openFile("existing.bin", "r+");
    assert(truncateFile);
    completed = false;
    assert(afatfs_fseek(truncateFile, 0, AFATFS_SEEK_END, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    completed = false;
    assert(afatfs_ftruncate(truncateFile, fileDone, &callbackContext));
    waitDone();
    uint32_t eofSize;
    assert(afatfs_fGetSize(truncateFile, &eofSize) && eofSize == 700);
    assert(afatfs_fseek(truncateFile, 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_SUCCESS);
    completed = false;
    assert(afatfs_ftruncate(truncateFile, fileDone, &callbackContext));
    waitDone();
    uint32_t size;
    assert(afatfs_fGetSize(truncateFile, &size) && size == 0);
    uint32_t position;
    assert(afatfs_ftell(truncateFile, &position) && position == 0);
    closeFile(truncateFile);
    unmount();
    mount();
    truncateFile = openFile("existing.bin", "r");
    assert(truncateFile);
    assert(afatfs_fGetSize(truncateFile, &size) && size == 0);
    closeFile(truncateFile);
    unmount();
    format(false);
    mount();
    afatfsFilePtr_t file = openFile("existing.bin", "r+");
    assert(file);
    completed = false;
    assert(afatfs_funlink(file, done, &callbackContext));
    waitDone();
    unmount();
    assert(!isOccupied(1100) && !isOccupied(1101));
    // Force the allocator through two bitmap FAT links, with all lower clusters occupied.
    memset(cluster(bitmapClusters[0]), 0xFF, SECTOR);
    memset(cluster(bitmapClusters[1]), 0xFF, SECTOR);
    mount();
    file = openFile("upper bitmap.txt", "w");
    assert(file);
    uint8_t data[1024];
    memset(data, 0x6C, sizeof(data));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    assert(isOccupied(8194) && isOccupied(8195));
    assert(get32(media + (PARTITION + FAT_OFFSET) * SECTOR + 8194 * 4) == 8195);
    mount();
    file = openFile("upper bitmap.txt", "r");
    assert(file);
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x6C);
    }
    closeFile(file);
    file = openFile("upper bitmap.txt", "w");
    assert(file);
    closeFile(file);
    unmount();
    assert(!isOccupied(8194) && !isOccupied(8195));
    puts("exFAT fragmented bitmap and NoFatChain deletion: passed");
}
typedef struct {
    bool done;
    afatfsFilePtr_t file;
} pendingOpen_t;
static void concurrentOpen(afatfsFilePtr_t file, void *user)
{
    pendingOpen_t *pending = (pendingOpen_t *)user;
    pending->file = file;
    pending->done = true;
}

static void holdPowerOffTransfer(bool shuttingDown)
{
    assert(io.pending);
    holdIO = true;
    unsigned delay = io.delay;
    unsigned beforeReads = reads;
    unsigned beforeWrites = writes;
    unsigned beforeSubmitted = submittedWrites;
    for (unsigned n = 0; n < 8; n++) {
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(
            io.delay == delay && reads == beforeReads && writes == beforeWrites && submittedWrites == beforeSubmitted);
        if (shuttingDown) {
            assert(!afatfs_destroy(false));
        } else {
            afatfs_poll();
        }
        assert(io.pending && !afatfs_isIdle() && !afatfs_canPowerOff());
    }
    assert(io.delay == delay && reads == beforeReads && writes == beforeWrites && submittedWrites == beforeSubmitted);
    holdIO = false;
}

// Two writers, a third handle doing speculative reads, delayed storage ACKs,
// and shutdown while the writers still own partially filled data sectors.
static void exerciseConcurrentPowerOff(const char *readerName, uint32_t journalSector, uint32_t journalSectors)
{
    pendingOpen_t a = {0};
    pendingOpen_t b = {0};
    assert(afatfs_fopen("PWR-A.BIN", "w", concurrentOpen, &a));
    assert(afatfs_fopen("PWR-B.BIN", "w", concurrentOpen, &b));
    for (unsigned n = 0; !a.done || !b.done; n++) {
        assert(n < 200000 && !afatfs_isIdle() && !afatfs_canPowerOff());
        pump();
    }
    assert(a.file && b.file);
    afatfsFilePtr_t reader = openFile(readerName, "r");
    assert(reader);
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    assert(!io.pending && afatfs_isIdle() && !afatfs_canPowerOff());
    assert(afatfs_getLastError() == AFATFS_ERROR_NONE); // BUSY is not an I/O error.
    unsigned started = submittedWrites;
    unsigned completedBefore = writes;
    afatfs_fprefetch(reader);
    assert(io.pending && !io.write);
    // Queue both allocations behind the outstanding prefetch.
    uint8_t one[1700];
    uint8_t two[2300];
    memset(one, 0xD1, sizeof(one));
    memset(two, 0xE2, sizeof(two));
    unsigned x = afatfs_fwrite(a.file, one, sizeof(one));
    unsigned y = afatfs_fwrite(b.file, two, sizeof(two));
    holdPowerOffTransfer(false);
    bool heldJournal = false;
    for (unsigned n = 0; x < sizeof(one) || y < sizeof(two); n++) {
        assert(n < 200000 && !afatfs_canPowerOff());
        if (!heldJournal && io.pending && io.write && io.block >= journalSector &&
            io.block - journalSector < journalSectors) {
            holdPowerOffTransfer(false);
            heldJournal = true;
        }
        x += afatfs_fwrite(a.file, one + x, sizeof(one) - x);
        y += afatfs_fwrite(b.file, two + y, sizeof(two) - y);
        pump();
    }
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    assert(!io.pending && !afatfs_isIdle() && !afatfs_canPowerOff()); // Partial buffers remain dirty/locked.
    assert(!journalSectors || heldJournal);
    completed = false;
    assert(afatfs_fseek(reader, 512, AFATFS_SEEK_SET, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    afatfs_fprefetch(reader);
    assert(io.pending && !io.write);
    holdPowerOffTransfer(true); // Destruction must also wait for speculative I/O.
    endWriteBusy = 4;
    bool heldData = false;
    bool heldCommit = false;
    bool heldRelease = false;
    bool heldClean = false;
    bool sawBackendWait = false;
    for (unsigned n = 0;; n++) {
        assert(n < 200000 && !afatfs_canPowerOff());
        if (io.pending && io.write) {
            bool journal = io.block >= journalSector && io.block - journalSector < journalSectors;
            if (!heldData && !journal && (io.buffer[0] == 0xD1 || io.buffer[0] == 0xE2)) {
                holdPowerOffTransfer(true);
                heldData = true;
            } else if (journal && !memcmp(io.buffer, "AFJNL002", 8) && get32(io.buffer + 20) == 0) {
                if (!heldCommit && get32(io.buffer + 24) == 1) {
                    holdPowerOffTransfer(true);
                    heldCommit = true;
                } else if (!heldRelease && get32(io.buffer + 24) == 0) {
                    holdPowerOffTransfer(true);
                    heldRelease = true;
                }
            } else if (journalSectors && !heldClean && io.block == PARTITION && !(io.buffer[106] & 2)) {
                holdPowerOffTransfer(true);
                heldClean = true;
            }
        }
        bool stopped = afatfs_destroy(false);
        if (stopped) {
            assert(afatfs_canPowerOff() && afatfs_isIdle() && !io.pending && !endWriteBusy);
            assert(submittedWrites - started == writes - completedBefore);
            break;
        }
        assert(!afatfs_canPowerOff());
        if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_STOPPING) {
            assert(!afatfs_isIdle());
            sawBackendWait = true;
        }
    }
    assert(heldData && sawBackendWait);
    if (journalSectors) {
        assert(heldCommit && heldRelease && heldClean && !(media[PARTITION * SECTOR + 106] & 2));
    }
    // A positive answer must remain stable without submitting any later storage writes.
    unsigned finalWrites = writes;
    unsigned finalSubmitted = submittedWrites;
    for (unsigned n = 0; n < 16; n++) {
        afatfs_poll();
        assert(afatfs_canPowerOff() && afatfs_isIdle() && !io.pending);
        assert(writes == finalWrites && submittedWrites == finalSubmitted);
    }
    mount();
    assert(!afatfs_canPowerOff());
    afatfsFilePtr_t file = openFile("PWR-A.BIN", "r");
    assert(file);
    uint32_t size;
    assert(afatfs_fGetSize(file, &size) && size == sizeof(one));
    memset(one, 0, sizeof(one));
    readBytes(file, one, sizeof(one));
    for (unsigned n = 0; n < sizeof(one); n++) {
        assert(one[n] == 0xD1);
    }
    closeFile(file);
    file = openFile("PWR-B.BIN", "r");
    assert(file);
    assert(afatfs_fGetSize(file, &size) && size == sizeof(two));
    memset(two, 0, sizeof(two));
    readBytes(file, two, sizeof(two));
    for (unsigned n = 0; n < sizeof(two); n++) {
        assert(two[n] == 0xE2);
    }
    closeFile(file);
    unmount();
    printf("concurrent power-off (%s, delay %u): held I/O, final ACKs and remounted data passed\n",
        journalSectors ? "exFAT journal" : "FAT", ioDelay);
}

static void exerciseBackendFailure(
    backendFailure_e kind, backendFailureTarget_e target, uint32_t journalSector, uint32_t journalSectors)
{
    char name[256];
    if (journalSectors) {
        memset(name, 'e', 251);
        memcpy(name + 251, ".bin", 5);
    } else {
        strcpy(name, "IOFAIL.BIN");
    }
    afatfsFilePtr_t file = openFile(name, "w");
    assert(file);
    uint8_t data[2000];
    memset(data, 0xD3, sizeof(data));
    writeBytes(file, data, sizeof(data));
    afatfsFilePtr_t other = openFile("IOKEEP.BIN", "w");
    assert(other);
    uint8_t otherData[777];
    memset(otherData, 0xA6, sizeof(otherData));
    writeBytes(other, otherData, sizeof(otherData));
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    assert(!io.pending && !afatfs_isIdle()); // Accepted data still occupies the last partial sector.
    unsigned submitted = submittedWrites;
    unsigned durable = writes;
    unsigned failed = failedWriteCompletions;
    backendFailure.kind = kind;
    backendFailure.target = target;
    backendFailure.journalSector = journalSector;
    backendFailure.journalSectors = journalSectors;
    backendFailure.hits = 0;
    for (unsigned n = 0;; n++) {
        assert(n < 20000 && !afatfs_canPowerOff());
        bool stopped = afatfs_destroy(false);
        assert(!stopped && !afatfs_canPowerOff());
        if (backendFailure.hits) {
            assert(!afatfs_isIdle());
        }
        if (backendFailure.hits >= 3) {
            break;
        }
    }
    // Persistent failure must never grant shutdown, including between retry attempts.
    for (unsigned n = 0; n < 32; n++) {
        assert(!afatfs_destroy(false));
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        if (journalSectors && target == TARGET_BOOT && (kind == FAIL_WRITE_CALLBACK || kind == FAIL_WRITE_SUBMIT)) {
            const uint8_t *header = media + journalSector * SECTOR;
            if ((int32_t)(get32(header + SECTOR + 8) - get32(header + 8)) > 0) {
                header += SECTOR;
            }
            assert(get32(header + 24) == 1 && !get32(header + 20) && !get32(header + 16));
        }
    }
    if (kind != FAIL_READ_SUBMIT) {
        assert(afatfs_getLastError() == AFATFS_ERROR_BACKEND_IO);
    }
    unsigned hits = backendFailure.hits;
    backendFailure.kind = FAIL_NONE;
    for (unsigned n = 0; !afatfs_destroy(false); n++) {
        assert(n < 200000 && !afatfs_canPowerOff());
    }
    assert(afatfs_canPowerOff() && afatfs_isIdle() && !io.pending);
    assert(submittedWrites - submitted == writes - durable + failedWriteCompletions - failed);
    mount();
    file = openFile(name, "r");
    assert(file);
    uint32_t size;
    assert(afatfs_fGetSize(file, &size) && size == sizeof(data));
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0xD3);
    }
    closeFile(file);
    other = openFile("IOKEEP.BIN", "r");
    assert(other);
    assert(afatfs_fGetSize(other, &size) && size == sizeof(otherData));
    memset(otherData, 0, sizeof(otherData));
    readBytes(other, otherData, sizeof(otherData));
    for (unsigned n = 0; n < sizeof(otherData); n++) {
        assert(otherData[n] == 0xA6);
    }
    closeFile(other);
    unmount();
    printf("backend failure (%s, kind %u, target %u): %u failures blocked power-off; retry and remount passed\n",
        journalSectors ? "exFAT" : "FAT", (unsigned)kind, (unsigned)target, hits);
}

static void exerciseFailedPrefetch(const char *source)
{
    afatfsFilePtr_t writer = openFile("RFAIL.BIN", "w");
    assert(writer);
    uint8_t data[1000];
    memset(data, 0x5C, sizeof(data));
    writeBytes(writer, data, sizeof(data));
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    afatfsFilePtr_t reader = openFile(source, "r");
    assert(reader);
    afatfs_fprefetch(reader);
    assert(io.pending && !io.write);
    uint8_t expected = media[io.block * SECTOR];
    backendFailure.kind = FAIL_READ_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = io.block;
    backendFailure.hits = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        while (io.pending) {
            pump();
        }
        assert(backendFailure.hits == attempt + 1);
        assert(afatfs_getLastError() == AFATFS_ERROR_BACKEND_IO);
        uint32_t position;
        assert(afatfs_ftell(reader, &position) && position == 0);
        assert(afatfs_fread(reader, data, 1) == 0 && io.pending);
    }
    backendFailure.kind = FAIL_NONE;
    readBytes(reader, data, 1);
    assert(data[0] == expected);
    assert(!afatfs_canPowerOff());
    closeFile(reader);
    unmount();
    mount();
    reader = openFile("RFAIL.BIN", "r");
    assert(reader);
    uint32_t size;
    assert(afatfs_fGetSize(reader, &size) && size == sizeof(data));
    readBytes(reader, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x5C);
    }
    closeFile(reader);
    unmount();
    puts("failed prefetch with pending writer: retries preserved cursor and shutdown data");
}

static void exerciseRuntimeJournalCorruption(void)
{
    format(false);
    mount();
    afatfsFilePtr_t file = openFile("AFATFS.JNL", "r");
    assert(file);
    const exFatFileEntry_t *entry;
    const exFatStreamEntry_t *stream;
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
    backendFailure.journalSector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
    backendFailure.journalSectors = (uint32_t)stream->dataSize / SECTOR;
    closeFile(file);
    char name[256];
    memset(name, 'c', 251);
    memcpy(name + 251, ".bin", 5);
    file = openFile(name, "w");
    assert(file);
    uint8_t data[2000];
    memset(data, 0x6A, sizeof(data));
    writeBytes(file, data, sizeof(data));
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    backendFailure.kind = FAIL_READ_CORRUPT;
    backendFailure.target = TARGET_JOURNAL_READ;
    backendFailure.hits = 0;
    for (unsigned n = 0; !afatfs_isReadOnly(); n++) {
        assert(n < 200000 && !afatfs_destroy(false) && !afatfs_canPowerOff());
    }
    assert(backendFailure.hits == 1 && afatfs_getLastError() == AFATFS_ERROR_RECOVERY_REQUIRED);
    assert(!io.pending);
    backendFailure.kind = FAIL_NONE;
    unsigned frozenWrites = writes;
    unsigned frozenSubmitted = submittedWrites;
    for (unsigned n = 0; n < 32; n++) {
        assert(!afatfs_destroy(false) && !afatfs_flush());
        afatfs_poll();
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(writes == frozenWrites && submittedWrites == frozenSubmitted && !io.pending);
    }
    // Explicit simulated forced discard is not a clean shutdown authorization.
    assert(afatfs_destroy(true) && !afatfs_canPowerOff());
    mount();
    assert(!afatfs_isReadOnly());
    file = openFile("existing.bin", "r");
    assert(file);
    readBytes(file, data, 700);
    for (unsigned n = 0; n < 700; n++) {
        assert(data[n] == (n < 500 ? 0xAB : 0));
    }
    closeFile(file);
    unmount();
    puts("runtime journal CRC error: preserved state, blocked clean shutdown, healthy-media replay passed");
}

static void exerciseJournalBackendFailures(void)
{
    static const struct {
        backendFailure_e kind;
        backendFailureTarget_e target;
    } cases[] = {// Start with ownership release: all file handles have already closed here.
        {FAIL_WRITE_CALLBACK, TARGET_RELEASE}, {FAIL_WRITE_SUBMIT, TARGET_RELEASE}, {FAIL_WRITE_CALLBACK, TARGET_DATA},
        {FAIL_WRITE_SUBMIT, TARGET_DATA}, {FAIL_READ_CALLBACK, TARGET_BOOT}, {FAIL_READ_SUBMIT, TARGET_BOOT},
        {FAIL_WRITE_CALLBACK, TARGET_BOOT}, {FAIL_WRITE_SUBMIT, TARGET_BOOT}, {FAIL_READ_CALLBACK, TARGET_JOURNAL_READ},
        {FAIL_READ_SUBMIT, TARGET_JOURNAL_READ}, {FAIL_WRITE_CALLBACK, TARGET_UNDO},
        {FAIL_WRITE_CALLBACK, TARGET_RECORD}, {FAIL_WRITE_CALLBACK, TARGET_PUBLISH},
        {FAIL_WRITE_CALLBACK, TARGET_COMMIT}, {FAIL_WRITE_SUBMIT, TARGET_COMMIT}, {FAIL_END_WRITE, TARGET_STOP}};
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        format(false);
        mount();
        afatfsFilePtr_t file = openFile("AFATFS.JNL", "r");
        assert(file);
        const exFatFileEntry_t *entry;
        const exFatStreamEntry_t *stream;
        assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
        uint32_t sector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
        uint32_t sectors = (uint32_t)stream->dataSize / SECTOR;
        closeFile(file);
        exerciseBackendFailure(cases[n].kind, cases[n].target, sector, sectors);
    }
    format(false);
    mount();
    exerciseFailedPrefetch("existing.bin");
    exerciseRuntimeJournalCorruption();
}

static void exerciseJournalPowerOff(void)
{
    unsigned savedDelay = ioDelay;
    const unsigned delays[] = {1, 7, 31};
    for (unsigned n = 0; n < sizeof(delays) / sizeof(delays[0]); n++) {
        format(false);
        ioDelay = delays[n];
        mount();
        afatfsFilePtr_t file = openFile("AFATFS.JNL", "r");
        assert(file);
        const exFatFileEntry_t *entry;
        const exFatStreamEntry_t *stream;
        assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
        uint32_t sector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
        uint32_t sectors = (uint32_t)stream->dataSize / SECTOR;
        closeFile(file);
        exerciseConcurrentPowerOff("existing.bin", sector, sectors);
    }
    ioDelay = savedDelay;
}

typedef struct {
    sdcard_operationCompleteCallback_c callback;
    sdcardBlockOperation_e operation;
    uint32_t block;
    uint32_t user;
    uint8_t *buffer;
} retiredCompletion_t;

static void replayRetiredCompletion(const retiredCompletion_t *retired)
{
    if (retired->callback) {
        // A backend may deliver a queued software ACK after cancellation. It may
        // no longer access old buffers or perform the old physical transfer.
        retired->callback(retired->operation, retired->block, retired->buffer, retired->user);
        retired->callback(retired->operation, retired->block, NULL, retired->user);
    }
}

static retiredCompletion_t removeTestCard(unsigned tear, afatfsFilePtr_t oldHandle)
{
    retiredCompletion_t retired = {0};
    if (io.pending) {
        retired.callback = io.callback;
        retired.operation = io.write ? SDCARD_BLOCK_OPERATION_WRITE : SDCARD_BLOCK_OPERATION_READ;
        retired.block = io.block;
        retired.user = io.user;
        retired.buffer = io.buffer;
        if (io.write && tear) {
            unsigned start = tear == 1 ? 0 : 256;
            memcpy(media + io.block * SECTOR + start, io.writeSnapshot + start, 256);
        }
    }
    cardPresent = false;
    afatfs_notifyMediaRemoved();
    assert(!io.pending && afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_REMOVED);
    assert(afatfs_getLastError() == AFATFS_ERROR_MEDIA_REMOVED);
    unsigned savedReads = reads;
    unsigned savedWrites = writes;
    unsigned savedSubmitted = submittedWrites;
    for (unsigned n = 0; n < 16; n++) {
        replayRetiredCompletion(&retired);
        afatfs_poll();
        assert(!afatfs_destroy(false) && !afatfs_flush());
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        uint8_t byte = 0xC9;
        if (oldHandle) {
            assert(afatfs_fwrite(oldHandle, &byte, 1) == 0 && afatfs_fread(oldHandle, &byte, 1) == 0);
            afatfs_fprefetch(oldHandle);
            afatfs_fputc(oldHandle, byte);
            assert(afatfs_fseek(oldHandle, 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_FAILURE);
            assert(!afatfs_fsync(oldHandle, NULL, NULL) && !afatfs_ftruncate(oldHandle, NULL, NULL));
        }
        assert(!afatfs_fopen("ABSENT.BIN", "w", NULL, NULL) && !afatfs_mkdir("ABSENT", NULL, NULL));
        assert(!afatfs_chdir(NULL));
        assert(!io.pending && reads == savedReads && writes == savedWrites && submittedWrites == savedSubmitted);
        assert(afatfs_getLastError() == AFATFS_ERROR_MEDIA_REMOVED);
    }
    return retired;
}

static void mountReinsertedCard(const retiredCompletion_t *retired)
{
    cardPresent = true;
    afatfs_init();
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; n++) {
        assert(n < 200000 && !afatfs_isIdle() && !afatfs_canPowerOff());
        afatfsError_e error = afatfs_getLastError();
        replayRetiredCompletion(retired);
        assert(afatfs_getLastError() == error);
        pump();
    }
    assert(!afatfs_canPowerOff());
}

static void removalCloseDone(void *user)
{
    ++*(unsigned *)user;
}

typedef enum {
    REMOVE_INIT,
    REMOVE_PREFETCH,
    REMOVE_DATA,
    REMOVE_JOURNAL_READ,
    REMOVE_UNDO,
    REMOVE_RECORD,
    REMOVE_PUBLISH,
    REMOVE_COMMIT,
    REMOVE_RELEASE,
    REMOVE_CLEAN,
    REMOVE_STOPPING,
    REMOVE_STOPPED,
    REMOVE_RECOVERY_READ,
    REMOVE_RECOVERY_WRITE
} removalPoint_e;

static bool removalPointMatches(removalPoint_e point, uint32_t journalSector, uint32_t journalSectors)
{
    if (point == REMOVE_STOPPED) {
        return afatfs_canPowerOff();
    }
    if (point == REMOVE_STOPPING) {
        return afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_STOPPING;
    }
    if (!io.pending) {
        return false;
    }
    bool journal = io.block >= journalSector && io.block - journalSector < journalSectors;
    const uint8_t *bytes = io.writeSnapshot;
    switch (point) {
        case REMOVE_JOURNAL_READ:
        case REMOVE_RECOVERY_READ:
            return !io.write && journal;
        case REMOVE_DATA:
            return io.write && !journal && bytes[0] == 0x8F;
        case REMOVE_UNDO:
            return io.write && journal && memcmp(bytes, "AFJNL002", 8) && memcmp(bytes, "AFUNDO02", 8);
        case REMOVE_RECORD:
            return io.write && journal && !memcmp(bytes, "AFUNDO02", 8);
        case REMOVE_PUBLISH:
            return io.write && journal && !memcmp(bytes, "AFJNL002", 8) && get32(bytes + 20) && get32(bytes + 16);
        case REMOVE_COMMIT:
            return io.write && journal && !memcmp(bytes, "AFJNL002", 8) && !get32(bytes + 20) && get32(bytes + 24);
        case REMOVE_RELEASE:
            return io.write && journal && !memcmp(bytes, "AFJNL002", 8) && !get32(bytes + 24);
        case REMOVE_CLEAN:
            return io.write && io.block == PARTITION && !(bytes[106] & 2);
        case REMOVE_RECOVERY_WRITE:
            return io.write && !journal && io.block != PARTITION;
        default:
            return false;
    }
}

static void verifyRemovalPattern(const char *name, unsigned length, uint8_t expected)
{
    afatfsFilePtr_t file = openFile(name, "r");
    assert(file);
    uint32_t size;
    assert(afatfs_fGetSize(file, &size) && size == length);
    uint8_t data[2000];
    assert(length <= sizeof(data));
    readBytes(file, data, length);
    for (unsigned n = 0; n < length; n++) {
        assert(data[n] == expected);
    }
    closeFile(file);
}

static void exerciseExFatMediaRemoval(void)
{
    enum {
        imageBytes = (PARTITION + VOLUME_SECTORS) * SECTOR
    };
    static uint8_t original[imageBytes];
    static uint8_t replacement[imageBytes];
    static uint8_t withdrawn[imageBytes];
    uint32_t journalSector = 0;
    uint32_t journalSectors = 0;
    for (unsigned card = 0; card < 2; card++) {
        format(false);
        mount();
        afatfsFilePtr_t file = openFile("SAFE.BIN", "w");
        assert(file);
        uint8_t data[1200];
        memset(data, card ? 0x4D : 0x7E, sizeof(data));
        writeBytes(file, data, sizeof(data));
        closeFile(file);
        file = openFile("AFATFS.JNL", "r");
        assert(file);
        const exFatFileEntry_t *entry;
        const exFatStreamEntry_t *stream;
        assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
        if (!card) {
            journalSector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
            journalSectors = (uint32_t)stream->dataSize / SECTOR;
        }
        closeFile(file);
        unmount();
        memcpy(card ? replacement : original, media, imageBytes);
    }
    char name[256];
    memset(name, 'm', 251);
    memcpy(name + 251, ".bin", 5);
    unsigned cases = 0;
    unsigned recoveredHandoffs = 0;
    for (unsigned point = REMOVE_INIT; point <= REMOVE_RECOVERY_WRITE; point++) {
        for (unsigned tear = 0; tear < 3; tear++) {
            for (unsigned otherCard = 0; otherCard < 2; otherCard++) {
                memcpy(media, original, imageBytes);
                cardPresent = true;
                endWriteBusy = 0;
                afatfsFilePtr_t handle = NULL;
                unsigned closeCount = 0;
                if (point == REMOVE_INIT) {
                    afatfs_init();
                    afatfs_poll();
                    assert(io.pending && !io.write);
                } else {
                    mount();
                    handle = openFile(name, "w");
                    assert(handle);
                    uint8_t data[2000];
                    memset(data, 0x8F, sizeof(data));
                    writeBytes(handle, data, sizeof(data));
                    for (unsigned n = 0; !afatfs_flush(); n++) {
                        assert(n < 200000);
                        pump();
                    }
                    if (point == REMOVE_PREFETCH) {
                        afatfsFilePtr_t reader = openFile("SAFE.BIN", "r");
                        assert(reader);
                        afatfs_fprefetch(reader);
                        assert(io.pending && !io.write);
                    } else {
                        assert(afatfs_fclose(handle, removalCloseDone, &closeCount));
                        endWriteBusy = point == REMOVE_STOPPING ? 8 : 0;
                        removalPoint_e wanted = point >= REMOVE_RECOVERY_READ ? REMOVE_COMMIT : (removalPoint_e)point;
                        for (unsigned n = 0; !removalPointMatches(wanted, journalSector, journalSectors); n++) {
                            assert(n < 200000 && !afatfs_canPowerOff());
                            bool done = afatfs_destroy(false);
                            assert(!done || wanted == REMOVE_STOPPED);
                        }
                    }
                }
                if (point == REMOVE_RELEASE) {
                    assert(!(media[PARTITION * SECTOR + 106] & 2)); // Ownership cannot be released before the boot ACK.
                } else if (point == REMOVE_CLEAN) {
                    const uint8_t *header = media + journalSector * SECTOR;
                    if ((int32_t)(get32(header + SECTOR + 8) - get32(header + 8)) > 0) {
                        header += SECTOR;
                    }
                    assert(get32(header + 24) == 1 && !get32(header + 20) && !get32(header + 16));
                }
                retiredCompletion_t retired = removeTestCard(tear, handle);
                if (point >= REMOVE_RECOVERY_READ) {
                    cardPresent = true;
                    afatfs_init();
                    for (unsigned n = 0; !removalPointMatches((removalPoint_e)point, journalSector, journalSectors);
                        n++) {
                        assert(n < 200000 && !afatfs_canPowerOff());
                        pump();
                    }
                    retired = removeTestCard(tear, NULL); // Replay must survive another removal.
                }
                unsigned savedCloseCount = closeCount;
                memcpy(withdrawn, media, imageBytes);
                if (otherCard) {
                    memcpy(media, replacement, imageBytes);
                    cardPresent = true;
                    for (unsigned n = 0; n < 16; n++) {
                        afatfs_poll();
                        sdcard_poll();
                        assert(!afatfs_canPowerOff() && !io.pending);
                    }
                    assert(!memcmp(media, replacement, imageBytes)); // No old write may reach the new card.
                    mountReinsertedCard(&retired);
                    assert(!afatfs_isReadOnly());
                    verifyRemovalPattern("SAFE.BIN", 1200, 0x4D);
                    assert(!openFile(name, "r"));
                    afatfsFilePtr_t fresh = openFile("NEW.BIN", "w");
                    assert(fresh);
                    uint8_t byte = 0xB3;
                    writeBytes(fresh, &byte, 1);
                    closeFile(fresh);
                    unmount();
                    memcpy(media, withdrawn, imageBytes);
                }
                bool previousHandoffGuard = point == REMOVE_CLEAN && tear != 1;
                if (previousHandoffGuard) {
                    // Interrupt the formerly read-only handoff recovery once more.
                    cardPresent = true;
                    afatfs_init();
                    for (unsigned n = 0; !removalPointMatches(REMOVE_RECOVERY_READ, journalSector, journalSectors);
                        n++) {
                        assert(n < 200000 && !afatfs_canPowerOff());
                        pump();
                    }
                    retired = removeTestCard(0, NULL);
                }
                mountReinsertedCard(&retired);
                assert(!afatfs_isReadOnly());
                verifyRemovalPattern("SAFE.BIN", 1200, 0x7E);
                if (savedCloseCount) {
                    verifyRemovalPattern(name, 2000, 0x8F);
                }
                assert(closeCount == savedCloseCount); // No cancelled application callback can revive.
                if (previousHandoffGuard) {
                    afatfsFilePtr_t resumed = openFile("RESUME.BIN", "w");
                    assert(resumed);
                    uint8_t data[73];
                    memset(data, 0xA5, sizeof(data));
                    writeBytes(resumed, data, sizeof(data));
                    closeFile(resumed);
                    assert(!afatfs_canPowerOff());
                    recoveredHandoffs++;
                }
                unmount();
                if (previousHandoffGuard) {
                    mount();
                    assert(!afatfs_isReadOnly());
                    verifyRemovalPattern("RESUME.BIN", 73, 0xA5);
                    verifyRemovalPattern("SAFE.BIN", 1200, 0x7E);
                    if (savedCloseCount) {
                        verifyRemovalPattern(name, 2000, 0x8F);
                    }
                    unmount();
                }
                cases++;
            }
        }
    }
    assert(cases == 84 && recoveredHandoffs == 4);
    printf("exFAT media removal: %u same/replacement-card cases, %u formerly read-only handoffs recovered writable\n",
        cases, recoveredHandoffs);
}
static uint32_t fixtureCrc32(const uint8_t *bytes, unsigned length)
{
    uint32_t crc = UINT32_MAX;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xEDB88320) : 0);
        }
    }
    return ~crc;
}

static void exerciseUntrustedHandoffs(void)
{
    enum {
        imageBytes = (PARTITION + VOLUME_SECTORS) * SECTOR
    };
    static uint8_t baseline[imageBytes];
    format(false);
    mount();
    afatfsFilePtr_t file = openFile("SAFE.BIN", "w");
    assert(file);
    uint8_t data[1200];
    memset(data, 0x76, sizeof(data));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    file = openFile("AFATFS.JNL", "r");
    assert(file);
    const exFatFileEntry_t *entry;
    const exFatStreamEntry_t *stream;
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
    uint32_t journalSector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
    closeFile(file);
    unmount();
    memcpy(baseline, media, imageBytes);
    const uint8_t *headers = baseline + journalSector * SECTOR;
    unsigned latest = (int32_t)(get32(headers + SECTOR + 8) - get32(headers + 8)) > 0 ? 1 : 0;
    assert(!get32(headers + latest * SECTOR + 24) && get32(headers + (latest ^ 1) * SECTOR + 24));
    for (unsigned kind = 0; kind < 8; kind++) {
        memcpy(media, baseline, imageBytes);
        if (kind != 4) {
            media[PARTITION * SECTOR + 106] |= 2;
        }
        if (kind == 1) {
            uint8_t *safeSet = cluster(5) + 8 * 32;
            assert(safeSet[0] == EXFAT_DIR_FILEDIR && safeSet[66] == 'S');
            safeSet[2] ^= 0x5A; // An interrupted external metadata update, invisible to the private log.
        }
        for (unsigned slot = 0; slot < 2; slot++) {
            uint8_t *header = media + (journalSector + slot) * SECTOR;
            if (kind == 2) {
                put32(header + 28, get32(header + 28) ^ 1); // CRC-valid geometry/volume mismatch.
            }
            if (kind == 3) {
                header[200] ^= 0x5A;
                continue;
            } // Neither header has a valid CRC.
            if (kind == 4 || kind == 5) {
                put32(header + 20, 1);
                put32(header + 24, kind == 4 ? 1 : 0);
                put32(header + 16, 0);
            }
            if (kind == 6) {
                put32(header + 20, 0);
                put32(header + 24, 1);
                put32(header + 16, 1);
            }
            if (kind == 7 && slot != latest) {
                put32(header + 20, 1);
                put32(header + 24, 1);
                put32(header + 16, 1); // Older owned log must not override a released latest header.
            }
            put32(header + 508, fixtureCrc32(header, 508));
        }
        unsigned beforeWrites = writes;
        unsigned beforeSubmitted = submittedWrites;
        mount();
        assert(afatfs_isReadOnly() && afatfs_getLastError() == AFATFS_ERROR_RECOVERY_REQUIRED);
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(!openFile("UNSAFE.BIN", "w"));
        for (unsigned n = 0; !afatfs_destroy(false); n++) {
            assert(n < 200000);
        }
        assert(writes == beforeWrites && submittedWrites == beforeSubmitted && !io.pending);
        assert((media[PARTITION * SECTOR + 106] & 2) == (kind == 4 ? 0 : 2));
    }
    puts("untrusted handoff: 8 legacy/external/invalid-journal cases stayed read-only with zero writes");
}

static void exerciseConcurrent(void)
{
    format(false);
    mount();
    pendingOpen_t a = {0};
    pendingOpen_t b = {0};
    assert(afatfs_fopen("first parallel file.txt", "w", concurrentOpen, &a));
    assert(afatfs_fopen("second parallel file.txt", "w", concurrentOpen, &b));
    for (unsigned n = 0; !a.done || !b.done; n++) {
        assert(n < 200000);
        pump();
    }
    assert(a.file && b.file);
    uint8_t one[1700];
    uint8_t two[1700];
    memset(one, 0x17, sizeof(one));
    memset(two, 0x29, sizeof(two));
    unsigned x = 0;
    unsigned y = 0;
    for (unsigned n = 0; x < sizeof(one) || y < sizeof(two); n++) {
        assert(n < 200000);
        x += afatfs_fwrite(a.file, one + x, sizeof(one) - x);
        y += afatfs_fwrite(b.file, two + y, sizeof(two) - y);
        pump();
    }
    uint32_t size;
    while (!afatfs_fGetSize(a.file, &size) || !afatfs_fGetSize(b.file, &size)) {
        pump();
    }
    for (unsigned n = 0; !afatfs_flush(); n++) {
        assert(n < 200000);
        pump();
    }
    uint32_t tails = 2 * (SECTOR - sizeof(one) % SECTOR);
    uint32_t available = afatfs_getFreeBufferSpace();
    assert(available >= tails && available % SECTOR == tails % SECTOR);
    assert(afatfs_getFileFreeBufferSpace(a.file) == available - tails / 2);
    assert(afatfs_getFileFreeBufferSpace(b.file) == available - tails / 2);
    assert(afatfs_getFileFreeBufferSpace(NULL) == 0);
    closeFile(a.file);
    closeFile(b.file);
    unmount();
    mount();
    afatfsFilePtr_t file = openFile("first parallel file.txt", "r");
    assert(file);
    readBytes(file, two, sizeof(two));
    assert(afatfs_getFileFreeBufferSpace(file) == 0);
    for (unsigned n = 0; n < sizeof(two); n++) {
        assert(two[n] == 0x17);
    }
    closeFile(file);
    file = openFile("second parallel file.txt", "r");
    assert(file);
    readBytes(file, one, sizeof(one));
    for (unsigned n = 0; n < sizeof(one); n++) {
        assert(one[n] == 0x29);
    }
    closeFile(file);
    unmount();
    puts("exFAT concurrent creation and allocation: passed");
}
static void exerciseUnsupportedMetadata(void)
{
    format(false);
    uint8_t *set = cluster(5) + 64;
    exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(set + 32);
    stream->dataSize = (uint64_t)UINT32_MAX + 1;
    ((exFatFileEntry_t *)set)->checksum = checksum(set, 96);
    uint8_t saved[96];
    memcpy(saved, set, sizeof(saved));
    mount();
    assert(!openFile("existing.bin", "w")); // Never create a duplicate of an unsupported file.
    assert(!openFile("existing.bin", "r"));
    unmount();
    assert(memcmp(saved, set, sizeof(saved)) == 0);
    format(false);
    cluster(500)[0] ^= 1; // Up-case bytes no longer match their directory checksum.
    afatfs_init();
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL; n++) {
        assert(n < 10000);
        afatfs_poll();
    }
    assert(afatfs_getLastError() == AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
    assert(!afatfs_destroy(false) && !afatfs_canPowerOff());
    assert(afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL);
    while (io.pending) {
        sdcard_poll();
    }
    assert(afatfs_destroy(true));
    puts("exFAT unsupported metadata and up-case corruption: passed");
}
static void exerciseDirtyMount(void)
{
    format(false);
    media[PARTITION * SECTOR + 106] |= 2;
    mount();
    assert(afatfs_isReadOnly() && afatfs_getLastError() == AFATFS_ERROR_RECOVERY_REQUIRED);
    assert(afatfs_getFreeBufferSpace() == 0);
    afatfsFilePtr_t file = openFile("existing.bin", "r");
    assert(file);
    assert(afatfs_getFileFreeBufferSpace(file) == 0);
    assert(!afatfs_ftruncate(file, fileDone, &callbackContext));
    assert(!afatfs_funlink(file, done, &callbackContext));
    assert(!openFile("denied.bin", "w"));
    completed = false;
    assert(afatfs_mkdir("denied dir", fileDone, &callbackContext));
    waitDone();
    assert(!opened);
    completed = false;
    assert(afatfs_fsync(file, fileDone, &callbackContext));
    waitDone();
    closeFile(file);
    for (unsigned n = 0; !afatfs_destroy(false); n++) {
        assert(n < 200000);
    }
    assert(writes == 0 && (media[PARTITION * SECTOR + 106] & 2));
    puts("exFAT dirty mount is read-only and preserves VolumeDirty: passed");
}

static void exerciseSync(void)
{
    format(false);
    mount();
    afatfsFilePtr_t file = openFile("synced.bin", "w");
    assert(file);
    uint8_t data[777];
    memset(data, 0x67, sizeof(data));
    writeBytes(file, data, sizeof(data));
    completed = false;
    assert(afatfs_fsync(file, fileDone, &callbackContext));
    waitDone();
    assert(opened == file);
    const exFatFileEntry_t *entry;
    const exFatStreamEntry_t *stream;
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
    assert(stream->dataSize == sizeof(data) && stream->fileSize == sizeof(data));
    closeFile(file);
    file = openFile("abort.bin", "w");
    assert(file);
    assert(afatfs_fwrite(file, data, 1) == 0); // Allocate asynchronously, then stop without accepting data.
    uint32_t size;
    assert(!afatfs_fGetSize(file, &size) && afatfs_getFileFreeBufferSpace(file) == 0);
    while (!afatfs_fGetSize(file, &size)) {
        pump();
    }
    assert(size == 0);
    closeFile(file);
    unmount();
    mount();
    file = openFile("abort.bin", "r");
    assert(file);
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
    assert(stream->dataSize == 0 && stream->firstCluster == 0);
    closeFile(file);
    file = openFile("synced.bin", "r");
    assert(file);
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x67);
    }
    closeFile(file);
    unmount();
    puts("exFAT fsync and empty preallocation close: passed");
}
static void exerciseInterruptedRecovery(void)
{
    enum {
        imageBytes = (PARTITION + VOLUME_SECTORS) * SECTOR
    };
    static uint8_t baseline[imageBytes];
    static uint8_t interrupted[imageBytes];
    static durableWrite_t mutation[TRACE_CAPACITY];
    static durableWrite_t recovery[TRACE_CAPACITY];
    format(false);
    mount();
    afatfsFilePtr_t file = openFile("AFATFS.JNL", "r");
    assert(file);
    const exFatFileEntry_t *entry;
    const exFatStreamEntry_t *stream;
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream));
    unsigned journalSector = PARTITION + HEAP_OFFSET + stream->firstCluster - 2;
    closeFile(file);
    unmount();
    memcpy(baseline, media, imageBytes);
    captureWrites = mutation;
    captureCount = 0;
    mount();
    file = openFile("existing.bin", "a");
    assert(file);
    uint8_t data[1800];
    memset(data, 0x69, sizeof(data));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    unsigned mutationCount = captureCount;
    captureWrites = NULL;
    memcpy(media, baseline, imageBytes);
    bool found = false;
    for (unsigned n = 0; n < mutationCount; n++) {
        memcpy(media + mutation[n].sector * SECTOR, mutation[n].bytes, SECTOR);
        const uint8_t *header = media + journalSector * SECTOR;
        if (get32(header + SECTOR + 8) > get32(header + 8)) {
            header += SECTOR;
        }
        if (get32(header + 20) && get32(header + 16) >= 2) {
            memcpy(interrupted, media, imageBytes);
            found = true;
        }
    }
    assert(found);
    memcpy(media, interrupted, imageBytes);
    captureWrites = recovery;
    captureCount = 0;
    mount();
    assert(!afatfs_isReadOnly());
    unsigned recoveryCount = captureCount;
    captureWrites = NULL;
    assert(recoveryCount >= 3);
    unmount();
    unsigned cuts = 0;
    for (unsigned prefix = 0; prefix <= recoveryCount; prefix++) {
        for (unsigned mode = 0; mode < (prefix < recoveryCount ? 3U : 1U); mode++) {
            memcpy(media, interrupted, imageBytes);
            for (unsigned n = 0; n < prefix; n++) {
                memcpy(media + recovery[n].sector * SECTOR, recovery[n].bytes, SECTOR);
            }
            if (mode) {
                unsigned start = mode == 1 ? 0 : 256;
                memcpy(media + recovery[prefix].sector * SECTOR + start, recovery[prefix].bytes + start, 256);
            }
            mount();
            assert(!afatfs_isReadOnly());
            file = openFile("existing.bin", "r");
            assert(file);
            readBytes(file, data, 700);
            for (unsigned n = 0; n < 700; n++) {
                assert(data[n] == (n < 500 ? 0xAB : 0));
            }
            closeFile(file);
            unmount();
            cuts++;
        }
    }
    for (unsigned damaged = 0; damaged < 3; damaged++) {
        memcpy(media, interrupted, imageBytes);
        if (damaged == 0) {
            media[journalSector * SECTOR + 200] ^= 0x5A;
            media[(journalSector + 1) * SECTOR + 200] ^= 0x5A;
        } else {
            media[(journalSector + (damaged == 1 ? 2 : 3)) * SECTOR + 200] ^= 0x5A;
        }
        writes = 0;
        mount();
        assert(afatfs_isReadOnly() && writes == 0);
        assert(afatfs_getLastError() == AFATFS_ERROR_RECOVERY_REQUIRED);
        // Keep the dirty marker: guarded shutdown must not bless an invalid log.
        for (unsigned n = 0; !afatfs_destroy(false); n++) {
            assert(n < 200000);
        }
        assert(writes == 0 && !io.pending);
    }
    const backendFailure_e failures[] = {FAIL_READ_CALLBACK, FAIL_READ_SUBMIT, FAIL_WRITE_CALLBACK, FAIL_WRITE_SUBMIT};
    for (unsigned f = 0; f < sizeof(failures) / sizeof(failures[0]); f++) {
        memcpy(media, interrupted, imageBytes);
        backendFailure.kind = failures[f];
        backendFailure.hits = 0;
        backendFailure.journalSector = journalSector;
        backendFailure.journalSectors = get32(interrupted + journalSector * SECTOR + 36);
        if (failures[f] == FAIL_READ_CALLBACK || failures[f] == FAIL_READ_SUBMIT) {
            backendFailure.target = TARGET_JOURNAL_READ;
        } else {
            backendFailure.target = TARGET_PREFETCH;
            backendFailure.sector = recovery[0].sector; // The first metadata sector restored by replay.
        }
        afatfs_init();
        for (unsigned n = 0; backendFailure.hits < 3; n++) {
            assert(n < 200000);
            assert(!afatfs_destroy(false)); // Must not discard an in-progress recovery.
            assert(afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_INITIALIZATION);
            assert(!afatfs_isIdle() && !afatfs_canPowerOff());
            pump();
        }
        backendFailure.kind = FAIL_NONE;
        for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; n++) {
            assert(n < 200000 && !afatfs_canPowerOff());
            pump();
        }
        assert(!afatfs_isReadOnly());
        file = openFile("existing.bin", "r");
        assert(file);
        readBytes(file, data, 700);
        for (unsigned n = 0; n < 700; n++) {
            assert(data[n] == (n < 500 ? 0xAB : 0));
        }
        closeFile(file);
        unmount();
    }
    printf(
        "exFAT interrupted recovery: %u cuts, 3 damaged-log guards and 4 backend failure/retry cases passed\n", cuts);
}

static void exerciseFatMediaRemoval(void)
{
    for (unsigned point = 0; point < 2; point++) {
        mount();
        uint8_t expectedSource[2000];
        uint8_t data[2000];
        afatfsFilePtr_t source = openFile("PWRREAD.BIN", "r");
        assert(source);
        readBytes(source, expectedSource, sizeof(expectedSource));
        closeFile(source);
        afatfsFilePtr_t writer = openFile("FMEDIA.BIN", "w");
        assert(writer);
        memset(data, 0x77, sizeof(data));
        writeBytes(writer, data, sizeof(data));
        closeFile(writer);
        unmount();
        mount();
        source = openFile("PWRREAD.BIN", "r");
        assert(source);
        writer = openFile("FMEDIA.BIN", "r+");
        assert(writer);
        memset(data, 0x69, sizeof(data));
        unsigned count = point ? SECTOR : 16;
        unsigned accepted = 0;
        for (unsigned n = 0; accepted < count; n++) {
            assert(n < 200000);
            accepted += afatfs_fwrite(writer, data + accepted, count - accepted);
            if (accepted < count) {
                pump();
            }
        }
        if (!point) {
            for (unsigned n = 0; !afatfs_flush(); n++) {
                assert(n < 200000);
                pump();
            }
            afatfs_fprefetch(source);
            assert(io.pending && !io.write);
        } else {
            for (unsigned n = 0; !(io.pending && io.write && io.writeSnapshot[0] == 0x69); n++) {
                assert(n < 200000);
                pump();
            }
        }
        retiredCompletion_t retired = removeTestCard(point ? 1 : 0, writer);
        mountReinsertedCard(&retired);
        source = openFile("PWRREAD.BIN", "r");
        assert(source);
        readBytes(source, data, sizeof(data));
        assert(!memcmp(data, expectedSource, sizeof(data)));
        closeFile(source);
        writer = openFile("FMEDIA.BIN", "r");
        assert(writer);
        uint32_t size;
        assert(afatfs_fGetSize(writer, &size) && size == sizeof(data));
        readBytes(writer, data, sizeof(data));
        for (unsigned n = 0; n < sizeof(data); n++) {
            assert(data[n] == (point && n < 256 ? 0x69 : 0x77));
        }
        closeFile(writer);
        unmount();
    }
    puts("FAT media removal: 2 pending prefetch/data-write reinsertion cases passed");
}

static afatfsError_e apiResult;
static void apiDone(afatfsError_e result, void *user)
{
    assert(user == &callbackContext);
    apiResult = result;
    completed = true;
}
static void waitRequest(afatfsRequest_t *request, afatfsError_e expected)
{
    for (unsigned n = 0; !request->complete; n++) {
        assert(n < 1000000 && !afatfs_canPowerOff() && !afatfs_isIdle());
        afatfs_poll();
        assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
    }
    assert(completed && apiResult == expected && request->result == expected);
}
#define API_TEST(call, expected)                                                                                       \
    do {                                                                                                               \
        completed = false;                                                                                             \
        assert(call);                                                                                                  \
        waitRequest(&request, expected);                                                                               \
    } while (0)
static uint32_t forwarded;
static unsigned forwardPauses;
static uint32_t consumeBytes(const uint8_t *data, uint32_t size, void *user)
{
    assert(user == &callbackContext);
    if (forwardPauses) {
        forwardPauses--;
        return 0;
    }
    uint32_t count = size < 37 ? size : 37;
    for (unsigned n = 0; n < count; n++) {
        assert(data[n] == (uint8_t)((forwarded + n) * 13));
    }
    forwarded += count;
    return count;
}
static void exerciseNewApi(bool exfat)
{
    afatfsRequest_t request;
    afatfsFileInfo_t info;
    afatfsVolumeInfo_t volume;
    afatfsDirectory_t dir;
    uint8_t data[4097];
    for (unsigned n = 0; n < sizeof(data); n++) {
        data[n] = (uint8_t)(n * 13);
    }
    afatfsFilePtr_t file;
    API_TEST(afatfs_open(&request, "RESULT.BIN", "x+", &file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(file);
    API_TEST(afatfs_write(&request, file, data, sizeof(data), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(request.transferred == sizeof(data));
    API_TEST(afatfs_sync(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!io.pending);
    API_TEST(afatfs_truncate(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    completed = false;
    assert(afatfs_fseek(file, 1024, AFATFS_SEEK_SET, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    API_TEST(afatfs_truncate(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    completed = false;
    assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    uint8_t actual[4097];
    API_TEST(afatfs_read(&request, file, actual, sizeof(actual), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(request.transferred == 1024 && !memcmp(actual, data, 1024));
    API_TEST(afatfs_close(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "RESULT.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    if (exfat) {
        API_TEST(afatfs_chmod(&request, "AFATFS.JNL", 0, FAT_FILE_ATTRIBUTE_READ_ONLY, apiDone, &callbackContext),
            AFATFS_ERROR_DENIED);
    } else if (afatfs_getContiguousFreeSpace()) {
        API_TEST(afatfs_chmod(&request, "FREESPAC.E", 0, FAT_FILE_ATTRIBUTE_READ_ONLY, apiDone, &callbackContext),
            AFATFS_ERROR_DENIED);
    }
    file = openFile("TCLIP.BIN", "x+");
    assert(file);
    assert(!openFile("TCLIP.BIN", "x"));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    API_TEST(afatfs_utime(&request, "TCLIP.BIN", 0x58221883, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_chmod(&request, "TCLIP.BIN", 0, FAT_FILE_ATTRIBUTE_ARCHIVE, apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    file = openFile("TCLIP.BIN", "r+");
    assert(file);
    const unsigned targets[] = {4097, 4096, 2048, 1537, 1024, 513, 512, 1, 0};
    for (unsigned n = 0; n < sizeof(targets) / sizeof(targets[0]); n++) {
        completed = false;
        assert(afatfs_fseek(file, (int32_t)targets[n], AFATFS_SEEK_SET, fileDone, &callbackContext) !=
            AFATFS_OPERATION_FAILURE);
        waitDone();
        completed = false;
        assert(afatfs_ftruncate(file, fileDone, &callbackContext));
        waitDone();
        uint32_t value;
        assert(afatfs_fGetSize(file, &value) && value == targets[n]);
        assert(afatfs_ftell(file, &value) && value == targets[n]);
        if (n == 1) {
            assert(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS);
            assert(info.modified != 0x58221883 && (info.attributes & FAT_FILE_ATTRIBUTE_ARCHIVE));
        }
        if (targets[n]) {
            completed = false;
            assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
            waitDone();
            readBytes(file, actual, targets[n]);
            assert(!memcmp(actual, data, targets[n]));
            while (!afatfs_fGetSize(file, &value)) {
                pump();
            }
        }
    }
    writeBytes(file, data, 1200);
    closeFile(file);
    file = openFile("CONTIG.BIN", "as");
    assert(file);
    writeBytes(file, data, sizeof(data));
    completed = false;
    assert(afatfs_fseek(file, 513, AFATFS_SEEK_SET, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    completed = false;
    assert(afatfs_ftruncate(file, fileDone, &callbackContext));
    waitDone();
    uint32_t clipped;
    assert(afatfs_fGetSize(file, &clipped) && clipped == 513);
    closeFile(file);
    file = openFile("SECOND.BIN", "as");
    assert(file);
    closeFile(file);
    API_TEST(afatfs_unlink(&request, "CONTIG.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "SECOND.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_stat(&request, "TCLIP.BIN", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(info.size == 1200 && !strcmp(info.name, "TCLIP.BIN"));
    API_TEST(afatfs_utime(&request, "TCLIP.BIN", 0x58221883, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_chmod(&request, "TCLIP.BIN", FAT_FILE_ATTRIBUTE_READ_ONLY, FAT_FILE_ATTRIBUTE_READ_ONLY, apiDone,
                 &callbackContext),
        AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "TCLIP.BIN", apiDone, &callbackContext), AFATFS_ERROR_DENIED);
    API_TEST(afatfs_rename(&request, "TCLIP.BIN", "RENAMED.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_stat(&request, "RENAMED.BIN", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(info.size == 1200 && info.modified == 0x58221883 && (info.attributes & FAT_FILE_ATTRIBUTE_READ_ONLY));
    API_TEST(afatfs_stat(&request, "TCLIP.BIN", &info, apiDone, &callbackContext), AFATFS_ERROR_NOT_FOUND);
    API_TEST(afatfs_chmod(&request, "RENAMED.BIN", 0, FAT_FILE_ATTRIBUTE_READ_ONLY, apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    file = openFile("RENAMED.BIN", "r");
    assert(file);
    forwarded = 0;
    forwardPauses = 3;
    API_TEST(afatfs_forward(&request, file, consumeBytes, &callbackContext, 2000, apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    assert(forwarded == 1200 && request.transferred == 1200);
    closeFile(file);
    completed = false;
    assert(afatfs_mkdir("RENAMED.BIN", fileDone, &callbackContext));
    waitDone();
    assert(!opened);
    API_TEST(afatfs_opendir(&request, "RENAMED.BIN", &dir, apiDone, &callbackContext), AFATFS_ERROR_DENIED);
    API_TEST(afatfs_findfirst(&request, &dir, &info, ".", "REN*.?IN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, "RENAMED.BIN"));
    afatfsOperationStatus_e status;
    for (unsigned n = 0; (status = afatfs_findnext(&dir, &info)) == AFATFS_OPERATION_IN_PROGRESS; n++) {
        assert(n < 200000);
        pump();
    }
    assert(status == AFATFS_OPERATION_SUCCESS && !info.name[0]);
    assert(afatfs_rewinddir(&dir) == AFATFS_OPERATION_SUCCESS);
    assert(afatfs_closedir(&dir) == AFATFS_OPERATION_SUCCESS);
    API_TEST(afatfs_getlabel(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_setlabel(&request, "APITEST", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_getlabel(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(volume.label, "APITEST"));
    API_TEST(afatfs_setlabel(&request, "", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    file = openFile("ALLOC.BIN", "w+");
    assert(file);
    API_TEST(afatfs_getfree(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    uint32_t before = volume.freeClusters;
    API_TEST(afatfs_expand(&request, file, 1537, false, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    uint32_t size;
    assert(afatfs_fGetSize(file, &size) && size == 0);
    API_TEST(afatfs_expand(&request, file, 1537, true, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && info.size == 1537);
    if (exfat) {
        readBytes(file, actual, 1537);
        for (unsigned n = 0; n < 1537; n++) {
            assert(!actual[n]);
        }
        completed = false;
        assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
        waitDone();
    }
    writeBytes(file, data, 1200);
    completed = false;
    assert(afatfs_fseek(file, 512, AFATFS_SEEK_SET, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    completed = false;
    assert(afatfs_ftruncate(file, fileDone, &callbackContext));
    waitDone();
    assert(afatfs_fGetSize(file, &size) && size == 512);
    completed = false;
    assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    readBytes(file, actual, 512);
    assert(!memcmp(actual, data, 512));
    while (!afatfs_fGetSize(file, &size)) {
        pump();
    }
    completed = false;
    assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    completed = false;
    assert(afatfs_ftruncate(file, fileDone, &callbackContext));
    waitDone();
    API_TEST(afatfs_getfree(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(volume.freeClusters == before);
    char workspace[128];
    API_TEST(
        afatfs_printf(&request, file, workspace, sizeof(workspace), apiDone, &callbackContext, "%s %d\n", "hello", 42),
        AFATFS_ERROR_NONE);
    API_TEST(afatfs_puts(&request, file, "second\n", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    completed = false;
    assert(afatfs_rewind(file, fileDone, &callbackContext) != AFATFS_OPERATION_FAILURE);
    waitDone();
    API_TEST(afatfs_gets(&request, file, workspace, sizeof(workspace), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(workspace, "hello 42\n"));
    API_TEST(afatfs_gets(&request, file, workspace, sizeof(workspace), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(workspace, "second\n"));
    closeFile(file);
    completed = false;
    assert(afatfs_mkdir("APIDIR", fileDone, &callbackContext));
    waitDone();
    afatfsFilePtr_t directory = opened;
    assert(directory && afatfs_chdir(directory));
    closeFile(directory);
    file = openFile("CHILD.BIN", "w");
    assert(file);
    writeBytes(file, data, 1);
    closeFile(file);
    char path[128];
    API_TEST(afatfs_getcwd(&request, path, sizeof(path), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(path, "/APIDIR"));
    assert(afatfs_chdir(NULL));
    API_TEST(afatfs_rmdir(&request, "APIDIR", apiDone, &callbackContext), AFATFS_ERROR_NOT_EMPTY);
    API_TEST(afatfs_opendir(&request, "APIDIR", &dir, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_rename(&request, "APIDIR", "APIMOVED", apiDone, &callbackContext), AFATFS_ERROR_DENIED);
    API_TEST(afatfs_rmdir(&request, "APIDIR", apiDone, &callbackContext), AFATFS_ERROR_DENIED);
    assert(afatfs_closedir(&dir) == AFATFS_OPERATION_SUCCESS);
    API_TEST(afatfs_rename(&request, "APIDIR", "APIMOVED", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_rename(&request, "APIMOVED", "APIDIR", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    completed = false;
    assert(afatfs_mkdir("APIDIR", fileDone, &callbackContext));
    waitDone();
    directory = opened;
    assert(afatfs_chdir(directory));
    closeFile(directory);
    API_TEST(afatfs_unlink(&request, "CHILD.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(afatfs_chdir(NULL));
    API_TEST(afatfs_rmdir(&request, "APIDIR", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "RENAMED.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "ALLOC.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(afatfs_chdrive(0) == AFATFS_OPERATION_SUCCESS && afatfs_chdrive(1) == AFATFS_OPERATION_FAILURE);
    assert(afatfs_setcp(20127) == AFATFS_OPERATION_SUCCESS && afatfs_setcp(65001) == AFATFS_OPERATION_FAILURE);
    printf(
        "extended API (%s): truncate, metadata, rename, directories, labels, free space, expand and streams passed\n",
        exfat ? "exFAT" : "FAT");
}

static void exerciseAccessProtection(bool exfat)
{
    uint8_t data[700];
    memset(data, 0x39, sizeof(data));
    afatfsFilePtr_t file = openFile("GUARD.BIN", "w");
    assert(file);
    writeBytes(file, data, sizeof(data));
    closeFile(file);

    file = openFile("guard.bin", "r");
    assert(file);
    unsigned before = submittedWrites;
    completed = false;
    assert(!afatfs_ftruncate(file, fileDone, &callbackContext));
    assert(!afatfs_funlink(file, done, &callbackContext));
    assert(afatfs_fwrite(file, data, 1) == 0);
    afatfs_fputc(file, 0xFF);
    uint32_t size;
    uint32_t position;
    assert(!completed && submittedWrites == before);
    assert(afatfs_fGetSize(file, &size) && size == sizeof(data));
    assert(afatfs_ftell(file, &position) && position == 0);
    afatfsFilePtr_t reader = openFile("GUARD.BIN", "r");
    assert(reader && reader != file);
    assert(!openFile("GUARD.BIN", "r+"));
    closeFile(reader);
    assert(!openFile("GUARD.BIN", "w"));
    closeFile(file);

    const char *modes[] = {"r+", "w+", "a", "a+", "as"};
    for (unsigned n = 0; n < sizeof(modes) / sizeof(modes[0]); n++) {
        file = openFile("GUARD.BIN", modes[n]);
        assert(file);
        assert(!openFile("guard.bin", "r"));
        assert(!openFile("guard.bin", "r+"));
        assert(!openFile("guard.bin", "w"));
        assert(!openFile("guard.bin", "a"));
        closeFile(file);
    }

    // Queue both requests before servicing delayed I/O, for existing and newly created files.
    const char *names[] = {"GUARD.BIN", "PENDING.BIN"};
    for (unsigned n = 0; n < 2; n++) {
        pendingOpen_t a = {0};
        pendingOpen_t b = {0};
        holdIO = true;
        assert(afatfs_fopen(names[n], "a", concurrentOpen, &a));
        assert(afatfs_fopen(names[n], "a", concurrentOpen, &b));
        holdIO = false;
        for (unsigned p = 0; !a.done || !b.done; p++) {
            assert(p < 200000);
            pump();
        }
        assert((a.file != NULL) != (b.file != NULL));
        closeFile(a.file ? a.file : b.file);
    }
    // The same name in a different directory is a different file.
    completed = false;
    assert(afatfs_mkdir("GUARDDIR", fileDone, &callbackContext));
    waitDone();
    afatfsFilePtr_t directory = opened;
    assert(directory && afatfs_chdir(directory));
    closeFile(directory);
    file = openFile("GUARD.BIN", "w");
    assert(file);
    assert(afatfs_chdir(NULL));
    reader = openFile("GUARD.BIN", "r");
    assert(reader);
    closeFile(reader);
    closeFile(file);
    unmount();

    // Set the on-disk attribute externally, then mount with an empty cache.
    uint8_t *guard = NULL;
    for (unsigned offset = 0; offset + 96 <= sizeof(media); offset += 32) {
        uint8_t *entry = media + offset;
        if (exfat && (entry < cluster(5) || entry >= cluster(5) + SECTOR)) {
            continue;
        }
        if (!exfat && !memcmp(entry, "GUARD   BIN", 11)) {
            guard = entry;
            break;
        }
        if (exfat && entry[0] == EXFAT_DIR_FILEDIR && entry[1] == 2 && entry[32] == EXFAT_DIR_STREAM &&
            entry[35] == 9 && entry[64] == EXFAT_DIR_FILENAME) {
            bool match = true;
            for (unsigned j = 0; j < 9; j++) {
                if (entry[66 + j * 2] != (uint8_t)"GUARD.BIN"[j] || entry[67 + j * 2]) {
                    match = false;
                }
            }
            if (match) {
                guard = entry;
                break;
            }
        }
    }
    assert(guard);
    if (exfat) {
        ((exFatFileEntry_t *)guard)->fileAttirb |= FAT_FILE_ATTRIBUTE_READ_ONLY;
        ((exFatFileEntry_t *)guard)->checksum = checksum(guard, 96);
    } else {
        ((fatDirectoryEntry_t *)guard)->attrib |= FAT_FILE_ATTRIBUTE_READ_ONLY;
    }
    mount();
    const char *denied[] = {"r+", "w", "w+", "a", "a+", "as", "ws"};
    before = submittedWrites;
    for (unsigned n = 0; n < sizeof(denied) / sizeof(denied[0]); n++) {
        assert(!openFile("GUARD.BIN", denied[n]));
    }
    assert(submittedWrites == before);
    file = openFile("GUARD.BIN", "r");
    assert(file);
    assert(!afatfs_ftruncate(file, NULL, NULL) && !afatfs_funlink(file, NULL, NULL));
    assert(afatfs_fGetSize(file, &size) && size == sizeof(data));
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x39);
    }
    closeFile(file);
    printf("access protection (%s): read-only mode/attribute, shared readers and exclusive writers passed\n",
        exfat ? "exFAT" : "FAT");
}

static void exerciseCloseDurability(void)
{
    const backendFailure_e failures[] = {FAIL_NONE, FAIL_WRITE_SUBMIT, FAIL_WRITE_CALLBACK, FAIL_END_WRITE};
    for (unsigned f = 0; f < sizeof(failures) / sizeof(failures[0]); f++) {
        afatfsFilePtr_t file = openFile("CLOSE.BIN", "w");
        assert(file);
        uint8_t data[777];
        memset(data, 0xD3, sizeof(data));
        writeBytes(file, data, sizeof(data));
        backendFailure.kind = failures[f];
        backendFailure.target = failures[f] == FAIL_END_WRITE ? TARGET_STOP : TARGET_DATA;
        backendFailure.hits = 0;
        holdIO = true;
        completed = false;
        assert(afatfs_fclose(file, done, &callbackContext));
        pendingOpen_t conflict = {0};
        assert(afatfs_fopen("CLOSE.BIN", "r+", concurrentOpen, &conflict));
        for (unsigned p = 0; p < 32; p++) {
            pump();
            assert(!completed && !afatfs_canPowerOff());
        }
        assert(conflict.done && !conflict.file);
        holdIO = false;
        if (failures[f] != FAIL_NONE) {
            for (unsigned p = 0; backendFailure.hits < 3; p++) {
                assert(p < 200000);
                pump();
                assert(!completed && !afatfs_canPowerOff());
            }
            for (unsigned p = 0; p < 32; p++) {
                pump();
                assert(!completed);
            }
        }
        backendFailure.kind = FAIL_NONE;
        waitDone();
        assert(!io.pending && !afatfs_canPowerOff());
        // Drop all RAM state immediately after the close callback, without destroy/flush.
        afatfs_notifyMediaRemoved();
        mount();
        file = openFile("CLOSE.BIN", "r");
        assert(file);
        uint32_t size;
        assert(afatfs_fGetSize(file, &size) && size == sizeof(data));
        memset(data, 0, sizeof(data));
        readBytes(file, data, sizeof(data));
        for (unsigned n = 0; n < sizeof(data); n++) {
            assert(data[n] == 0xD3);
        }
        closeFile(file);
    }
    puts("close durability: held I/O, failed submission/ACK/end-write and immediate remount passed");
}

static void exerciseFat(bool fat32, unsigned sectorsPerCluster)
{
    memset(media, 0, sizeof(media));
    memset(&io, 0, sizeof(io));
    unsigned count = fat32 ? 65530 : 9000;
    unsigned fatSectors = ((count + 2) * (fat32 ? 4 : 2) + 511) / 512;
    unsigned rootSectors = fat32 ? 0 : 32;
    unsigned heap = PARTITION + 1 + fatSectors * 2 + rootSectors;
    mbrPartitionEntry_t *part = (mbrPartitionEntry_t *)(media + 446);
    part->type = fat32 ? MBR_PARTITION_TYPE_FAT32 : MBR_PARTITION_TYPE_FAT16;
    part->lbaBegin = PARTITION;
    part->numSectors = heap - PARTITION + count * sectorsPerCluster;
    media[510] = 0x55;
    media[511] = 0xAA;
    uint8_t *boot = media + PARTITION * SECTOR;
    fatVolumeID_t *volume = (fatVolumeID_t *)boot;
    volume->fat.bytesPerSector = SECTOR;
    volume->fat.sectorsPerCluster = (uint8_t)sectorsPerCluster;
    volume->fat.reservedSectorCount = 1;
    volume->fat.numFATs = 2;
    volume->fat.totalSectors32 = part->numSectors;
    if (fat32) {
        volume->fat.fatDescriptor.fat32.FATSize32 = fatSectors;
        volume->fat.fatDescriptor.fat32.rootCluster = 2;
    } else {
        volume->fat.FATSize16 = (uint16_t)fatSectors;
        volume->fat.rootEntryCount = 512;
    }
    boot[510] = 0x55;
    boot[511] = 0xAA;
    for (unsigned n = 0; n < 2; n++) {
        uint8_t *table = media + (PARTITION + 1 + n * fatSectors) * SECTOR;
        if (fat32) {
            put32(table, 0x0FFFFFF8);
            put32(table + 4, 0x0FFFFFFF);
            put32(table + 8, 0x0FFFFFFF);
        } else {
            table[0] = 0xF8;
            table[1] = table[2] = table[3] = 0xFF;
        }
    }
    mount();
    exerciseNewApi(false);
    exerciseAccessProtection(false);
    exerciseCloseDurability();
    uint8_t data[2000];
    for (unsigned n = 0; n < sizeof(data); n++) {
        data[n] = (uint8_t)(n * 13);
    }
    afatfsFilePtr_t file = openFile("REGRESS.TXT", "w");
    assert(file);
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    mount();
    file = openFile("regress.txt", "r");
    assert(file);
    if (sectorsPerCluster == 4) {
        uint32_t position;
        assert(!io.pending && afatfs_ftell(file, &position));
        afatfs_fprefetch(file);
        assert(io.pending && !io.write);
        uint32_t firstSector = io.block;
        afatfs_fprefetch(file); // A repeated hint must not replace the pending read.
        assert(io.pending && io.block == firstSector);
        assert(afatfs_ftell(file, &position) && position == 0);
        while (io.pending) {
            pump();
        }
        uint32_t reusable = afatfs_getFreeBufferSpace();
        assert(afatfs_fread(file, data, 1) == 1);
        assert(afatfs_getFreeBufferSpace() == reusable - SECTOR); // Retained reader.
        for (unsigned n = 0; n < 3; n++) {
            unsigned alreadyRead = n == 0 ? 1 : 0;
            assert(afatfs_fread(file, data + n * SECTOR + alreadyRead, SECTOR - alreadyRead) == SECTOR - alreadyRead);
            assert(io.pending && !io.write && io.block == firstSector + n + 1);
            assert(afatfs_getFreeBufferSpace() == reusable - SECTOR); // Pending prefetch.
            // The handle can be idle while its speculative backend read is pending.
            assert(afatfs_ftell(file, &position) && position == (n + 1) * SECTOR);
            while (io.pending) {
                pump();
            }
        }
        assert(afatfs_fread(file, data + 3 * SECTOR, sizeof(data) - 3 * SECTOR) == sizeof(data) - 3 * SECTOR);
        assert(!io.pending);
        afatfs_fprefetch(file);
        assert(!io.pending); // EOF: no speculative read.
        for (unsigned n = 0; n < sizeof(data); n++) {
            assert(data[n] == (uint8_t)(n * 13));
        }
        assert(afatfs_fseek(file, 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_SUCCESS);
        puts("prefetch automatic sector release, unchanged cursor and EOF: passed");
    }
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == (uint8_t)(n * 13));
    }
    if (sectorsPerCluster == 4) {
        closeFile(file);
        // Start with a cold cache and a read that outlives the close operations.
        unmount();
        mount();
        file = openFile("regress.txt", "r");
        assert(file);
        unsigned savedDelay = ioDelay;
        ioDelay = 100;
        assert(afatfs_isIdle() && !afatfs_canPowerOff());
        uint32_t position;
        afatfs_fprefetch(file);
        assert(io.pending && afatfs_ftell(file, &position));
        unsigned pendingDelay = io.delay;
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(io.delay == pendingDelay); // Queries must not poll or advance I/O.
        endWriteBusy = 3;
        assert(!afatfs_destroy(false));
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(io.pending);
        bool sawStopping = false;
        for (unsigned n = 0; !afatfs_destroy(false); n++) {
            assert(n < 200000);
            assert(!afatfs_canPowerOff());
            if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_STOPPING) {
                sawStopping = true;
                assert(!afatfs_isIdle());
            }
        }
        assert(sawStopping && !endWriteBusy);
        assert(!io.pending);
        assert(afatfs_isIdle() && afatfs_canPowerOff());
        ioDelay = savedDelay;
        mount();
        file = openFile("regress.txt", "r");
        assert(file);
        puts("shutdown waits for pending prefetch: passed");
        closeFile(file);
        file = openFile("regress.txt", "r+");
        assert(file);
        uint32_t available = afatfs_getFreeBufferSpace();
        assert(afatfs_getFileFreeBufferSpace(file) == available);
        for (unsigned n = 0; !afatfs_fwrite(file, data, 1); n++) {
            assert(n < 200000);
            pump();
        }
        assert(afatfs_getFreeBufferSpace() == available - 1);
        assert(afatfs_fwrite(file, data + 1, 36) == 36);
        assert(afatfs_getFreeBufferSpace() == available - 37);
        assert(afatfs_getFileFreeBufferSpace(file) == available - 37);
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
        assert(afatfs_fwrite(file, data + 37, SECTOR - 37) == SECTOR - 37);
        assert(afatfs_getFreeBufferSpace() == available - SECTOR); // Unlocked dirty buffer.
        assert(afatfs_getFileFreeBufferSpace(file) == available - SECTOR);
        for (unsigned n = 0; !afatfs_flush(); n++) {
            assert(n < 200000);
            pump();
        }
        assert(afatfs_getFreeBufferSpace() == available);
        assert(afatfs_getFileFreeBufferSpace(file) == available);
        puts("buffer space retained readers, pending reads, partial writes and flush: passed");
    }
    closeFile(file);
    file = openFile("regress.txt", "w");
    assert(file);
    closeFile(file);
    assert(afatfs_getFileFreeBufferSpace(file) == 0);
    file = openFile("regress.txt", "r+");
    assert(file);
    completed = false;
    assert(afatfs_funlink(file, done, &callbackContext));
    waitDone();
    assert(!openFile("regress.txt", "r"));
    completed = false;
    assert(afatfs_mkdir("SUBDIR", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    afatfsFilePtr_t dir = opened;
    assert(afatfs_getFileFreeBufferSpace(dir) == 0);
    assert(afatfs_chdir(dir));
    closeFile(dir);
    file = openFile("CHILD.TXT", "w");
    assert(file);
    writeBytes(file, (const uint8_t *)"hello", 5);
    closeFile(file);
    assert(afatfs_chdir(NULL));
    unmount();
    mount();
    completed = false;
    assert(afatfs_mkdir("SUBDIR", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    dir = opened;
    assert(afatfs_chdir(dir));
    closeFile(dir);
    file = openFile("CHILD.TXT", "r");
    assert(file);
    readBytes(file, data, 5);
    assert(memcmp(data, "hello", 5) == 0);
    closeFile(file);
    assert(afatfs_chdir(NULL));
    unmount();
    mount();
    file = openFile("PWRREAD.BIN", "w");
    assert(file);
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    mount();
    exerciseConcurrentPowerOff("PWRREAD.BIN", 0, 0);
    mount();
    exerciseBackendFailure(FAIL_WRITE_CALLBACK, TARGET_DATA, 0, 0);
    mount();
    exerciseBackendFailure(FAIL_WRITE_SUBMIT, TARGET_DATA, 0, 0);
    mount();
    exerciseBackendFailure(FAIL_END_WRITE, TARGET_STOP, 0, 0);
    mount();
    exerciseFailedPrefetch("PWRREAD.BIN");
    exerciseFatMediaRemoval();
    printf("FAT%u regression (%u sectors/cluster): passed\n", fat32 ? 32 : 16, sectorsPerCluster);
}
#define GPT_START 2048U
#define GPT_LAST ((uint32_t)(sizeof(media) / SECTOR - 1))
static uint32_t gptCRC(const uint8_t *bytes, unsigned length)
{
    uint32_t value = UINT32_MAX;
    for (unsigned n = 0; n < length; n++) {
        value ^= bytes[n];
        for (unsigned bit = 0; bit < 8; bit++) {
            value = value & 1 ? (value >> 1) ^ 0xEDB88320U : value >> 1;
        }
    }
    return value ^ UINT32_MAX;
}
static void gptHeaderCRC(uint8_t *header)
{
    put32(header + 16, 0);
    put32(header + 16, gptCRC(header, get32(header + 12)));
}
static void gptRefresh(void)
{
    const uint32_t locations[] = {1, GPT_LAST};
    for (unsigned n = 0; n < 2; n++) {
        uint8_t *header = media + locations[n] * SECTOR;
        put32(header + 88, gptCRC(media + get32(header + 72) * SECTOR, get32(header + 80) * get32(header + 84)));
        gptHeaderCRC(header);
    }
}
static void formatGPT(unsigned entrySize, unsigned count, unsigned tableLBA, bool readOnly)
{
    format(false);
    memmove(media + GPT_START * SECTOR, media + PARTITION * SECTOR, VOLUME_SECTORS * SECTOR);
    memset(media, 0, GPT_START * SECTOR);
    uint8_t *boot = media + GPT_START * SECTOR;
    ((fatVolumeID_t *)boot)->exFat.partitionOffset = GPT_START;
    uint32_t sum = 0;
    for (unsigned n = 0; n < 11 * SECTOR; n++) {
        if (n != 106 && n != 107 && n != 112) {
            sum = (sum >> 1 | sum << 31) + boot[n];
        }
    }
    for (unsigned n = 0; n < SECTOR; n += 4) {
        put32(boot + 11 * SECTOR + n, sum);
    }
    memcpy(boot + 12 * SECTOR, boot, 12 * SECTOR);
    mbrPartitionEntry_t *pmbr = (mbrPartitionEntry_t *)(media + 446);
    pmbr->type = MBR_PARTITION_TYPE_GPT;
    pmbr->lbaBegin = 1;
    pmbr->numSectors = GPT_LAST;
    media[510] = 0x55;
    media[511] = 0xAA;
    unsigned bytes = entrySize * count;
    unsigned sectors = (bytes + 511) / 512;
    if (sectors < 32) {
        sectors = 32;
    }
    uint8_t *table = media + tableLBA * SECTOR;
    static const uint8_t basic[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    // A reserved partition and an NTFS Basic Data partition precede exFAT.
    table[0] = 0x19;
    table[16] = 1;
    put32(table + 32, tableLBA + sectors);
    put32(table + 40, tableLBA + sectors + 15);
    uint8_t *ntfs = table + entrySize;
    memcpy(ntfs, basic, 16);
    ntfs[16] = 2;
    put32(ntfs + 32, tableLBA + sectors + 16);
    put32(ntfs + 40, tableLBA + sectors + 31);
    memcpy(media + (tableLBA + sectors + 16) * SECTOR + 3, "NTFS    ", 8);
    uint8_t *entry = table + (count - 1) * entrySize;
    memcpy(entry, basic, 16);
    entry[16] = 3;
    put32(entry + 32, GPT_START);
    put32(entry + 40, GPT_START + VOLUME_SECTORS - 1);
    if (readOnly) {
        entry[55] = 0x10;
    }
    memcpy(media + (GPT_LAST - sectors) * SECTOR, table, bytes);
    const uint32_t locations[] = {1, GPT_LAST};
    for (unsigned n = 0; n < 2; n++) {
        uint8_t *header = media + locations[n] * SECTOR;
        memcpy(header, "EFI PART", 8);
        put32(header + 8, 0x10000);
        put32(header + 12, 92);
        put32(header + 24, locations[n]);
        put32(header + 32, locations[1 - n]);
        put32(header + 40, tableLBA + sectors);
        put32(header + 48, GPT_LAST - sectors - 1);
        header[56] = 0x73;
        put32(header + 72, n ? GPT_LAST - sectors : tableLBA);
        put32(header + 80, count);
        put32(header + 84, entrySize);
    }
    gptRefresh();
}
static void checkGPTWorkload(void)
{
    uint8_t *prefix = malloc(GPT_START * SECTOR);
    uint8_t *backup = malloc(257 * SECTOR);
    assert(prefix && backup);
    memcpy(prefix, media, GPT_START * SECTOR);
    memcpy(backup, media + (GPT_LAST - 256) * SECTOR, 257 * SECTOR);
    mount();
    assert(!afatfs_isReadOnly());
    afatfsFilePtr_t file = openFile("existing.bin", "r");
    assert(file);
    uint8_t data[1200];
    readBytes(file, data, 700);
    for (unsigned n = 0; n < 700; n++) {
        assert(data[n] == (n < 500 ? 0xAB : 0));
    }
    closeFile(file);
    file = openFile("GPT write.bin", "w");
    assert(file);
    memset(data, 0x65, sizeof(data));
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    assert(!(media[GPT_START * SECTOR + 106] & 2));
    mount();
    file = openFile("GPT write.bin", "r");
    assert(file);
    uint32_t size;
    assert(afatfs_fGetSize(file, &size) && size == sizeof(data));
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x65);
    }
    closeFile(file);
    unmount();
    assert(!memcmp(prefix, media, GPT_START * SECTOR));
    assert(!memcmp(backup, media + (GPT_LAST - 256) * SECTOR, 257 * SECTOR));
    free(prefix);
    free(backup);
}
static void rejectGPT(void)
{
    unsigned before = submittedWrites;
    afatfs_init();
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL; n++) {
        assert(n < 200000 && afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY);
        afatfs_poll();
        assert(!afatfs_canPowerOff());
    }
    assert(submittedWrites == before && afatfs_getLastError() == AFATFS_ERROR_BAD_MBR);
    assert(!afatfs_destroy(false));
    assert(afatfs_destroy(true));
}
static void exerciseGPT(void)
{
    assert(gptCRC((const uint8_t *)"123456789", 9) == 0xCBF43926);
    const unsigned sizes[] = {128, 256, 512, 1024};
    for (unsigned n = 0; n < 4; n++) {
        formatGPT(sizes[n], 128, 2, false);
        checkGPTWorkload();
    }
    formatGPT(128, 3, 2, false);
    checkGPTWorkload(); // Few declared entries, with the reserved 16-KiB space.
    formatGPT(128, 129, 8, false);
    checkGPTWorkload(); // Non-default array LBA and partial final sector.
    formatGPT(128, 128, 2, false);
    ((mbrPartitionEntry_t *)(media + 446))->numSectors = UINT32_MAX;
    checkGPTWorkload();
    // Recovery from a damaged primary header or array does not rewrite either GPT copy.
    for (unsigned n = 0; n < 3; n++) {
        formatGPT(128, 128, 2, false);
        if (n == 0) {
            media[SECTOR] ^= 1;
        } else if (n == 1) {
            media[SECTOR + 16] ^= 1;
        } else {
            media[2 * SECTOR + 16383] ^= 1;
        }
        checkGPTWorkload();
    }
    formatGPT(128, 128, 2, false);
    media[GPT_LAST * SECTOR] ^= 1;
    checkGPTWorkload();
    // A protective MBR is authoritative even if a hybrid entry advertises a supported legacy partition.
    formatGPT(128, 128, 2, false);
    mbrPartitionEntry_t *hybrid = (mbrPartitionEntry_t *)(media + 462);
    hybrid->type = MBR_PARTITION_TYPE_EXFAT;
    hybrid->lbaBegin = 100;
    hybrid->numSectors = 10;
    checkGPTWorkload();
    for (unsigned problem = 0; problem < 13; problem++) {
        formatGPT(128, 128, 2, false);
        const uint32_t headers[] = {1, GPT_LAST};
        for (unsigned n = 0; n < 2; n++) {
            uint8_t *header = media + headers[n] * SECTOR;
            uint8_t *table = media + get32(header + 72) * SECTOR;
            uint8_t *entry = table + 127 * 128;
            switch (problem) {
                case 0:
                    header[0] ^= 1;
                    break;
                case 1:
                    table[16383] ^= 1;
                    continue; // Keep the now-invalid CRC.
                case 2:
                    put32(header + 84, 96);
                    break;
                case 3:
                    put32(header + 80, UINT32_MAX);
                    break;
                case 4:
                    put32(header + 80, 0);
                    break;
                case 5:
                    put32(header + 76, 1);
                    break; // Array LBA above the backend address range.
                case 6:
                    put32(entry + 36, 1);
                    break;
                case 7:
                    put32(entry + 40, GPT_START - 1);
                    break;
                case 8:
                    put32(entry + 32, 1);
                    break;
                case 9:
                    put32(header + 24, 99);
                    break;
                case 10:
                    put32(header + 12, 80);
                    break;
                case 11:
                    put32(header + 52, UINT32_MAX);
                    break;
                case 12:
                    put32(table + 32, GPT_START);
                    put32(table + 40, GPT_START + 2);
                    break; // Overlap.
            }
            put32(header + 88, gptCRC(table, 16384));
            gptHeaderCRC(header);
        }
        rejectGPT();
    }
    for (unsigned dirty = 0; dirty < 2; dirty++) {
        formatGPT(128, 128, 2, true);
        media[GPT_START * SECTOR + 106] |= (uint8_t)(dirty * 2);
        mount();
        assert(afatfs_isReadOnly() && submittedWrites == 0);
        afatfsFilePtr_t file = openFile("existing.bin", "r");
        assert(file);
        assert(!openFile("DENIED.BIN", "w") && !afatfs_ftruncate(file, NULL, NULL));
        closeFile(file);
        unmount();
        assert(submittedWrites == 0);
        assert((media[GPT_START * SECTOR + 106] & 2) == dirty * 2);
    }
    for (unsigned failure = FAIL_READ_CALLBACK; failure <= FAIL_READ_SUBMIT; failure += 2) {
        formatGPT(128, 128, 2, false);
        afatfs_init();
        backendFailure.kind = (backendFailure_e)failure;
        backendFailure.target = TARGET_PREFETCH;
        backendFailure.sector = 2;
        for (unsigned n = 0; backendFailure.hits < 3; n++) {
            assert(n < 200000 && !submittedWrites);
            pump();
        }
        backendFailure.kind = FAIL_NONE;
        for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; n++) {
            assert(n < 200000);
            pump();
        }
        unmount();
    }
    formatGPT(128, 128, 2, false);
    afatfs_init();
    for (unsigned n = 0; !(io.pending && io.block == 2); n++) {
        assert(n < 200000);
        pump();
    }
    retiredCompletion_t retired = removeTestCard(0, NULL);
    assert(submittedWrites == 0);
    mountReinsertedCard(&retired);
    unmount();
    puts("GPT exFAT: 12 layouts/recovery cases, 13 corruptions, read-only, I/O retry and reinsertion passed");
}

static void exerciseApiRecovery(void)
{
    format(false);
    mount();
    afatfsRequest_t request;
    uint8_t data[1200];
    memset(data, 0x68, sizeof(data));
    afatfsFilePtr_t file = openFile("APIKEEP.BIN", "w");
    assert(file);
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    unmount();
    mount();
    file = openFile("APIKEEP.BIN", "r");
    assert(file);
    afatfs_fprefetch(file);
    assert(io.pending && !io.write);
    backendFailure.kind = FAIL_READ_CALLBACK;
    backendFailure.target = TARGET_PREFETCH;
    backendFailure.sector = io.block;
    backendFailure.hits = 0;
    completed = false;
    assert(afatfs_read(&request, file, data, sizeof(data), apiDone, &callbackContext));
    for (unsigned n = 0; backendFailure.hits < 3; n++) {
        assert(n < 200000);
        afatfs_poll();
        assert(!request.complete && !request.transferred && !afatfs_canPowerOff());
    }
    backendFailure.kind = FAIL_NONE;
    waitRequest(&request, AFATFS_ERROR_NONE);
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x68);
    }
    closeFile(file);
    const backendFailure_e failures[] = {FAIL_WRITE_SUBMIT, FAIL_WRITE_CALLBACK};
    for (unsigned f = 0; f < 2; f++) {
        completed = false;
        assert(afatfs_rename(&request, "APIKEEP.BIN", "RENFAIL.BIN", apiDone, &callbackContext));
        backendFailure.kind = failures[f];
        backendFailure.target = TARGET_STOP;
        backendFailure.hits = 0;
        for (unsigned n = 0; backendFailure.hits < 3; n++) {
            assert(n < 200000);
            afatfs_poll();
            assert(!request.complete && !afatfs_canPowerOff());
        }
        backendFailure.kind = FAIL_NONE;
        waitRequest(&request, AFATFS_ERROR_NONE);
        API_TEST(afatfs_rename(&request, "RENFAIL.BIN", "APIKEEP.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    }
    completed = false;
    assert(afatfs_rename(&request, "APIKEEP.BIN", "APINEW.BIN", apiDone, &callbackContext));
    for (unsigned n = 0; !(request.phase == 4 && io.pending && io.write); n++) {
        assert(n < 200000 && !request.complete);
        afatfs_poll();
    }
    retiredCompletion_t retired = removeTestCard(2, NULL);
    assert(request.complete && request.result == AFATFS_ERROR_MEDIA_REMOVED && !completed);
    mountReinsertedCard(&retired);
    assert(!completed);
    file = openFile("APIKEEP.BIN", "r");
    assert(file);
    memset(data, 0, sizeof(data));
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == 0x68);
    }
    closeFile(file);
    afatfsFileInfo_t info;
    API_TEST(afatfs_stat(&request, "APINEW.BIN", &info, apiDone, &callbackContext), AFATFS_ERROR_NOT_FOUND);
    unmount();
    puts(
        "extended API recovery: read/write backend retries, interrupted rename, torn sector and stale callbacks passed");
}
static void exerciseBenignRename(void)
{
    format(false);
    uint8_t *set = cluster(5) + 64;
    uint8_t benign[32];
    memset(benign, 0xA7, sizeof(benign));
    benign[0] = 0xE0;
    ((exFatFileEntry_t *)set)->numOfEntries = 3;
    memcpy(set + 96, benign, 32);
    set[128] = 0;
    ((exFatFileEntry_t *)set)->checksum = checksum(set, 128);
    mount();
    afatfsRequest_t request;
    char large[256];
    memset(large, 'z', sizeof(large));
    memcpy(large + 251, ".bin", 5);
    unsigned before = submittedWrites;
    API_TEST(afatfs_rename(&request, "existing.bin", large, apiDone, &callbackContext), AFATFS_ERROR_UNSUPPORTED);
    assert(submittedWrites == before);
    char name[128];
    memset(name, 'n', sizeof(name));
    memcpy(name + 123, ".bin", 5);
    API_TEST(afatfs_rename(&request, "existing.bin", name, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    unmount();
    mount();
    afatfsFilePtr_t file = openFile(name, "r");
    assert(file);
    const exFatFileEntry_t *entry;
    const exFatStreamEntry_t *stream;
    assert(afatfs_fGetExFatFileInfo(file, &entry, &stream) && entry->numOfEntries == 11);
    assert(!memcmp((const uint8_t *)entry + entry->numOfEntries * 32, benign, 32));
    assert(stream->dataSize == 700 && stream->fileSize == 500 && (stream->flags & 2));
    uint8_t data[700];
    readBytes(file, data, sizeof(data));
    for (unsigned n = 0; n < sizeof(data); n++) {
        assert(data[n] == (n < 500 ? 0xAB : 0));
    }
    closeFile(file);
    unmount();
    puts(
        "extended API rename: benign secondary entries, NoFatChain and VDL preserved; oversized entry sets rejected without writes");
}

static void exerciseFormatApi(void)
{
    const fatFilesystemType_e types[] = {
        FAT_FILESYSTEM_TYPE_FAT16,
        FAT_FILESYSTEM_TYPE_FAT32,
#ifndef TEST_FAT_ONLY
        FAT_FILESYSTEM_TYPE_EXFAT,
        FAT_FILESYSTEM_TYPE_EXFAT,
        FAT_FILESYSTEM_TYPE_FAT16,
#endif
    };
    for (unsigned n = 0; n < sizeof(types) / sizeof(types[0]); n++) {
        assert(afatfs_canPowerOff());
        afatfsRequest_t request;
        afatfsPartitionOptions_t partitions;
        memset(&partitions, 0, sizeof(partitions));
        partitions.gpt = n == 3;
        partitions.sectorCount = (uint32_t)(sizeof(media) / SECTOR);
        partitions.start[0] = 2048;
        partitions.length[0] = partitions.sectorCount - 2048 - 33;
        partitions.type[0] = types[n] == FAT_FILESYSTEM_TYPE_EXFAT ? MBR_PARTITION_TYPE_EXFAT
            : types[n] == FAT_FILESYSTEM_TYPE_FAT32                ? MBR_PARTITION_TYPE_FAT32
                                                                   : MBR_PARTITION_TYPE_FAT16;
        partitions.diskGuid[0] = 0x52;
        partitions.diskGuid[15] = 0xC1;
        completed = false;
        holdIO = true;
        assert(afatfs_fdisk(&request, &partitions, apiDone, &callbackContext));
        for (unsigned p = 0; p < 16; p++) {
            afatfs_poll();
            assert(!request.complete && !afatfs_isIdle() && !afatfs_canPowerOff());
        }
        holdIO = false;
        if (!n) {
            backendFailure.kind = FAIL_WRITE_CALLBACK;
            backendFailure.target = TARGET_STOP;
            backendFailure.hits = 0;
            for (unsigned p = 0; backendFailure.hits < 3; p++) {
                assert(p < 200000);
                afatfs_poll();
                assert(!request.complete && !afatfs_canPowerOff());
            }
            backendFailure.kind = FAIL_NONE;
        }
        waitRequest(&request, AFATFS_ERROR_NONE);
        assert(afatfs_canPowerOff());
        if (!partitions.gpt) {
            assert(memcmp(media + SECTOR, "EFI PART", 8) &&
                memcmp(media + (partitions.sectorCount - 1) * SECTOR, "EFI PART", 8));
        }
        afatfsFormatOptions_t options = {types[n], partitions.start[0], partitions.length[0], 0, 0x12345678};
        completed = false;
        assert(afatfs_mkfs(&request, &options, apiDone, &callbackContext));
        if (!n) {
            backendFailure.kind = FAIL_WRITE_SUBMIT;
            backendFailure.target = TARGET_STOP;
            backendFailure.hits = 0;
            for (unsigned p = 0; backendFailure.hits < 3; p++) {
                assert(p < 200000);
                afatfs_poll();
                assert(!request.complete && !afatfs_canPowerOff());
            }
            backendFailure.kind = FAIL_NONE;
        }
        waitRequest(&request, AFATFS_ERROR_NONE);
        assert(afatfs_canPowerOff() && !io.pending);
        // Free data is deliberately nonzero: directory growth must initialize it, and exFAT VDL must hide it.
        fatVolumeID_t *boot = (fatVolumeID_t *)(media + options.startSector * SECTOR);
        uint32_t freeSector;
        if (types[n] == FAT_FILESYSTEM_TYPE_EXFAT) {
            freeSector = options.startSector + boot->exFat.clusterHeapOffset +
                (boot->exFat.rootDirectoryCluster - 1) * (1U << boot->exFat.sectorsPerClusterShift);
        } else {
            bool fat32 = types[n] == FAT_FILESYSTEM_TYPE_FAT32;
            freeSector = options.startSector + boot->fat.reservedSectorCount +
                2 * (fat32 ? boot->fat.fatDescriptor.fat32.FATSize32 : boot->fat.FATSize16) +
                (fat32 ? boot->fat.sectorsPerCluster : 32);
        }
        assert(freeSector < options.startSector + options.sectorCount);
        memset(media + freeSector * SECTOR, 0xA5, (options.startSector + options.sectorCount - freeSector) * SECTOR);
        mount();
        assert(!afatfs_isReadOnly());
        unsigned before = submittedWrites;
        assert(!afatfs_fdisk(&request, &partitions, apiDone, &callbackContext));
        assert(!afatfs_mkfs(&request, &options, apiDone, &callbackContext));
        assert(submittedWrites == before);
        afatfsVolumeInfo_t volume;
        API_TEST(afatfs_getlabel(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
        assert(volume.serial == options.serial);
        if (types[n] != FAT_FILESYSTEM_TYPE_FAT16) {
            unsigned fills =
                types[n] == FAT_FILESYSTEM_TYPE_EXFAT ? 3 : 16 - (afatfs_getContiguousFreeSpace() ? 1U : 0U);
            for (unsigned p = 0; p < fills; p++) {
                char name[64];
                if (types[n] == FAT_FILESYSTEM_TYPE_EXFAT && p) {
                    snprintf(name, sizeof(name), "label full directory %u.bin", p);
                } else {
                    snprintf(name, sizeof(name), "FILL%04u", p);
                }
                afatfsFilePtr_t file = openFile(name, "w");
                assert(file);
                closeFile(file);
            }
            API_TEST(afatfs_setlabel(&request, "FULLROOT", apiDone, &callbackContext), AFATFS_ERROR_NONE);
            API_TEST(afatfs_getlabel(&request, &volume, apiDone, &callbackContext), AFATFS_ERROR_NONE);
            assert(!strcmp(volume.label, "FULLROOT"));
        }
        exerciseNewApi(types[n] == FAT_FILESYSTEM_TYPE_EXFAT);
        const char *imageDirectory = getenv("AFATFS_API_IMAGE_DIR");
        if (imageDirectory) {
            uint8_t payload[2003];
            for (unsigned p = 0; p < sizeof(payload); p++) {
                payload[p] = (uint8_t)(p * 17 + 55);
            }
            afatfsFilePtr_t verify = openFile("VERIFY.BIN", "w");
            assert(verify);
            writeBytes(verify, payload, sizeof(payload));
            closeFile(verify);
            API_TEST(afatfs_utime(&request, "VERIFY.BIN", 0x58221883, apiDone, &callbackContext), AFATFS_ERROR_NONE);
            API_TEST(afatfs_setlabel(&request, "APIVAL", apiDone, &callbackContext), AFATFS_ERROR_NONE);
        }
        if (types[n] == FAT_FILESYSTEM_TYPE_FAT32) {
            backendFailure.kind = FAIL_WRITE_CALLBACK;
            backendFailure.target = TARGET_PREFETCH;
            backendFailure.sector = options.startSector + 1;
            backendFailure.hits = 0;
            for (unsigned p = 0; backendFailure.hits < 3; p++) {
                assert(p < 200000 && !afatfs_destroy(false));
                assert(!afatfs_canPowerOff());
                pump();
            }
            backendFailure.kind = FAIL_NONE;
        }
        unmount();
        if (types[n] == FAT_FILESYSTEM_TYPE_FAT32) {
            assert(get32(media + (options.startSector + 1) * SECTOR + 488) == UINT32_MAX);
            assert(get32(media + (options.startSector + 7) * SECTOR + 488) == UINT32_MAX);
        }
        if (imageDirectory) {
            char path[512];
            snprintf(path, sizeof(path), "%s/api-fat%u-%s.img", imageDirectory, (unsigned)types[n],
                partitions.gpt ? "gpt" : "mbr");
            FILE *image = fopen(path, "wb");
            assert(image && fwrite(media, 1, sizeof(media), image) == sizeof(media));
            assert(!fclose(image));
        }
        printf("format API: FAT type %u, %s, remount/workload and pending-I/O guards passed\n", (unsigned)types[n],
            partitions.gpt ? "GPT" : "MBR");
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    assert(!afatfs_isIdle() && !afatfs_canPowerOff());
    assert(sizeof(exFatFileEntry_t) == 32 && sizeof(exFatStreamEntry_t) == 32 && sizeof(exFatUpTableEntry_t) == 32);
    assert(offsetof(exFatStreamEntry_t, firstCluster) == 20);
    assert(offsetof(fatVolumeID_t, exFat.bytesPerSectorShift) == 108);
    assert(fat_unicodeToUpperCase('z') == 'Z' && fat_unicodeToUpperCase(0x00E1) == 0x00C1);
    assert(exFat_nameHash("existing.bin") == nameHash("existing.bin"));
#ifndef TEST_FAT_ONLY
    exerciseGPT();
    exerciseApiRecovery();
    exerciseBenignRename();
    format(false);
    mount();
    exerciseNewApi(true);
    exerciseAccessProtection(true);
    unmount();
    exercise(false);
    ioDelay = 5;
    exercise(true);
    exerciseBitmapAndContiguousDelete();
    exerciseConcurrent();
    exerciseUnsupportedMetadata();
    exerciseDirtyMount();
    exerciseSync();
    exerciseInterruptedRecovery();
    exerciseJournalBackendFailures();
    exerciseJournalPowerOff();
    exerciseExFatMediaRemoval();
    exerciseUntrustedHandoffs();
    rejectCorruption(108, 32, true); // Invalid shift, without undefined shifts in the parser.
    rejectCorruption(3, 'N', true);  // Partition type 07 alone does not identify exFAT.
    rejectCorruption(110, 2, true);  // TexFAT is not silently treated as ordinary exFAT.
    rejectCorruption(120, 1, false); // Boot checksum mismatch.
    puts("exFAT malformed boot sectors: passed");
#endif
#ifdef TEST_FAT_ONLY
    formatGPT(128, 128, 2, false);
    rejectGPT(); // GPT/exFAT remains opt-in.
#endif
    exerciseFat(false, 1);
    exerciseFat(false, 4);
    exerciseFat(true, 1);
    exerciseFormatApi();
    assert(afatfs_destroy(true));
    assert(!afatfs_isIdle() && !afatfs_canPowerOff());
    puts("global idle and clean power-off status: passed");
    return 0;
}
