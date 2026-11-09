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
#undef LFN_OK
