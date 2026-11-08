// A fresh process is used for every remount. Trace records are completed 512-byte media writes.
#define main interop_workload_main
#include "test_exfat_external.c"
#undef main

#ifdef _WIN32
#define media_seek _fseeki64
#else
#define media_seek fseeko
#endif

static FILE *diskFile, *traceFile;
static unsigned completedWrites;
static struct {
    bool pending, write;
    unsigned delay;
    uint32_t block, user;
    uint8_t *buffer;
    uint8_t snapshot[512];
    sdcard_operationCompleteCallback_c callback;
} transfer;

bool sdcard_sim_init(const char *path) { diskFile = fopen(path, "r+b"); return diskFile != NULL; }
void sdcard_sim_destroy(void) { CHECK(!transfer.pending); CHECK(fclose(diskFile) == 0); }
bool sdcard_sim_isReady(void) { return !transfer.pending; }
bool sdcard_readBlock(uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    if (transfer.pending) return false;
    transfer.pending = true; transfer.write = false; transfer.delay = 3;
    transfer.block = block; transfer.buffer = buffer; transfer.callback = callback; transfer.user = user;
    return true;
}
sdcardOperationStatus_e sdcard_writeBlock(uint32_t block, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t user)
{
    if (!sdcard_readBlock(block, buffer, callback, user)) return SDCARD_OPERATION_BUSY;
    transfer.write = true;
    memcpy(transfer.snapshot, buffer, sizeof(transfer.snapshot));
    return SDCARD_OPERATION_IN_PROGRESS;
}
bool sdcard_poll(void)
{
    if (transfer.pending && --transfer.delay == 0) {
        CHECK(media_seek(diskFile, (uint64_t)transfer.block * 512, SEEK_SET) == 0);
        if (transfer.write) {
            CHECK(fwrite(transfer.snapshot, 1, 512, diskFile) == 512);
            CHECK(fflush(diskFile) == 0);
            if (traceFile) {
                uint8_t index[4];
                for (unsigned n = 0; n < 4; n++) index[n] = (uint8_t)(transfer.block >> (8 * n));
                CHECK(fwrite(index, 1, 4, traceFile) == 4);
                CHECK(fwrite(transfer.snapshot, 1, 512, traceFile) == 512);
            }
            completedWrites++;
        } else CHECK(fread(transfer.buffer, 1, 512, diskFile) == 512);
        transfer.pending = false;
        if (transfer.callback) transfer.callback(transfer.write ? SDCARD_BLOCK_OPERATION_WRITE : SDCARD_BLOCK_OPERATION_READ,
            transfer.block, transfer.buffer, transfer.user);
    }
    return !transfer.pending;
}
sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t block, uint32_t count) { (void)block; (void)count; return SDCARD_OPERATION_SUCCESS; }
sdcardOperationStatus_e sdcard_endWriteBlocks(void) { return transfer.pending ? SDCARD_OPERATION_BUSY : SDCARD_OPERATION_SUCCESS; }
void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback) { (void)callback; }
void sdcard_reset(void) { memset(&transfer, 0, sizeof(transfer)); }

static void drain(void)
{
    for (unsigned n = 0; !afatfs_flush() || transfer.pending; n++) { CHECK(n < MAX_POLLS); pump(); }
}

static bool probePattern(const char *name, unsigned count, unsigned tag)
{
    afatfsFilePtr_t file = openFile(name, "r");
    if (!file) return false;
    uint32_t size; CHECK(afatfs_fGetSize(file, &size));
    if (size < count) { closeFile(file); return false; }
    uint8_t buffer[4096]; CHECK(count <= sizeof(buffer));
    unsigned got = 0;
    for (unsigned n = 0; got < count; n++) { CHECK(n < MAX_POLLS); got += afatfs_fread(file, buffer + got, count - got); pump(); }
    bool match = true;
    for (unsigned n = 0; n < count; n++) if (buffer[n] != pattern(n, tag)) match = false;
    closeFile(file);
    return match;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(argc == 6);
    CHECK(sdcard_sim_init(argv[1]));
    bool recover = strcmp(argv[2], "recover") == 0;
    bool prepare = strcmp(argv[2], "prepare") == 0;
    bool probe = strcmp(argv[2], "probe") == 0 || recover;
    if (!probe || strcmp(argv[4], "unused")) { traceFile = fopen(argv[4], "wb"); CHECK(traceFile); }
    mountImage();
    unsigned mountWrites = completedWrites;
    bool keep = probePattern("keep.bin", 4096, 9);
    CHECK(keep);
    if (prepare) { unmountImage(); sdcard_sim_destroy(); if (traceFile) fclose(traceFile); return 0; }
    if (probe) {
        bool prefix = probePattern("target.bin", 700, 11);
        afatfsFilePtr_t file = recover ? NULL : openFile("must-not-create.bin", "w");
        bool protected = !file;
        bool readOnly = afatfs_isReadOnly();
        unsigned error = (unsigned)afatfs_getLastError();
        if (file) closeFile(file);
        unmountImage(); sdcard_sim_destroy();
        if (traceFile) CHECK(fclose(traceFile) == 0);
        FILE *out = fopen(argv[5], "w"); CHECK(out);
        fprintf(out, "{\"writes\":%u,\"mountWrites\":%u,\"protected\":%s,\"keep\":true,\"targetPrefix\":%s,\"readOnly\":%s,\"error\":%u}\n",
            completedWrites, mountWrites, protected ? "true" : "false", prefix ? "true" : "false", readOnly ? "true" : "false", error);
        CHECK(fclose(out) == 0);
        return 0;
    }
    unsigned clusterSize = (unsigned)strtoul(argv[2], NULL, 10);
    const char *scenario = argv[3];
    const char *name = "target.bin";
    char longName[256]; makeLongName(longName);
    if (strcmp(scenario, "long-create") == 0) name = longName;
    afatfsFilePtr_t file;
    if (strcmp(scenario, "mkdir") == 0) {
        file = openDirectory("new directory");
    } else if (strcmp(scenario, "empty-abort") == 0) {
        file = openFile(name, "w"); CHECK(file);
        uint8_t byte = 0x12; CHECK(afatfs_fwrite(file, &byte, 1) == 0);
        uint32_t size;
        for (unsigned n = 0; !afatfs_fGetSize(file, &size); n++) { CHECK(n < MAX_POLLS); pump(); }
        CHECK(size == 0);
    } else {
        file = openFile(name, strncmp(scenario, "append", 6) == 0 ? "a" : "w"); CHECK(file);
        writePattern(file, clusterSize * 2 + 123, 73);
    }
    unsigned accepted = completedWrites;
    unsigned synced = 0;
    if (strcmp(scenario, "append-sync") == 0) {
        completed = false; CHECK(afatfs_fsync(file, fileDone, &context)); waitDone();
        synced = completedWrites;
    }
    completed = false; CHECK(afatfs_fclose(file, done, &context)); waitDone();
    unsigned closed = completedWrites;
    bool prematureFlush = false;
    for (unsigned n = 0; n < 20; n++) {
        bool ready = afatfs_flush();
        if (ready && transfer.pending && transfer.write) prematureFlush = true;
        pump();
    }
    drain();
    unsigned flushed = completedWrites;
    unmountImage(); sdcard_sim_destroy();
    CHECK(fclose(traceFile) == 0);
    FILE *out = fopen(argv[5], "w"); CHECK(out);
    fprintf(out, "{\"writes\":%u,\"mount\":%u,\"accepted\":%u,\"synced\":%u,\"closed\":%u,\"flushed\":%u,\"prematureFlush\":%s}\n",
        completedWrites, mountWrites, accepted, synced, closed, flushed, prematureFlush ? "true" : "false");
    CHECK(fclose(out) == 0);
    return 0;
}
