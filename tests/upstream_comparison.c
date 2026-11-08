// Common legacy API benchmark for the working tree and upstream/master.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "asyncfatfs.h"
#include "sdcard.h"

#define SECTOR_BYTES 512U
#define FILE_BYTES (1024U * 1024U)
#define CLUSTER_COUNT 65530U
#ifndef COMPARISON_CPU_READ_PASSES
#define COMPARISON_CPU_READ_PASSES 1
#endif

typedef struct benchmarkStats_t {
#ifdef COMPARISON_READ_CACHE_AUDIT
    uint64_t firstDataReads;
    uint64_t repeatedDataReads;
#endif
#ifdef COMPARISON_MULTIREAD_BACKEND
    uint64_t readSessions;
    uint64_t readBlocks;
    uint64_t readStops;
#endif
    uint64_t reads;
    uint64_t writes;
    uint64_t multiBlocks;
    uint64_t multiSessions;
    uint64_t polls;
    uint64_t metadataReads;
    uint64_t metadataWrites;
#ifdef COMPARISON_CPU_WORK_AUDIT
    uint64_t generatedBytes;
    uint64_t writeAttempts;
    uint64_t pumpCalls;
#endif
#ifdef COMPARISON_METADATA_TRACE
    uint64_t primaryWrites;
    uint64_t mirrorWrites;
    uint64_t directoryWrites;
    uint64_t primaryReads;
    uint64_t mirrorReads;
    uint64_t directoryReads;
    uint64_t unchangedPrimaryWrites;
    uint64_t unchangedMirrorWrites;
#endif
} benchmarkStats_t;

static uint8_t *media;
static uint32_t mediaSectors;
static uint32_t dataStart;
static uint32_t rootSectors;
static uint64_t virtualTime;
static uint64_t transferEnd;
static uint32_t latencyProfile;
static uint32_t multiNext;
static uint32_t multiRemaining;
#ifdef COMPARISON_MULTIREAD_BACKEND
static bool multiRead;
sdcardOperationStatus_e sdcard_endReadBlocks(void);
#endif
static bool transferPending;
static bool transferWrite;
static uint32_t transferSector;
static uint8_t *transferBuffer;
static sdcard_operationCompleteCallback_c transferCallback;
static uint32_t transferUser;
static benchmarkStats_t statistics;
#ifdef COMPARISON_READ_CACHE_AUDIT
static uint8_t *readSeen;
#endif
static LARGE_INTEGER timerFrequency;
#ifdef COMPARISON_CPU_ONLY
static ULONG64 phaseCycles;
#endif

#ifdef COMPARISON_CHECK_ALLOCATION_ORDER
static uint32_t fatOrderWrites;

static void checkAllocationPreimage(uint32_t sector, const uint8_t *image)
{
    uint32_t fatSectors = (dataStart - 2) / 2;
    if (sector < 2 || sector >= dataStart) {
        return;
    }
    uint32_t copyIndex = (sector - 2) / fatSectors;
    uint32_t firstCluster = ((sector - 2) % fatSectors) * (SECTOR_BYTES / 4);
    const uint8_t *table = media + (size_t)(2 + copyIndex * fatSectors) * SECTOR_BYTES;
    for (uint32_t index = 0; index < SECTOR_BYTES / 4 && firstCluster + index < CLUSTER_COUNT + 2; index++) {
        if (firstCluster + index < 2) {
            continue;
        }
        const uint8_t *entry = image + index * 4;
        uint32_t nextCluster =
            entry[0] | (uint32_t)entry[1] << 8 | (uint32_t)entry[2] << 16 | (uint32_t)(entry[3] & 0x0F) << 24;
        if (nextCluster >= 2 && nextCluster < 0x0FFFFFF8) {
            assert(nextCluster < CLUSTER_COUNT + 2);
            const uint8_t *target = table + nextCluster * 4;
            // A partially written image must not be the first durable EOC for
            // the cluster to which that same image publishes a link.
            assert(target[0] || target[1] || target[2] || (target[3] & 0x0F));
        }
    }
}

static void checkAllocationOrder(uint32_t sector)
{
    uint32_t fatSectors = (dataStart - 2) / 2;
    if (sector < 2 || sector >= dataStart) {
        return;
    }
    uint32_t copyIndex = (sector - 2) / fatSectors;
    uint32_t firstCluster = ((sector - 2) % fatSectors) * (SECTOR_BYTES / 4);
    const uint8_t *table = media + (size_t)(2 + copyIndex * fatSectors) * SECTOR_BYTES;
    for (uint32_t index = 0; index < SECTOR_BYTES / 4 && firstCluster + index < CLUSTER_COUNT + 2; index++) {
        uint32_t clusterNumber = firstCluster + index;
        if (clusterNumber < 2) {
            continue;
        }
        const uint8_t *entry = table + clusterNumber * 4;
        uint32_t nextCluster =
            entry[0] | (uint32_t)entry[1] << 8 | (uint32_t)entry[2] << 16 | (uint32_t)(entry[3] & 0x0F) << 24;
        if (nextCluster >= 2 && nextCluster < 0x0FFFFFF8) {
            assert(nextCluster < CLUSTER_COUNT + 2);
            const uint8_t *nextEntry = table + nextCluster * 4;
            assert(nextEntry[0] || nextEntry[1] || nextEntry[2] || (nextEntry[3] & 0x0F));
        }
    }
    fatOrderWrites++;
}

static void checkPublishedDirectory(void)
{
    const uint8_t *table = media + 2 * SECTOR_BYTES;
    const uint8_t *directory = media + (size_t)dataStart * SECTOR_BYTES;
    for (uint32_t position = 0; position < rootSectors * SECTOR_BYTES; position += 32) {
        const uint8_t *entry = directory + position;
        if (!entry[0] || entry[0] == 0xE5 || (entry[11] & 0x18)) {
            continue;
        }
        uint32_t size = entry[28] | (uint32_t)entry[29] << 8 | (uint32_t)entry[30] << 16 | (uint32_t)entry[31] << 24;
        uint32_t cluster = entry[26] | (uint32_t)entry[27] << 8 | (uint32_t)entry[20] << 16 | (uint32_t)entry[21] << 24;
        uint32_t remaining = size ? (size - 1) / (rootSectors * SECTOR_BYTES) + 1 : 0;
        while (remaining--) {
            assert(cluster >= 2 && cluster < CLUSTER_COUNT + 2);
            const uint8_t *link = table + cluster * 4;
            uint32_t next =
                link[0] | (uint32_t)link[1] << 8 | (uint32_t)link[2] << 16 | (uint32_t)(link[3] & 0x0F) << 24;
            assert(next != 0);
            cluster = next;
        }
    }
}
#endif

void sdcard_reset(void)
{
    assert(!transferPending);
    multiRemaining = 0;
#ifdef COMPARISON_MULTIREAD_BACKEND
    multiRead = false;
#endif
}

bool sdcard_poll(void)
{
    statistics.polls++;
    virtualTime += 10;
    if (transferPending && virtualTime >= transferEnd) {
        transferPending = false;
        uint8_t *sectorData = media + (size_t)transferSector * SECTOR_BYTES;
        if (transferWrite) {
#ifdef COMPARISON_METADATA_TRACE
            if (!memcmp(sectorData, transferBuffer, SECTOR_BYTES)) {
                uint32_t fatSectors = (dataStart - 2) / 2;
                statistics.unchangedPrimaryWrites += transferSector >= 2 && transferSector < 2 + fatSectors;
                statistics.unchangedMirrorWrites += transferSector >= 2 + fatSectors && transferSector < dataStart;
            }
#endif
#ifdef COMPARISON_CHECK_ALLOCATION_ORDER
            checkAllocationPreimage(transferSector, transferBuffer);
#endif
            memcpy(sectorData, transferBuffer, SECTOR_BYTES);
#ifdef COMPARISON_CHECK_ALLOCATION_ORDER
            checkAllocationOrder(transferSector);
            checkPublishedDirectory();
#endif
            statistics.writes++;
            statistics.metadataWrites += transferSector < dataStart + rootSectors;
#ifdef COMPARISON_METADATA_TRACE
            uint32_t fatSectors = (dataStart - 2) / 2;
            statistics.primaryWrites += transferSector >= 2 && transferSector < 2 + fatSectors;
            statistics.mirrorWrites += transferSector >= 2 + fatSectors && transferSector < dataStart;
            statistics.directoryWrites += transferSector >= dataStart && transferSector < dataStart + rootSectors;
#endif
        } else {
            memcpy(transferBuffer, sectorData, SECTOR_BYTES);
#ifdef COMPARISON_MULTIREAD_BACKEND
            if (multiRead && !multiRemaining) {
                multiRead = false;
                statistics.readStops++;
                virtualTime += latencyProfile ? 100 : 0;
            }
#endif
            statistics.reads++;
#ifdef COMPARISON_READ_CACHE_AUDIT
            if (readSeen && transferSector >= dataStart + rootSectors) {
                statistics.firstDataReads += !readSeen[transferSector];
                statistics.repeatedDataReads += readSeen[transferSector] != 0;
                readSeen[transferSector] = 1;
            }
#endif
            statistics.metadataReads += transferSector < dataStart + rootSectors;
#ifdef COMPARISON_METADATA_TRACE
            uint32_t fatSectors = (dataStart - 2) / 2;
            statistics.primaryReads += transferSector >= 2 && transferSector < 2 + fatSectors;
            statistics.mirrorReads += transferSector >= 2 + fatSectors && transferSector < dataStart;
            statistics.directoryReads += transferSector >= dataStart && transferSector < dataStart + rootSectors;
#endif
        }
        transferCallback(transferWrite ? SDCARD_BLOCK_OPERATION_WRITE : SDCARD_BLOCK_OPERATION_READ, transferSector,
            transferBuffer, transferUser);
    }
    return !transferPending;
}

static void startTransfer(uint32_t sector, uint8_t *buffer, sdcard_operationCompleteCallback_c callback,
    uint32_t callbackData, bool writing, uint32_t latency)
{
    assert(sector < mediaSectors);
    transferPending = true;
    transferWrite = writing;
    transferSector = sector;
    transferBuffer = buffer;
    transferCallback = callback;
    transferUser = callbackData;
    transferEnd = virtualTime + (latencyProfile ? latency : 10);
}

bool sdcard_readBlock(
    uint32_t sector, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    if (transferPending) {
        return false;
    }
#ifdef COMPARISON_MULTIREAD_BACKEND
    if (multiRead && multiRemaining && sector == multiNext) {
        multiNext++;
        multiRemaining--;
        statistics.readBlocks++;
        startTransfer(sector, buffer, callback, callbackData, false, 100);
        return true;
    }
    if (sdcard_endReadBlocks() != SDCARD_OPERATION_SUCCESS) {
        return false;
    }
#endif
    if (multiRemaining) {
        multiRemaining = 0;
        virtualTime += latencyProfile ? 100 : 0;
    }
    startTransfer(sector, buffer, callback, callbackData, false, 200);
    return true;
}

#ifdef COMPARISON_MULTIREAD_BACKEND
bool sdcard_isReadBlockInSequence(uint32_t sector)
{
    return !transferPending && multiRead && multiRemaining && multiNext == sector;
}

sdcardOperationStatus_e sdcard_endReadBlocks(void)
{
    if (transferPending && !transferWrite) {
        return SDCARD_OPERATION_BUSY;
    }
    if (multiRead) {
        multiRead = false;
        multiRemaining = 0;
        statistics.readStops++;
        virtualTime += latencyProfile ? 100 : 0;
    }
    return SDCARD_OPERATION_SUCCESS;
}

sdcardOperationStatus_e sdcard_beginReadBlocks(uint32_t sector, uint32_t count)
{
    if (transferPending) {
        return SDCARD_OPERATION_BUSY;
    }
    assert(count >= 2 && sector < mediaSectors && count <= mediaSectors - sector);
    if (multiRead && multiRemaining && sector == multiNext) {
        return SDCARD_OPERATION_SUCCESS;
    }
    sdcard_endReadBlocks();
    if (multiRemaining) {
        multiRemaining = 0;
        virtualTime += latencyProfile ? 100 : 0;
    }
    multiRead = true;
    multiNext = sector;
    multiRemaining = count;
    statistics.readSessions++;
    virtualTime += latencyProfile ? 100 : 0;
    return SDCARD_OPERATION_SUCCESS;
}
#endif

sdcardOperationStatus_e sdcard_writeBlock(
    uint32_t sector, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)
{
    if (transferPending) {
        return SDCARD_OPERATION_BUSY;
    }
#ifdef COMPARISON_MULTIREAD_BACKEND
    sdcard_endReadBlocks();
#endif
    bool multiple = multiRemaining && sector == multiNext;
    if (multiple) {
        multiNext++;
        multiRemaining--;
        statistics.multiBlocks++;
    } else if (multiRemaining) {
        multiRemaining = 0;
        virtualTime += latencyProfile ? 100 : 0;
    }
    uint32_t latency = multiple ? 400 : 800;
#ifdef COMPARISON_ZERO_MIRROR_LATENCY
    // Diagnostic only: still writes the second FAT and waits for its callback,
    // but charges only one poll to isolate its device latency from library work.
    if (sector >= 2 + (dataStart - 2) / 2 && sector < dataStart) {
        latency = 10;
    }
#endif
    startTransfer(sector, buffer, callback, callbackData, true, latency);
    return SDCARD_OPERATION_IN_PROGRESS;
}

sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t sector, uint32_t count)
{
    if (transferPending) {
        return SDCARD_OPERATION_BUSY;
    }
#ifdef COMPARISON_MULTIREAD_BACKEND
    sdcard_endReadBlocks();
#endif
    statistics.multiSessions++;
    multiNext = sector;
    multiRemaining = count;
    virtualTime += latencyProfile ? 100 : 0;
    return SDCARD_OPERATION_SUCCESS;
}

sdcardOperationStatus_e sdcard_endWriteBlocks(void)
{
    if (transferPending) {
        return SDCARD_OPERATION_BUSY;
    }
    if (multiRemaining) {
        virtualTime += latencyProfile ? 100 : 0;
    }
    multiRemaining = 0;
    return SDCARD_OPERATION_SUCCESS;
}

void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback)
{
    (void)callback;
}

static void pump(void)
{
#ifdef COMPARISON_CPU_WORK_AUDIT
    statistics.pumpCalls++;
#endif
    afatfs_poll();
    assert(statistics.polls < 100000000ULL);
    assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
}

static void put16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)value;
    buffer[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *buffer, uint32_t value)
{
    put16(buffer, (uint16_t)value);
    put16(buffer + 2, (uint16_t)(value >> 16));
}

static void formatMedia(uint32_t clusterSectors)
{
    const uint32_t fatSectors = ((CLUSTER_COUNT + 2) * 4 + 511) / 512;
    dataStart = 2 + 2 * fatSectors;
    rootSectors = clusterSectors;
    mediaSectors = dataStart + CLUSTER_COUNT * clusterSectors;
    media = calloc(mediaSectors, SECTOR_BYTES);
    assert(media);
    uint8_t *partition = media + 446;
    partition[4] = 0x0B;
    put32(partition + 8, 1);
    put32(partition + 12, mediaSectors - 1);
    media[510] = 0x55;
    media[511] = 0xAA;
    uint8_t *boot = media + SECTOR_BYTES;
    boot[0] = 0xEB;
    boot[1] = 0x58;
    boot[2] = 0x90;
    put16(boot + 11, SECTOR_BYTES);
    boot[13] = (uint8_t)clusterSectors;
    put16(boot + 14, 1);
    boot[16] = 2;
    boot[21] = 0xF8;
    put32(boot + 32, mediaSectors - 1);
    put32(boot + 36, fatSectors);
    put32(boot + 44, 2);
    put16(boot + 48, 0xFFFF);
    put16(boot + 50, 0xFFFF);
    boot[510] = 0x55;
    boot[511] = 0xAA;
    for (uint32_t index = 0; index < 2; index++) {
        uint8_t *table = media + (size_t)(2 + index * fatSectors) * SECTOR_BYTES;
        put32(table, 0x0FFFFFF8);
        put32(table + 4, 0x0FFFFFFF);
        put32(table + 8, 0x0FFFFFFF);
    }
}

static void mountMedia(void)
{
    afatfs_init();
    for (uint32_t retry = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; retry++) {
        assert(retry < 1000000);
        pump();
    }
}

static void unmountMedia(void)
{
    for (uint32_t retry = 0; !afatfs_destroy(false); retry++) {
        assert(retry < 10000000);
        pump();
    }
    assert(!transferPending);
}

static afatfsFilePtr_t openedFile;
static bool callbackDone;

static void openDone(afatfsFilePtr_t file, void *user)
{
    (void)user;
    openedFile = file;
    callbackDone = true;
}

static afatfsFilePtr_t openFile(const char *name, const char *mode)
{
    callbackDone = false;
    assert(afatfs_fopen(name, mode, openDone, NULL));
    for (uint32_t retry = 0; !callbackDone; retry++) {
        assert(retry < 1000000);
        pump();
    }
    assert(openedFile);
    return openedFile;
}

static void closeDone(void *user)
{
    (void)user;
    callbackDone = true;
}

static void closeFile(afatfsFilePtr_t file)
{
    callbackDone = false;
    while (!afatfs_fclose(file, closeDone, NULL)) {
        pump();
    }
    for (uint32_t retry = 0; !callbackDone; retry++) {
        assert(retry < 1000000);
        pump();
    }
}

static void drain(void)
{
    for (uint32_t retry = 0; !afatfs_flush(); retry++) {
        assert(retry < 10000000);
        pump();
    }
    while (transferPending) {
        pump();
    }
    assert(sdcard_endWriteBlocks() == SDCARD_OPERATION_SUCCESS);
}

static uint8_t payloadByte(uint32_t offset, uint32_t fileIndex)
{
    return (uint8_t)(offset * 17 + fileIndex * 29);
}

static void printPhase(const char *phase, uint32_t clusterSectors, uint32_t chunkBytes, uint32_t fileCount,
    uint32_t repetition, benchmarkStats_t before, uint64_t beforeVirtual, LARGE_INTEGER beforeTimer)
{
    LARGE_INTEGER afterTimer;
    QueryPerformanceCounter(&afterTimer);
#ifdef COMPARISON_CPU_ONLY
    ULONG64 completedCycles;
    assert(QueryThreadCycleTime(GetCurrentThread(), &completedCycles));
#endif
    double seconds = (double)(afterTimer.QuadPart - beforeTimer.QuadPart) / timerFrequency.QuadPart;
    uint32_t payloadBytes = FILE_BYTES * fileCount;
    if (!strcmp(phase, "read")) {
        payloadBytes *= COMPARISON_CPU_READ_PASSES;
    }
#ifdef COMPARISON_CPU_ONLY
    fprintf(stderr, "CPU_CYCLES,%s,%u,%u,%u,%u,%u,%llu,%u\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(completedCycles - phaseCycles), payloadBytes);
#endif
#ifdef COMPARISON_READ_CACHE_AUDIT
    fprintf(stderr, "READ_REUSE,%s,%u,%u,%u,%u,%u,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.firstDataReads - before.firstDataReads),
        (unsigned long long)(statistics.repeatedDataReads - before.repeatedDataReads));
#endif
    printf("%s,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.9f\n", phase, latencyProfile, clusterSectors,
        chunkBytes, fileCount, repetition, payloadBytes, (unsigned long long)(statistics.reads - before.reads),
        (unsigned long long)(statistics.writes - before.writes),
        (unsigned long long)(statistics.multiSessions - before.multiSessions),
        (unsigned long long)(statistics.multiBlocks - before.multiBlocks),
        (unsigned long long)(statistics.polls - before.polls), (unsigned long long)(virtualTime - beforeVirtual),
        (unsigned long long)(statistics.metadataReads - before.metadataReads),
        (unsigned long long)(statistics.metadataWrites - before.metadataWrites), seconds);
#ifdef COMPARISON_MULTIREAD_BACKEND
    fprintf(stderr, "READ_BATCH,%s,%u,%u,%u,%u,%u,%llu,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.readSessions - before.readSessions),
        (unsigned long long)(statistics.readBlocks - before.readBlocks),
        (unsigned long long)(statistics.readStops - before.readStops));
#endif
#ifdef COMPARISON_METADATA_TRACE
    fprintf(stderr, "META,%s,%u,%u,%u,%u,%u,%llu,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.primaryWrites - before.primaryWrites),
        (unsigned long long)(statistics.mirrorWrites - before.mirrorWrites),
        (unsigned long long)(statistics.directoryWrites - before.directoryWrites));
    fprintf(stderr, "META_READ,%s,%u,%u,%u,%u,%u,%llu,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.primaryReads - before.primaryReads),
        (unsigned long long)(statistics.mirrorReads - before.mirrorReads),
        (unsigned long long)(statistics.directoryReads - before.directoryReads));
    fprintf(stderr, "FAT_NOOP,%s,%u,%u,%u,%u,%u,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.unchangedPrimaryWrites - before.unchangedPrimaryWrites),
        (unsigned long long)(statistics.unchangedMirrorWrites - before.unchangedMirrorWrites));
#endif
#ifdef COMPARISON_CPU_WORK_AUDIT
    fprintf(stderr, "WORK,%s,%u,%u,%u,%u,%u,%llu,%llu,%llu\n", phase, latencyProfile, clusterSectors, chunkBytes,
        fileCount, repetition, (unsigned long long)(statistics.generatedBytes - before.generatedBytes),
        (unsigned long long)(statistics.writeAttempts - before.writeAttempts),
        (unsigned long long)(statistics.pumpCalls - before.pumpCalls));
#endif
}

static uint32_t get32(const uint8_t *buffer)
{
    return buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}

// Independent on-disk audit, without calling filesystem code.
static void auditMedia(uint32_t fileCount, uint32_t clusterSectors)
{
    uint32_t fatSectors = (dataStart - 2) / 2;
    const uint8_t *table = media + 2 * SECTOR_BYTES;
    uint32_t allocated = 0;
    for (uint32_t clusterNumber = 2; clusterNumber < CLUSTER_COUNT + 2; clusterNumber++) {
        allocated += (get32(table + clusterNumber * 4) & 0x0FFFFFFF) != 0;
    }
    assert(allocated ==
        1 + fileCount * ((FILE_BYTES + clusterSectors * SECTOR_BYTES - 1) / (clusterSectors * SECTOR_BYTES)));
    uint32_t mirrorDifferences = 0;
    for (uint32_t sector = 0; sector < fatSectors; sector++) {
        mirrorDifferences += memcmp(table + (size_t)sector * SECTOR_BYTES,
                                 table + (size_t)(fatSectors + sector) * SECTOR_BYTES, SECTOR_BYTES) != 0;
    }
#ifdef COMPARISON_CURRENT
    assert(mirrorDifferences == 0);
#endif
    fprintf(stderr, "MIRROR,%u,%u,%u\n", clusterSectors, fileCount, mirrorDifferences);
    const uint8_t *directory = media + (size_t)dataStart * SECTOR_BYTES;
    for (uint32_t fileIndex = 0; fileIndex < fileCount; fileIndex++) {
        char name[12] = "BENCHA  BIN";
        name[5] = (char)('A' + fileIndex);
        const uint8_t *entry = NULL;
        for (uint32_t position = 0; position < clusterSectors * SECTOR_BYTES; position += 32) {
            if (!memcmp(directory + position, name, 11)) {
                entry = directory + position;
                break;
            }
        }
        assert(entry && get32(entry + 28) == FILE_BYTES);
        uint32_t clusterNumber = (get32(entry + 20) & 0xFFFF) << 16;
        clusterNumber |= get32(entry + 26) & 0xFFFF;
        uint32_t offset = 0;
        while (offset < FILE_BYTES) {
            assert(clusterNumber >= 2 && clusterNumber < CLUSTER_COUNT + 2);
            const uint8_t *content = media + (size_t)(dataStart + (clusterNumber - 2) * clusterSectors) * SECTOR_BYTES;
            uint32_t amount = clusterSectors * SECTOR_BYTES;
            if (amount > FILE_BYTES - offset) {
                amount = FILE_BYTES - offset;
            }
            for (uint32_t index = 0; index < amount; index++) {
                assert(content[index] == payloadByte(offset + index, fileIndex));
            }
            offset += amount;
            clusterNumber = get32(table + clusterNumber * 4) & 0x0FFFFFFF;
        }
        assert(clusterNumber >= 0x0FFFFFF8);
    }
}

static void runCase(uint32_t clusterSectors, uint32_t chunkBytes, uint32_t fileCount, uint32_t repetition)
{
    formatMedia(clusterSectors);
    memset(&statistics, 0, sizeof(statistics));
    virtualTime = 0;
    mountMedia();
    const char *names[] = {"BENCHA.BIN", "BENCHB.BIN", "BENCHC.BIN"};
    afatfsFilePtr_t files[3];
    uint8_t buffer[4096];
#if defined(COMPARISON_INTERLEAVED_REQUESTS) && defined(COMPARISON_PREPARED_INTERLEAVED)
    // Host-only application buffers, excluded from library RAM/stack measurements.
    uint8_t preparedBuffers[3][4096];
#endif
    benchmarkStats_t before = statistics;
    uint64_t beforeVirtual = virtualTime;
    LARGE_INTEGER beforeTimer;
    QueryPerformanceCounter(&beforeTimer);
#ifdef COMPARISON_CPU_ONLY
    assert(QueryThreadCycleTime(GetCurrentThread(), &phaseCycles));
#endif
    for (uint32_t index = 0; index < fileCount; index++) {
        files[index] = openFile(names[index], "w");
    }
    for (uint32_t offset = 0; offset < FILE_BYTES;) {
        uint32_t amount = chunkBytes < FILE_BYTES - offset ? chunkBytes : FILE_BYTES - offset;
#ifdef COMPARISON_INTERLEAVED_REQUESTS
#ifdef COMPARISON_PREPARED_INTERLEAVED
        for (uint32_t index = 0; index < fileCount; index++) {
            for (uint32_t position = 0; position < amount; position++) {
                preparedBuffers[index][position] = payloadByte(offset + position, index);
            }
#ifdef COMPARISON_CPU_WORK_AUDIT
            statistics.generatedBytes += amount;
#endif
        }
#endif
        uint32_t written[3] = {0};
        bool pending;
        do {
            pending = false;
            for (uint32_t index = 0; index < fileCount; index++) {
#ifndef COMPARISON_PREPARED_INTERLEAVED
                for (uint32_t position = written[index]; position < amount; position++) {
                    buffer[position] = payloadByte(offset + position, index);
                }
#ifdef COMPARISON_CPU_WORK_AUDIT
                statistics.generatedBytes += amount - written[index];
#endif
#endif
                if (written[index] < amount) {
#ifdef COMPARISON_CPU_WORK_AUDIT
                    statistics.writeAttempts++;
#endif
#ifdef COMPARISON_PREPARED_INTERLEAVED
                    written[index] += afatfs_fwrite(
                        files[index], preparedBuffers[index] + written[index], amount - written[index]);
#else
                    written[index] += afatfs_fwrite(files[index], buffer + written[index], amount - written[index]);
#endif
                }
                pending |= written[index] < amount;
            }
            pump();
        } while (pending);
#else
        for (uint32_t index = 0; index < fileCount; index++) {
            for (uint32_t position = 0; position < amount; position++) {
                buffer[position] = payloadByte(offset + position, index);
            }
#ifdef COMPARISON_CPU_WORK_AUDIT
            statistics.generatedBytes += amount;
#endif
            for (uint32_t written = 0; written < amount;) {
#ifdef COMPARISON_CPU_WORK_AUDIT
                statistics.writeAttempts++;
#endif
                written += afatfs_fwrite(files[index], buffer + written, amount - written);
                pump();
            }
        }
#endif
        offset += amount;
    }
#ifdef COMPARISON_CHECK_IDLE
    for (uint32_t retry = 0; !afatfs_isIdle(); retry++) {
        assert(retry < 10000);
        pump();
    }
#endif
    for (uint32_t index = 0; index < fileCount; index++) {
        closeFile(files[index]);
    }
    drain();
    printPhase("write", clusterSectors, chunkBytes, fileCount, repetition, before, beforeVirtual, beforeTimer);
    uint64_t drainedWrites = statistics.writes;
    unmountMedia();
    assert(statistics.writes == drainedWrites);
    auditMedia(fileCount, clusterSectors);
    mountMedia(); // Cold library cache; the media remains in RAM for both revisions.
#ifdef COMPARISON_READ_CACHE_AUDIT
    readSeen = calloc(mediaSectors, 1);
    assert(readSeen);
#endif
    before = statistics;
    beforeVirtual = virtualTime;
    QueryPerformanceCounter(&beforeTimer);
#ifdef COMPARISON_CPU_ONLY
    assert(QueryThreadCycleTime(GetCurrentThread(), &phaseCycles));
#endif
    for (uint32_t index = 0; index < fileCount; index++) {
        files[index] = openFile(names[index], "r");
        uint32_t size;
        assert(afatfs_fGetSize(files[index], &size) && size == FILE_BYTES);
    }
    for (uint32_t pass = 0; pass < COMPARISON_CPU_READ_PASSES; pass++) {
        if (pass) {
            for (uint32_t index = 0; index < fileCount; index++) {
#ifdef COMPARISON_CURRENT
                assert(afatfs_fseek(files[index], 0, AFATFS_SEEK_SET, NULL, NULL) == AFATFS_OPERATION_SUCCESS);
#else
                assert(afatfs_fseek(files[index], 0, AFATFS_SEEK_SET) == AFATFS_OPERATION_SUCCESS);
#endif
            }
        }
        for (uint32_t offset = 0; offset < FILE_BYTES;) {
            uint32_t amount = chunkBytes < FILE_BYTES - offset ? chunkBytes : FILE_BYTES - offset;
            for (uint32_t index = 0; index < fileCount; index++) {
                for (uint32_t received = 0; received < amount;) {
                    received += afatfs_fread(files[index], buffer + received, amount - received);
                    pump();
                }
                for (uint32_t position = 0; position < amount; position++) {
                    assert(buffer[position] == payloadByte(offset + position, index));
                }
            }
            offset += amount;
        }
    }
    for (uint32_t index = 0; index < fileCount; index++) {
        closeFile(files[index]);
    }
    drain();
    printPhase("read", clusterSectors, chunkBytes, fileCount, repetition, before, beforeVirtual, beforeTimer);
#ifdef COMPARISON_READ_CACHE_AUDIT
    free(readSeen);
    readSeen = NULL;
#endif
    unmountMedia();
    free(media);
}

#ifdef COMPARISON_NEAR_FULL
static void runNearFull(void)
{
    formatMedia(1);
    uint32_t fatSectors = (dataStart - 2) / 2;
    for (uint32_t copyIndex = 0; copyIndex < 2; copyIndex++) {
        uint8_t *table = media + (size_t)(2 + copyIndex * fatSectors) * SECTOR_BYTES;
        // Leave exactly three free clusters: two for the file and one directory.
        for (uint32_t cluster = 6; cluster < CLUSTER_COUNT + 2; cluster++) {
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
            put32(table + cluster * 4, cluster + 1 < CLUSTER_COUNT + 2 ? cluster + 1 : 0x0FFFFFFF);
#else
            put32(table + cluster * 4, 0x0FFFFFFF);
#endif
        }
    }
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
    fatDirectoryEntry_t *directoryEntry = (fatDirectoryEntry_t *)(media + (size_t)dataStart * SECTOR_BYTES);
    memcpy(directoryEntry->fileName, "FULL    BIN", 11);
    directoryEntry->attrib = FAT_FILE_ATTRIBUTE_ARCHIVE;
    directoryEntry->firstClusterLow = 6;
    directoryEntry->fileSize = (CLUSTER_COUNT - 4) * SECTOR_BYTES;
#endif
    mountMedia();
    afatfsFilePtr_t file = openFile("NEARFULL.BIN", "w");
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
    // The unused writer owns no clusters; its lane must not hide the two odd gaps.
    afatfsFilePtr_t idle = openFile("IDLE.BIN", "w");
#endif
    uint8_t payload[1024];
    memset(payload, 0x6A, sizeof(payload));
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
    memset(payload + SECTOR_BYTES, 0x6B, sizeof(payload) - SECTOR_BYTES);
#endif
    for (uint32_t written = 0; written < sizeof(payload);) {
        uint32_t amount = (uint32_t)sizeof(payload) - written;
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
        if (amount > 257) {
            amount = 257;
        }
#endif
        written += afatfs_fwrite(file, payload + written, amount);
        pump();
    }
    callbackDone = false;
    assert(afatfs_mkdir("LASTDIR", openDone, NULL));
    for (uint32_t retry = 0; !callbackDone; retry++) {
        assert(retry < 10000);
        pump();
    }
    assert(openedFile);
    closeFile(openedFile);
    closeFile(file);
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
    closeFile(idle);
#endif
    drain();
    unmountMedia();
    const uint8_t *table = media + 2 * SECTOR_BYTES;
    assert(!memcmp(table, table + (size_t)fatSectors * SECTOR_BYTES, (size_t)fatSectors * SECTOR_BYTES));
    assert((get32(table + 5 * 4) & 0x0FFFFFFF) >= 0x0FFFFFF8);
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
    for (uint32_t cluster = 6; cluster < CLUSTER_COUNT + 2; cluster++) {
        assert((get32(table + cluster * 4) & 0x0FFFFFFF) ==
            (cluster + 1 < CLUSTER_COUNT + 2 ? cluster + 1 : 0x0FFFFFFF));
    }
#endif
    mountMedia();
    file = openFile("NEARFULL.BIN", "r");
    uint32_t received = 0;
    while (received < sizeof(payload)) {
        received += afatfs_fread(file, payload + received, (uint32_t)sizeof(payload) - received);
        pump();
    }
    for (uint32_t index = 0; index < sizeof(payload); index++) {
#ifdef COMPARISON_SMALL_WINDOW_NEAR_FULL
        assert(payload[index] == (uint8_t)(0x6A + index / SECTOR_BYTES));
#else
        assert(payload[index] == 0x6A);
#endif
    }
    closeFile(file);
    unmountMedia();
    free(media);
    puts("Near-full reservation reclamation: file, last directory, mirror and remount passed.");
}
#endif

int main(int count, char **arguments)
{
    uint32_t repetitions = count > 1 ? (uint32_t)strtoul(arguments[1], NULL, 10) : 7;
#ifdef COMPARISON_CPU_ONLY
    DWORD_PTR processMask;
    DWORD_PTR systemMask;
    assert(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask));
    assert(processMask);
    assert(SetThreadAffinityMask(GetCurrentThread(), processMask & (~processMask + 1)));
#endif
    QueryPerformanceFrequency(&timerFrequency);
#ifdef COMPARISON_NEAR_FULL
    runNearFull();
    return 0;
#endif
    puts(
        "phase,profile,cluster_sectors,chunk_bytes,files,repetition,payload_bytes,reads,writes,multi_sessions,multi_blocks,polls,virtual_us,metadata_reads,metadata_writes,host_seconds");
    for (latencyProfile = 0; latencyProfile < 2; latencyProfile++) {
#ifdef COMPARISON_CPU_ONLY
        if (latencyProfile != 0) {
            break;
        }
#endif
        for (uint32_t repetition = 0; repetition < repetitions; repetition++) {
            for (uint32_t clusterSectors = 1; clusterSectors <= 8; clusterSectors *= 8) {
#ifdef COMPARISON_CPU_ONLY
                if (clusterSectors != 1) {
                    break;
                }
#endif
                for (uint32_t chunkBytes = 257; chunkBytes <= 4096; chunkBytes = 4096) {
#ifdef COMPARISON_THREE_FILES
                    for (uint32_t fileCount = 3; fileCount <= 3; fileCount++) {
#else
                    for (uint32_t fileCount = 1; fileCount <= 2; fileCount++) {
#endif
                        runCase(clusterSectors, chunkBytes, fileCount, repetition);
                    }
                    if (chunkBytes == 4096) {
                        break;
                    }
                }
            }
        }
    }
#ifdef COMPARISON_CHECK_ALLOCATION_ORDER
    assert(fatOrderWrites > 0);
    fprintf(stderr, "Allocation order checked after %u FAT sector writes.\n", fatOrderWrites);
#endif
    return 0;
}
