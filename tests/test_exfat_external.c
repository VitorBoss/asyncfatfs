// Black-box interoperability workload for images created by external exFAT formatters.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "asyncfatfs.h"
#include "sdcard_sim.h"

#define CHECK(test)                                                                                                    \
    do {                                                                                                               \
        if (!(test)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #test, action);                               \
            exit(2);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define MAX_POLLS 2000000U
static const char *action = "initializing";
static bool completed;
static afatfsFilePtr_t result;
static int context;

static void pump(void)
{
    afatfs_poll();
    if (afatfs_getFilesystemState() == AFATFS_FILESYSTEM_STATE_FATAL) {
        fprintf(stderr, "Filesystem error %u during %s\n", (unsigned)afatfs_getLastError(), action);
        exit(2);
    }
}
static void fileDone(afatfsFilePtr_t file, void *user)
{
    CHECK(user == &context);
    result = file;
    completed = true;
}
static void done(void *user)
{
    CHECK(user == &context);
    completed = true;
}
static void waitDone(void)
{
    for (unsigned n = 0; !completed; n++) {
        CHECK(n < MAX_POLLS);
        pump();
    }
}
static void mountImage(void)
{
    action = "mount";
    afatfs_init();
    for (unsigned n = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; n++) {
        CHECK(n < MAX_POLLS);
        pump();
    }
}
static void unmountImage(void)
{
    action = "unmount";
    for (unsigned n = 0; !afatfs_destroy(false); n++) {
        CHECK(n < MAX_POLLS);
    }
}
static afatfsFilePtr_t openFile(const char *name, const char *mode)
{
    action = name;
    completed = false;
    result = NULL;
    CHECK(afatfs_fopen(name, mode, fileDone, &context));
    waitDone();
    return result;
}
static afatfsFilePtr_t openDirectory(const char *name)
{
    action = name;
    completed = false;
    result = NULL;
    CHECK(afatfs_mkdir(name, fileDone, &context));
    waitDone();
    CHECK(result);
    return result;
}
static void closeFile(afatfsFilePtr_t file)
{
    completed = false;
    CHECK(afatfs_fclose(file, done, &context));
    waitDone();
}
static uint8_t pattern(uint32_t index, unsigned tag)
{
    return (uint8_t)(index * 17 + tag);
}
static void writePattern(afatfsFilePtr_t file, uint32_t count, unsigned tag)
{
    uint8_t buffer[1024];
    uint32_t offset = 0;
    for (unsigned n = 0; offset < count; n++) {
        CHECK(n < MAX_POLLS);
        unsigned amount = count - offset < sizeof(buffer) ? count - offset : sizeof(buffer);
        for (unsigned i = 0; i < amount; i++) {
            buffer[i] = pattern(offset + i, tag);
        }
        offset += afatfs_fwrite(file, buffer, amount);
        pump();
        CHECK(!afatfs_isFull());
    }
    uint32_t size;
    for (unsigned n = 0; !afatfs_fGetSize(file, &size); n++) {
        CHECK(n < MAX_POLLS);
        pump();
    }
}
static void readPattern(afatfsFilePtr_t file, uint32_t count, unsigned tag)
{
    uint8_t buffer[777];
    uint32_t offset = 0;
    for (unsigned n = 0; offset < count; n++) {
        CHECK(n < MAX_POLLS);
        unsigned amount = count - offset < sizeof(buffer) ? count - offset : sizeof(buffer);
        unsigned got = afatfs_fread(file, buffer, amount);
        for (unsigned i = 0; i < got; i++) {
            if (buffer[i] != pattern(offset + i, tag)) {
                fprintf(stderr, "Data mismatch at offset %u: %02x expected %02x\n", offset + i, buffer[i],
                    pattern(offset + i, tag));
                CHECK(false);
            }
        }
        offset += got;
        pump();
    }
}
static void verifyFile(const char *name, uint32_t count, unsigned tag)
{
    afatfsFilePtr_t file = openFile(name, "r");
    CHECK(file);
    uint32_t size;
    CHECK(afatfs_fGetSize(file, &size) && size == count);
    readPattern(file, count, tag);
    CHECK(afatfs_feof(file));
    closeFile(file);
}
static void makeLongName(char name[256])
{
    memset(name, 'n', 251);
    memcpy(name + 251, ".bin", 5);
}
static void growDirectory(const char *name, unsigned clusterSize)
{
    afatfsFilePtr_t dir = openDirectory(name);
    CHECK(afatfs_chdir(dir));
    closeFile(dir);
    // More than one cluster of file entry sets, for every cluster size in the matrix.
    unsigned children = clusterSize / 96 + 20;
    for (unsigned n = 0; n < children; n++) {
        char child[32];
        snprintf(child, sizeof(child), "child%04u.bin", n);
        afatfsFilePtr_t file = openFile(child, "w");
        CHECK(file);
        writePattern(file, 37, n);
        closeFile(file);
    }
    CHECK(afatfs_chdir(NULL));
}
static void verifyDirectory(const char *name, unsigned clusterSize)
{
    afatfsFilePtr_t dir = openDirectory(name);
    CHECK(afatfs_chdir(dir));
    closeFile(dir);
    unsigned children = clusterSize / 96 + 20;
    for (unsigned n = 0; n < children; n++) {
        char child[32];
        snprintf(child, sizeof(child), "child%04u.bin", n);
        verifyFile(child, 37, n);
    }
    CHECK(afatfs_chdir(NULL));
}
static void exercise(unsigned clusterSize)
{
    verifyFile("external seed.bin", 20003, 11);
    verifyFile("external empty.bin", 0, 0);
    verifyFile("external fragmented.bin", clusterSize * 3, 37);
    afatfsFilePtr_t dir = openDirectory("External directory");
    CHECK(afatfs_chdir(dir));
    closeFile(dir);
    verifyFile("external child.bin", 91, 49);
    CHECK(afatfs_chdir(NULL));

    afatfsFilePtr_t file = openFile("external seed.bin", "a");
    CHECK(file);
    writePattern(file, clusterSize * 2 + 77, 21);
    closeFile(file);
    file = openFile("external truncate.bin", "w");
    CHECK(file);
    writePattern(file, 333, 17);
    closeFile(file);
    file = openFile("external delete.bin", "r+");
    CHECK(file);
    completed = false;
    CHECK(afatfs_funlink(file, done, &context));
    waitDone();

    file = openFile("library output.bin", "w");
    CHECK(file);
    writePattern(file, clusterSize * 3 + 123, 73);
    closeFile(file);
    char longName[256];
    makeLongName(longName);
    file = openFile(longName, "w");
    CHECK(file);
    writePattern(file, 777, 55);
    closeFile(file);
    file = openFile("library empty.bin", "w");
    CHECK(file);
    closeFile(file);
    growDirectory("External directory", clusterSize);
    growDirectory("Library directory", clusterSize);

    unmountImage();
    mountImage();
    file = openFile("external seed.bin", "r");
    CHECK(file);
    uint32_t size;
    CHECK(afatfs_fGetSize(file, &size) && size == 20003 + clusterSize * 2 + 77);
    readPattern(file, 20003, 11);
    readPattern(file, clusterSize * 2 + 77, 21);
    closeFile(file);
    verifyFile("external truncate.bin", 333, 17);
    CHECK(!openFile("external delete.bin", "r"));
    verifyFile("library output.bin", clusterSize * 3 + 123, 73);
    verifyFile(longName, 777, 55);
    verifyFile("library empty.bin", 0, 0);
    verifyDirectory("External directory", clusterSize);
    verifyDirectory("Library directory", clusterSize);
}
#include "exfat_native_api.inc"
#ifdef TEST_NATIVE_LFN
#include "fat_lfn_native.inc"
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(argc == 4 || argc == 5);
    unsigned clusterSize = (unsigned)strtoul(argv[3], NULL, 10);
    CHECK(clusterSize >= 512 && clusterSize <= 65536);
    CHECK(sdcard_sim_init(argv[1]));
    mountImage();
#ifdef TEST_NATIVE_LFN
    if (strcmp(argv[2], "exercise-lfn") == 0) {
        CHECK(argc == 5);
        exerciseNativeLfn(clusterSize, argv[4]);
    } else if (strcmp(argv[2], "verify-lfn-roundtrip") == 0) {
        verifyNativeLfnRoundtrip();
    } else
#endif
        if (strcmp(argv[2], "exercise") == 0) {
        exercise(clusterSize);
    } else if (strcmp(argv[2], "exercise-api") == 0) {
        exerciseNativeApi(clusterSize);
    } else if (strcmp(argv[2], "verify-api-roundtrip") == 0) {
        verifyNativeApiRoundtrip(clusterSize);
    } else {
        CHECK(strcmp(argv[2], "verify-roundtrip") == 0);
        verifyFile("external roundtrip.bin", clusterSize + 93, 91);
        afatfsFilePtr_t file = openFile("library output.bin", "r");
        CHECK(file);
        uint32_t size;
        CHECK(afatfs_fGetSize(file, &size) && size == clusterSize * 4 + 216);
        readPattern(file, clusterSize * 3 + 123, 73);
        readPattern(file, clusterSize + 93, 91);
        closeFile(file);
    }
    unmountImage();
    sdcard_sim_destroy();
    printf("PASS external exFAT %s (cluster size %u)\n", argv[2], clusterSize);
    return 0;
}
