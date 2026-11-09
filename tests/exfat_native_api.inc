/* Native-Windows round-trip workload; uses only public library interfaces.
 * Included by test_exfat_external.c so the original simulated backend is shared. */
static afatfsRequest_t nativeRequest;
static afatfsError_e nativeResult;

static void nativeApiDone(afatfsError_e error, void *user)
{
    CHECK(user == &context);
    nativeResult = error;
    completed = true;
}
static void waitNativeApi(afatfsError_e expected)
{
    for (unsigned n = 0; !completed; n++) {
        CHECK(n < MAX_POLLS && !afatfs_canPowerOff());
        pump();
    }
    CHECK(nativeRequest.complete && nativeRequest.result == expected && nativeResult == expected);
    CHECK(!afatfs_canPowerOff());
}
#define NATIVE_API(call, expected)                                                                                     \
    do {                                                                                                               \
        completed = false;                                                                                             \
        CHECK(call);                                                                                                   \
        waitNativeApi(expected);                                                                                       \
    } while (0)
#define NATIVE_OK(call) NATIVE_API(call, AFATFS_ERROR_NONE)

static afatfsFilePtr_t nativeApiOpen(const char *name, const char *mode)
{
    afatfsFilePtr_t file = NULL;
    action = name;
    NATIVE_OK(afatfs_open(&nativeRequest, name, mode, &file, nativeApiDone, &context));
    CHECK(file);
    return file;
}
static void nativeApiClose(afatfsFilePtr_t file)
{
    NATIVE_OK(afatfs_close(&nativeRequest, file, nativeApiDone, &context));
}
static void nativeApiSeek(afatfsFilePtr_t file, uint32_t offset)
{
    completed = false;
    CHECK(afatfs_fseek(file, (int32_t)offset, AFATFS_SEEK_SET, fileDone, &context) != AFATFS_OPERATION_FAILURE);
    waitDone();
    uint32_t actual;
    CHECK(afatfs_ftell(file, &actual) && actual == offset);
}
static void nativeApiWrite(afatfsFilePtr_t file, uint32_t count, unsigned tag)
{
    uint8_t buffer[1024];
    for (uint32_t offset = 0; offset < count;) {
        uint32_t amount = count - offset < sizeof(buffer) ? count - offset : sizeof(buffer);
        for (unsigned n = 0; n < amount; n++) {
            buffer[n] = pattern(offset + n, tag);
        }
        NATIVE_OK(afatfs_write(&nativeRequest, file, buffer, amount, nativeApiDone, &context));
        CHECK(nativeRequest.transferred == amount);
        offset += amount;
    }
}
static void nativeApiVerify(const char *name, uint32_t count, unsigned tag)
{
    afatfsFilePtr_t file = nativeApiOpen(name, "r");
    afatfsFileInfo_t info;
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && info.size == count);
    uint8_t buffer[777];
    for (uint32_t offset = 0; offset < count;) {
        uint32_t amount = count - offset < sizeof(buffer) ? count - offset : sizeof(buffer);
        NATIVE_OK(afatfs_read(&nativeRequest, file, buffer, amount, nativeApiDone, &context));
        CHECK(nativeRequest.transferred == amount);
        for (unsigned n = 0; n < amount; n++) {
            CHECK(buffer[n] == pattern(offset + n, tag));
        }
        offset += amount;
    }
    CHECK(afatfs_feof(file));
    nativeApiClose(file);
}

static void exerciseNativeApi(unsigned clusterSize)
{
    afatfsFileInfo_t info;
    afatfsVolumeInfo_t before;
    afatfsVolumeInfo_t after;
    afatfsFilePtr_t file = nativeApiOpen("api native truncate.bin", "r+");
    uint32_t original = clusterSize * 3 + 99;
    nativeApiSeek(file, original);
    NATIVE_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && info.size == original);
    nativeApiSeek(file, clusterSize + 17);
    NATIVE_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    uint32_t cursor;
    CHECK(afatfs_ftell(file, &cursor) && cursor == clusterSize + 17);
    nativeApiClose(file);
    NATIVE_OK(
        afatfs_rename(&nativeRequest, "api native truncate.bin", "api renamed truncate.bin", nativeApiDone, &context));
    nativeApiVerify("api renamed truncate.bin", clusterSize + 17, 101);
    NATIVE_OK(afatfs_utime(&nativeRequest, "api renamed truncate.bin", 0x58221883, nativeApiDone, &context));
    NATIVE_OK(afatfs_chmod(&nativeRequest, "api renamed truncate.bin", FAT_FILE_ATTRIBUTE_HIDDEN,
        FAT_FILE_ATTRIBUTE_HIDDEN, nativeApiDone, &context));
    NATIVE_OK(afatfs_rename(&nativeRequest, "api native rename.bin", "api renamed file.bin", nativeApiDone, &context));
    NATIVE_API(
        afatfs_rename(&nativeRequest, "api renamed file.bin", "api native collision.bin", nativeApiDone, &context),
        AFATFS_ERROR_EXISTS);
    NATIVE_API(afatfs_rename(&nativeRequest, "api missing.bin", "api unexpected.bin", nativeApiDone, &context),
        AFATFS_ERROR_NOT_FOUND);
    NATIVE_OK(afatfs_rename(&nativeRequest, "API native directory", "API renamed directory", nativeApiDone, &context));

    file = nativeApiOpen("api native fragmented.bin", "r+");
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && !info.contiguous);
    nativeApiSeek(file, clusterSize - 7);
    NATIVE_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    nativeApiClose(file);
    NATIVE_OK(afatfs_rename(
        &nativeRequest, "api native fragmented.bin", "api renamed fragmented.bin", nativeApiDone, &context));

    file = nativeApiOpen("api hint.bin", "x+");
    NATIVE_OK(afatfs_getfree(&nativeRequest, &before, nativeApiDone, &context));
    NATIVE_OK(afatfs_expand(&nativeRequest, file, clusterSize * 3 + 123, false, nativeApiDone, &context));
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && !info.size && !info.firstCluster);
    NATIVE_OK(afatfs_getfree(&nativeRequest, &after, nativeApiDone, &context));
    CHECK(before.freeClusters == after.freeClusters);
    nativeApiClose(file);

    file = nativeApiOpen("api preallocated.bin", "x+");
    NATIVE_OK(afatfs_getfree(&nativeRequest, &before, nativeApiDone, &context));
    NATIVE_OK(afatfs_expand(&nativeRequest, file, clusterSize * 3 + 123, true, nativeApiDone, &context));
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && info.size == clusterSize * 3 + 123 &&
        info.contiguous && !info.validSize && info.allocatedSize == clusterSize * 4);
    NATIVE_OK(afatfs_getfree(&nativeRequest, &after, nativeApiDone, &context));
    CHECK(before.freeClusters - after.freeClusters == 4);
    nativeApiClose(file);

    file = nativeApiOpen("api prealloc clipped.bin", "x+");
    NATIVE_OK(afatfs_expand(&nativeRequest, file, clusterSize * 3 + 123, true, nativeApiDone, &context));
    nativeApiWrite(file, clusterSize + 37, 113);
    nativeApiSeek(file, clusterSize - 7);
    NATIVE_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    CHECK(afatfs_ftell(file, &cursor) && cursor == clusterSize - 7);
    nativeApiClose(file);

    file = nativeApiOpen("api zero truncate.bin", "x+");
    nativeApiWrite(file, 1003, 117);
    nativeApiSeek(file, 0);
    NATIVE_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    nativeApiClose(file);
    NATIVE_OK(afatfs_unlink(&nativeRequest, "api native empty.bin", nativeApiDone, &context));
    file = nativeApiOpen("api text.txt", "x+");
    char text[80];
    NATIVE_OK(afatfs_printf(&nativeRequest, file, text, sizeof(text), nativeApiDone, &context, "value=%d\n", 42));
    NATIVE_OK(afatfs_puts(&nativeRequest, file, "line two\n", nativeApiDone, &context));
    NATIVE_OK(afatfs_sync(&nativeRequest, file, nativeApiDone, &context));
    nativeApiClose(file);
    afatfsDirectory_t directory;
    NATIVE_OK(afatfs_findfirst(&nativeRequest, &directory, &info, ".", "api*.bin", nativeApiDone, &context));
    CHECK(info.name[0]);
    CHECK(afatfs_closedir(&directory) == AFATFS_OPERATION_SUCCESS);
    NATIVE_OK(afatfs_getlabel(&nativeRequest, &after, nativeApiDone, &context));
    CHECK(!strcmp(after.label, "AFATFS_TEST"));
    char cwd[80];
    NATIVE_OK(afatfs_getcwd(&nativeRequest, cwd, sizeof(cwd), nativeApiDone, &context));
    CHECK(!strcmp(cwd, "/"));
    unmountImage();
    CHECK(afatfs_canPowerOff());
    mountImage();
    nativeApiVerify("api renamed truncate.bin", clusterSize + 17, 101);
    nativeApiVerify("api renamed file.bin", 1203, 103);
    nativeApiVerify("api prealloc clipped.bin", clusterSize - 7, 113);
    nativeApiVerify("api renamed fragmented.bin", clusterSize - 7, 149);
    puts("PASS extended native API: cursor truncation, rename, preallocation, metadata, directory and text operations");
}

static void verifyNativeApiRoundtrip(unsigned clusterSize)
{
    nativeApiVerify("api windows renamed.bin", 1203, 103);
    nativeApiVerify("api renamed truncate.bin", 333, 101);
    nativeApiVerify("api prealloc clipped.bin", clusterSize - 7, 113);
    afatfsFileInfo_t info;
    NATIVE_API(
        afatfs_stat(&nativeRequest, "api renamed file.bin", &info, nativeApiDone, &context), AFATFS_ERROR_NOT_FOUND);
    afatfsFilePtr_t file = nativeApiOpen("api preallocated.bin", "r");
    CHECK(afatfs_fstat(file, &info) == AFATFS_OPERATION_SUCCESS && info.size == clusterSize * 4 + 220);
    uint8_t buffer[777];
    uint32_t size = info.size;
    uint32_t original = clusterSize * 3 + 123;
    for (uint32_t offset = 0; offset < size;) {
        uint32_t amount = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
        NATIVE_OK(afatfs_read(&nativeRequest, file, buffer, amount, nativeApiDone, &context));
        CHECK(nativeRequest.transferred == amount);
        for (unsigned n = 0; n < amount; n++) {
            uint32_t position = offset + n;
            uint8_t expected = 0;
            if (position >= original) {
                expected = pattern(position - original, 137);
            } else if (position >= clusterSize + 11 && position < clusterSize + 54) {
                expected = pattern(position - clusterSize - 11, 131);
            }
            CHECK(buffer[n] == expected);
        }
        offset += amount;
    }
    nativeApiClose(file);
    file = nativeApiOpen("api text.txt", "r");
    char text[80];
    const char *lines[] = {"value=42\n", "line two\n", "native\n"};
    for (unsigned n = 0; n < 3; n++) {
        NATIVE_OK(afatfs_gets(&nativeRequest, file, text, sizeof(text), nativeApiDone, &context));
        CHECK(!strcmp(text, lines[n]));
    }
    nativeApiClose(file);
    puts(
        "PASS extended native API roundtrip: Windows rename/truncate, preallocated zero gaps, overwrite and growth beyond allocation");
}
#undef NATIVE_OK
#undef NATIVE_API
