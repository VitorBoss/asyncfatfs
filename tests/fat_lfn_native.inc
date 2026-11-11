/* Native FAT32/VFAT round trips, using only public AsyncFatFS APIs and the existing image backend. */
#define LFN_OK(call)                                                                                                   \
    do {                                                                                                               \
        completed = false;                                                                                             \
        CHECK(call);                                                                                                   \
        waitNativeApi(AFATFS_ERROR_NONE);                                                                              \
    } while (0)

static void nativeLfnCheckName(const char *name, const char *expected, uint32_t size)
{
    afatfsFilePtr_t file = nativeApiOpen(name, "r");
    afatfsFileInfo_t info;
    nativeApiStat(file, &info);
    CHECK(!strcmp(info.name, expected) && info.size == size);
    nativeApiClose(file);
}

static void exerciseNativeLfn(unsigned clusterSize, const char *manifest)
{
    action = "Windows alias manifest";
    FILE *input = fopen(manifest, "r");
    CHECK(input);
    char line[1024];
    unsigned count = 0;
    while (fgets(line, sizeof(line), input)) {
        char *name = strchr(line, '|');
        CHECK(name);
        *name++ = 0;
        char *sizeText = strchr(name, '|');
        CHECK(sizeText);
        *sizeText++ = 0;
        char *tagText = strchr(sizeText, '|');
        CHECK(tagText);
        *tagText++ = 0;
        uint32_t size = (uint32_t)strtoul(sizeText, NULL, 10);
        unsigned tag = (unsigned)strtoul(tagText, NULL, 10);
        verifyFile(name, size, tag);
        verifyFile(line, size, tag);
        nativeLfnCheckName(line, name, size);
        if (!strcmp(name, "Windows delete by alias long filename.bin")) {
            LFN_OK(afatfs_unlink(&nativeRequest, line, nativeApiDone, &context));
        }
        count++;
    }
    CHECK(!ferror(input) && !fclose(input) && count == 19);
    LFN_OK(afatfs_rename(&nativeRequest, "Windows mixed Case long filename.txt", "Library renamed native filename.data",
        nativeApiDone, &context));
    afatfsFilePtr_t file = nativeApiOpen("Windows truncate long filename.bin", "r+");
    nativeApiSeek(file, 333);
    LFN_OK(afatfs_truncate(&nativeRequest, file, nativeApiDone, &context));
    nativeApiClose(file);
    for (unsigned index = 0; index < 14; index++) {
        char name[80];
        snprintf(name, sizeof(name), "Collision library filename number %02u.bin", index);
        file = nativeApiOpen(name, "x+");
        writePattern(file, 97 + index, 160 + index);
        nativeApiClose(file);
    }
    file = nativeApiOpen("Library Case Change.txt", "x+");
    writePattern(file, 193, 201);
    nativeApiClose(file);
    LFN_OK(
        afatfs_rename(&nativeRequest, "Library Case Change.txt", "LIBRARY CASE CHANGE.TXT", nativeApiDone, &context));
    const char *lengths[] = {"1234567890123", "12345678901234567890123456"};
    for (unsigned index = 0; index < 2; index++) {
        file = nativeApiOpen(lengths[index], "x+");
        writePattern(file, 27, 210 + index);
        nativeApiClose(file);
    }
    afatfsFilePtr_t directory = openDirectory("Library empty long directory");
    closeFile(directory);
    LFN_OK(afatfs_rename(
        &nativeRequest, "Library empty long directory", "Library renamed empty directory", nativeApiDone, &context));
    directory = openDirectory("Library nested directory");
    CHECK(afatfs_chdir(directory));
    closeFile(directory);
    directory = openDirectory("Library child directory");
    CHECK(afatfs_chdir(directory));
    closeFile(directory);
    file = nativeApiOpen("Child long filename.bin", "x+");
    writePattern(file, clusterSize + 13, 221);
    nativeApiClose(file);
    CHECK(afatfs_chdir(NULL));
    afatfsDirectory_t cursor;
    afatfsFileInfo_t info;
    LFN_OK(afatfs_findfirst(&nativeRequest, &cursor, &info, ".", "Collision library*.bin", nativeApiDone, &context));
    count = 0;
    while (info.name[0]) {
        count++;
        afatfsOperationStatus_e status;
        for (unsigned retry = 0; (status = afatfs_findnext(&cursor, &info)) == AFATFS_OPERATION_IN_PROGRESS; retry++) {
            CHECK(retry < MAX_POLLS);
            pump();
        }
        CHECK(status == AFATFS_OPERATION_SUCCESS);
    }
    CHECK(count == 14 && afatfs_closedir(&cursor) == AFATFS_OPERATION_SUCCESS);
    puts(
        "PASS native FAT32 LFN: Windows aliases, native collisions, long-name truncate/rename, case, directories and enumeration");
}

static void verifyNativeLfnRoundtrip(void)
{
    verifyFile("Windows renamed library output.data", 731, 231);
    nativeLfnCheckName("Windows renamed library output.data", "Windows renamed library output.data", 731);
    verifyFile("Windows renamed maximum filename.bin", 777, 55);
    CHECK(!openFile("LIBRARY CASE CHANGE.TXT", "r"));
    afatfsFilePtr_t directory = nativeApiOpen("Windows renamed nested directory", "r");
    CHECK(afatfs_chdir(directory));
    nativeApiClose(directory);
    directory = nativeApiOpen("Library child directory", "r");
    CHECK(afatfs_chdir(directory));
    nativeApiClose(directory);
    verifyFile("Native child created after rename.bin", 777, 93);
    char cwd[256];
    LFN_OK(afatfs_getcwd(&nativeRequest, cwd, sizeof(cwd), nativeApiDone, &context));
    CHECK(!strcmp(cwd, "/Windows renamed nested directory/Library child directory"));
    afatfsFilePtr_t file = nativeApiOpen("Library final native roundtrip.bin", "x+");
    writePattern(file, 701, 239);
    nativeApiClose(file);
    CHECK(afatfs_chdir(NULL));
    puts("PASS native FAT32 LFN roundtrip: Windows rename/delete, maximum name, nested path and final library write");
}

typedef struct nativeLfnPending_t {
    afatfsFilePtr_t file;
    uint32_t offset;
    bool complete;
} nativeLfnPending_t;

static const char *nativeConcurrentNames[] = {
    "Concurrent library collision filename number 00.bin",
    "Concurrent library collision filename number 01.bin",
    "Concurrent library collision filename number 02.bin",
};
static const char *nativeConcurrentRenamed = "Concurrent library renamed while writers pending.bin";

static void nativeConcurrentOpened(afatfsFilePtr_t file, void *user)
{
    nativeLfnPending_t *pending = user;
    CHECK(file);
    pending->file = file;
    pending->complete = true;
}

static void nativeConcurrentClosed(void *user)
{
    nativeLfnPending_t *pending = user;
    pending->complete = true;
}

static void nativeConcurrentWrite(nativeLfnPending_t *pending, uint32_t size, unsigned tag)
{
    uint8_t buffer[2048];
    uint32_t amount = MIN((uint32_t)sizeof(buffer), size - pending->offset);
    for (unsigned index = 0; index < amount; index++) {
        buffer[index] = pattern(pending->offset + index, tag);
    }
    pending->offset += afatfs_fwrite(pending->file, buffer, amount);
}

static void exerciseNativeLfnConcurrent(void)
{
    action = "concurrent LFN creation, rename, writes and prefetch";
    verifyFile("Windows concurrent seed first.bin", 33001, 71);
    verifyFile("Windows concurrent seed second.bin", 49003, 81);
    nativeLfnPending_t pending[3] = {0};
    for (unsigned index = 0; index < 3; index++) {
        CHECK(afatfs_fopen(nativeConcurrentNames[index], "x+", nativeConcurrentOpened, &pending[index]));
    }
    CHECK(!pending[0].complete && !pending[1].complete && !pending[2].complete);
    for (unsigned retry = 0; !pending[0].complete || !pending[1].complete || !pending[2].complete; retry++) {
        CHECK(retry < MAX_POLLS && !afatfs_canPowerOff());
        pump();
    }
    writePattern(pending[2].file, 2300, 183);
    closeFile(pending[2].file);
    LFN_OK(afatfs_expand(&nativeRequest, pending[0].file, 65536, true, nativeApiDone, &context));
    // Rename uses two temporary handles. Leave them available within the default
    // three-handle limit, then reopen the second writer for the streaming phase.
    writePattern(pending[1].file, 1025, 77);
    closeFile(pending[1].file);
    pending[1].offset = 1025;
    completed = false;
    CHECK(afatfs_rename(&nativeRequest, nativeConcurrentNames[2], nativeConcurrentRenamed, nativeApiDone, &context));
    unsigned overlapPolls = 0;
    for (; !completed; overlapPolls++) {
        CHECK(overlapPolls < MAX_POLLS && !afatfs_canPowerOff());
        nativeConcurrentWrite(&pending[0], 65536, 41);
        pump();
    }
    CHECK(nativeResult == AFATFS_ERROR_NONE && nativeRequest.complete && overlapPolls > 0);
    pending[1].file = openFile(nativeConcurrentNames[1], "a");
    CHECK(pending[1].file);
    afatfsFilePtr_t reader = openFile("Windows concurrent seed first.bin", "r");
    CHECK(reader);
    afatfs_fprefetch(reader);
    uint32_t readOffset = 0;
    for (unsigned retry = 0; pending[0].offset < 65536 || pending[1].offset < 32768 + 333 || readOffset < 33001;
        retry++) {
        CHECK(retry < MAX_POLLS && !afatfs_canPowerOff());
        nativeConcurrentWrite(&pending[0], 65536, 41);
        nativeConcurrentWrite(&pending[1], 32768 + 333, 77);
        uint8_t buffer[277];
        uint32_t amount = MIN((uint32_t)sizeof(buffer), 33001 - readOffset);
        unsigned received = afatfs_fread(reader, buffer, amount);
        for (unsigned index = 0; index < received; index++) {
            CHECK(buffer[index] == pattern(readOffset + index, 71));
        }
        readOffset += received;
        afatfs_fprefetch(reader);
        pump();
    }
    for (unsigned index = 0; index < 2; index++) {
        uint32_t position;
        for (unsigned retry = 0; !afatfs_ftell(pending[index].file, &position); retry++) {
            CHECK(retry < MAX_POLLS);
            pump();
        }
        if (position != pending[index].offset) {
            fprintf(stderr, "Concurrent writer %u: cursor %u, accepted offset %u\n", index, position, pending[index].offset);
        }
        CHECK(position == pending[index].offset);
    }
    pending[2].file = reader;
    for (unsigned index = 0; index < 3; index++) {
        pending[index].complete = false;
        CHECK(afatfs_fclose(pending[index].file, nativeConcurrentClosed, &pending[index]));
    }
    for (unsigned retry = 0; !pending[0].complete || !pending[1].complete || !pending[2].complete; retry++) {
        CHECK(retry < MAX_POLLS && !afatfs_canPowerOff());
        pump();
    }
    verifyFile(nativeConcurrentNames[0], 65536, 41);
    verifyFile(nativeConcurrentNames[1], 32768 + 333, 77);
    verifyFile(nativeConcurrentRenamed, 2300, 183);
    CHECK(!openFile(nativeConcurrentNames[2], "r"));
    printf(
        "PASS concurrent native LFN: 3 pending creates/closes, 2 writers, prefetch reader, rename overlap %u polls\n",
        overlapPolls);
}

static void verifyNativeLfnConcurrentRoundtrip(void)
{
    verifyFile(nativeConcurrentNames[0], 6101, 91);
    verifyFile(nativeConcurrentNames[1], 8103, 92);
    verifyFile(nativeConcurrentRenamed, 2300, 183);
    afatfsFilePtr_t file = openFile("Concurrent library final roundtrip.bin", "x+");
    CHECK(file);
    writePattern(file, 731, 94);
    closeFile(file);
    puts("PASS concurrent native LFN roundtrip: Windows parallel overwrites and final library write");
}
#undef LFN_OK
