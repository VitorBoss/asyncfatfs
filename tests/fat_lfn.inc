// Run on FAT16 and FAT32, including fixed roots, cluster crossings and delayed I/O.
static void lfnSaveImage(const char *stage)
{
    const char *directory = getenv("AFATFS_LFN_IMAGE_DIR");
    if (!directory) {
        return;
    }
    const fatVolumeID_t *volume = (const fatVolumeID_t *)(media + PARTITION * SECTOR);
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/lfn-fat%u-spc%u-%s.img", directory, volume->fat.FATSize16 ? 16U : 32U,
        volume->fat.sectorsPerCluster, stage);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *output = fopen(path, "wb");
    assert(output);
    size_t bytes = (size_t)volume->fat.totalSectors32 * SECTOR;
    assert(bytes <= sizeof(media) - PARTITION * SECTOR);
    assert(fwrite(media + PARTITION * SECTOR, 1, bytes, output) == bytes);
    assert(!fclose(output));
}

static void lfnStat(afatfsFilePtr_t file, afatfsFileInfo_t *info)
{
    afatfsOperationStatus_e status;
    for (unsigned retry = 0; (status = afatfs_fstat(file, info)) == AFATFS_OPERATION_IN_PROGRESS; retry++) {
        assert(retry < 100000);
        assert(!afatfs_canPowerOff());
        pump();
    }
    assert(status == AFATFS_OPERATION_SUCCESS);
}

static void lfnAliasName(afatfsFilePtr_t file, char *name)
{
    fatDirectoryEntry_t *entry;
    for (unsigned retry = 0; !afatfs_fGetFileInfo(file, &entry); retry++) {
        assert(retry < 100000);
        pump();
    }
    unsigned written = 0;
    for (unsigned index = 0; index < 11; index++) {
        if (index == 8 && entry->fileName[index] != ' ') {
            name[written++] = '.';
        }
        if (entry->fileName[index] != ' ') {
            name[written++] = entry->fileName[index];
        }
    }
    name[written] = 0;
}

static void lfnDirectoryParent(afatfsFilePtr_t directory, uint32_t expectedParent)
{
    afatfsFileInfo_t info;
    lfnStat(directory, &info);
    afatfsFinder_t finder;
    afatfs_findFirst(directory, &finder);
    for (unsigned index = 0; index < 2; index++) {
        fatDirectoryEntry_t *entry;
        afatfsOperationStatus_e status;
        for (unsigned retry = 0; (status = afatfs_findNext(directory, &finder, &entry)) == AFATFS_OPERATION_IN_PROGRESS;
            retry++) {
            assert(retry < 100000);
            pump();
        }
        assert(status == AFATFS_OPERATION_SUCCESS && entry);
        assert(!memcmp(entry->fileName, index ? "..         " : ".          ", 11));
        uint32_t cluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
        assert(cluster == (index ? expectedParent : info.firstCluster));
    }
    afatfs_findLast(directory);
}

typedef struct lfnOpenResult_t {
    afatfsFilePtr_t file;
    bool complete;
} lfnOpenResult_t;

static void lfnOpened(afatfsFilePtr_t file, void *context)
{
    lfnOpenResult_t *result = (lfnOpenResult_t *)context;
    result->file = file;
    result->complete = true;
}

static void exerciseLfnConcurrent(void)
{
    lfnOpenResult_t results[3];
    memset(results, 0, sizeof(results));
    holdIO = true;
    assert(afatfs_fopen("Parallel long filename one.bin", "x+", lfnOpened, &results[0]));
    assert(afatfs_fopen("Parallel long filename two.bin", "x+", lfnOpened, &results[1]));
    assert(afatfs_fopen("Parallel long filename one.bin", "x+", lfnOpened, &results[2]));
    for (unsigned retry = 0; retry < 20; retry++) {
        pump();
        assert(!results[0].complete && !results[1].complete && !results[2].complete);
        assert(!afatfs_isIdle() && !afatfs_canPowerOff());
    }
    holdIO = false;
    for (unsigned retry = 0; !results[0].complete || !results[1].complete || !results[2].complete; retry++) {
        assert(retry < 100000);
        pump();
    }
    assert(results[0].file && results[1].file && !results[2].file);
    closeFile(results[0].file);
    closeFile(results[1].file);
    afatfsRequest_t request;
    API_TEST(afatfs_unlink(&request, "Parallel long filename one.bin", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "Parallel long filename two.bin", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    lfnOpenResult_t pending;
    memset(&pending, 0, sizeof(pending));
    memset(&backendFailure, 0, sizeof(backendFailure));
    backendFailure.kind = FAIL_WRITE_CALLBACK;
    backendFailure.target = TARGET_STOP;
    assert(afatfs_fopen("Metadata write retry long name.bin", "x+", lfnOpened, &pending));
    for (unsigned retry = 0; backendFailure.hits < 3; retry++) {
        assert(retry < 100000 && !pending.complete && !afatfs_canPowerOff());
        pump();
    }
    backendFailure.kind = FAIL_NONE;
    for (unsigned retry = 0; !pending.complete; retry++) {
        assert(retry < 100000);
        pump();
    }
    assert(pending.file);
    closeFile(pending.file);
    unmount();
    mount();
    API_TEST(
        afatfs_unlink(&request, "Metadata write retry long name.bin", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    puts("FAT LFN: concurrent creators, exclusive-name collision and failed metadata-write retries passed");
}

static void exerciseLfnBoundary(void)
{
    completed = false;
    assert(afatfs_mkdir("BOUNDARY", fileDone, &callbackContext));
    waitDone();
    assert(opened && afatfs_chdir(opened));
    closeFile(opened);
    char name[64];
    for (unsigned index = 0; index < 8; index++) {
        snprintf(name, sizeof(name), "PAD%u.TXT", index);
        afatfsFilePtr_t file = openFile(name, "x");
        assert(file);
        closeFile(file);
    }
    afatfsFilePtr_t blocker = openFile("BLOCKER.BIN", "x+");
    assert(blocker);
    uint8_t data[512];
    memset(data, 0x39, sizeof(data));
    writeBytes(blocker, data, sizeof(data));
    closeFile(blocker);
    char maximum[256];
    memset(maximum, 'Z', 255);
    maximum[255] = 0;
    afatfsFilePtr_t file = openFile(maximum, "x+");
    assert(file);
    writeBytes(file, data, sizeof(data));
    closeFile(file);
    // Dots + pads + blocker put the maximum-name short entry at slot 31.
    // Its following end marker is in another sector, or beyond the cluster chain.
    assert(afatfs_chdir(NULL));
    unmount();
    lfnSaveImage("boundary");
    mount();
    file = openFile("BOUNDARY", "r");
    assert(file && afatfs_chdir(file));
    closeFile(file);
    afatfsRequest_t request;
    afatfsFileInfo_t info;
    API_TEST(afatfs_stat(&request, maximum, &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, maximum) && info.size == sizeof(data));
    API_TEST(afatfs_unlink(&request, maximum, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    char aliases[14][13];
    for (unsigned index = 0; index < 14; index++) {
        snprintf(name, sizeof(name), "Collision long filename number %u.txt", index);
        file = openFile(name, "x");
        assert(file);
        lfnAliasName(file, aliases[index]);
        closeFile(file);
    }
    assert(!strcmp(aliases[8], "COLLIS~9.TXT"));
    assert(!strcmp(aliases[9], "COLLI~10.TXT"));
    for (unsigned index = 0; index < 14; index++) {
        API_TEST(afatfs_unlink(&request, aliases[index], apiDone, &callbackContext), AFATFS_ERROR_NONE);
    }
    API_TEST(afatfs_unlink(&request, "BLOCKER.BIN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    for (unsigned index = 0; index < 8; index++) {
        snprintf(name, sizeof(name), "PAD%u.TXT", index);
        API_TEST(afatfs_unlink(&request, name, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    }
    assert(afatfs_chdir(NULL));
    API_TEST(afatfs_rmdir(&request, "BOUNDARY", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    puts("FAT LFN: slot-31 publication, multi-sector/cluster names and two-digit alias collisions passed");
}

static void exerciseLfnForeign(void)
{
    afatfsRequest_t request;
    // Fixed UTF-16LE fixture, independently encoded from section 7 of the FAT specification.
    static const uint8_t entries[64] = {0x42, 0x77, 0x00, 0x6E, 0x00, 0x2E, 0x00, 0x66, 0x00, 0x6F, 0x00, 0x0F, 0x00,
        0x07, 0x78, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
        0xFF, 0x01, 0x54, 0x00, 0x68, 0x00, 0x65, 0x00, 0x20, 0x00, 0x71, 0x00, 0x0F, 0x00, 0x07, 0x75, 0x00, 0x69,
        0x00, 0x63, 0x00, 0x6B, 0x00, 0x20, 0x00, 0x62, 0x00, 0x00, 0x00, 0x72, 0x00, 0x6F, 0x00};
    completed = false;
    assert(afatfs_mkdir("FOREIGN", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    afatfsFileInfo_t info;
    lfnStat(opened, &info);
    uint32_t directoryCluster = info.firstCluster;
    assert(afatfs_chdir(opened));
    closeFile(opened);
    unmount();
    fatVolumeID_t *volume = (fatVolumeID_t *)(media + PARTITION * SECTOR);
    bool fat32 = volume->fat.FATSize16 == 0;
    uint32_t fatSectors = fat32 ? volume->fat.fatDescriptor.fat32.FATSize32 : volume->fat.FATSize16;
    uint32_t heap = PARTITION + volume->fat.reservedSectorCount + 2 * fatSectors +
        (volume->fat.rootEntryCount * 32 + SECTOR - 1) / SECTOR;
    uint8_t *directory = media + (heap + (directoryCluster - 2) * volume->fat.sectorsPerCluster) * SECTOR;
    static const unsigned badOffsets[] = {0, 32, 13, 45, 12, 26, 27, 18, 16, 44};
    static const uint8_t badValues[] = {2, 2, 8, 8, 1, 1, 1, 0, 0xFF, 1};
    for (unsigned variant = 0; variant < 14; variant++) {
        memcpy(directory + 64, entries, sizeof(entries));
        fatDirectoryEntry_t *shortEntry = (fatDirectoryEntry_t *)(directory + 128);
        memset(shortEntry, 0, sizeof(*shortEntry));
        memcpy(shortEntry->fileName, "THEQUI~1FOX", 11);
        shortEntry->attrib = FAT_FILE_ATTRIBUTE_ARCHIVE;
        memset(directory + 160, 0, 32);
        if (variant >= 1 && variant <= 10) {
            directory[64 + badOffsets[variant - 1]] = badValues[variant - 1];
        } else if (variant == 11) {
            directory[64 + 33] = 0xE1; // Valid non-ASCII LFN: preserve access through its alias.
        } else if (variant == 12) {
            shortEntry->ntReserved = 0x18; // Windows lowercase base and extension flags.
        } else if (variant == 13) {
            shortEntry->ntReserved = 0x18;
            directory[77] = 8; // Invalid LFN must fall back to the case-preserving short name.
        }
        mount();
        afatfsFilePtr_t parent = openFile("FOREIGN", "r");
        if (!parent) {
            fprintf(stderr, "foreign fixture variant %u: parent cluster %lu, heap %lu, error %u\n", variant,
                (unsigned long)directoryCluster, (unsigned long)heap, afatfs_getLastError());
        }
        assert(parent && afatfs_chdir(parent));
        closeFile(parent);
        afatfsFilePtr_t file = openFile("The quick brown.fox", "r");
        bool valid = variant == 0 || variant == 12;
        assert((file != NULL) == valid);
        if (file) {
            lfnStat(file, &info);
            assert(!strcmp(info.name, "The quick brown.fox"));
            closeFile(file);
        }
        file = openFile("THEQUI~1.FOX", "r");
        assert(file);
        lfnStat(file, &info);
        const char *expected = valid ? "The quick brown.fox" : variant == 13 ? "thequi~1.fox" : "THEQUI~1.FOX";
        assert(!strcmp(info.name, expected));
        closeFile(file);
        afatfsDirectory_t cursor;
        API_TEST(afatfs_opendir(&request, ".", &cursor, apiDone, &callbackContext), AFATFS_ERROR_NONE);
        afatfsOperationStatus_e status;
        for (unsigned retry = 0; (status = afatfs_readdir(&cursor, &info)) == AFATFS_OPERATION_IN_PROGRESS; retry++) {
            assert(retry < 100000);
            pump();
        }
        assert(status == AFATFS_OPERATION_SUCCESS);
        assert(!strcmp(info.name, expected));
        assert(afatfs_closedir(&cursor) == AFATFS_OPERATION_SUCCESS);
        if (variant == 13) {
            API_TEST(
                afatfs_rename(&request, "THEQUI~1.FOX", "CAPITAL.FOX", apiDone, &callbackContext), AFATFS_ERROR_NONE);
            API_TEST(afatfs_stat(&request, "CAPITAL.FOX", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
            assert(!strcmp(info.name, "CAPITAL.FOX"));
        }
        API_TEST(afatfs_unlink(&request, variant == 13 ? "CAPITAL.FOX" : "THEQUI~1.FOX", apiDone, &callbackContext),
            AFATFS_ERROR_NONE);
        assert(afatfs_chdir(NULL));
        unmount();
        // Only a structurally valid chain may be deleted alongside the short entry.
        assert(directory[128] == 0xE5);
        if (valid || variant == 11) {
            assert(directory[64] == 0xE5 && directory[96] == 0xE5);
        } else {
            assert(directory[64] != 0xE5);
        }
    }
    memcpy(directory + 64, entries, sizeof(entries));
    memset(directory + 128, 0, 64);
    directory[128] = 0xE5;
    mount();
    afatfsFilePtr_t parent = openFile("FOREIGN", "r");
    assert(parent && afatfs_chdir(parent));
    closeFile(parent);
    afatfsFilePtr_t replacement = openFile("THEQUI~1.FOX", "x");
    assert(replacement);
    closeFile(replacement);
    assert(afatfs_chdir(NULL));
    unmount();
    assert(directory[96] == 0xE5); // The new short entry cannot inherit the orphan's name.
    mount();
    parent = openFile("FOREIGN", "r");
    assert(parent && afatfs_chdir(parent));
    closeFile(parent);
    assert(!openFile("The quick brown.fox", "r"));
    API_TEST(afatfs_stat(&request, "THEQUI~1.FOX", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, "THEQUI~1.FOX"));
    API_TEST(afatfs_unlink(&request, "THEQUI~1.FOX", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(afatfs_chdir(NULL));
    unmount();
    memset(directory + 64, 0, 128);
    mount();
    API_TEST(afatfs_rmdir(&request, "FOREIGN", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    puts("FAT LFN foreign fixture: 14 valid/malformed/Unicode/case-flag cases and orphan reuse passed");
}

static void exerciseLfn(void)
{
    afatfsRequest_t request;
    afatfsFileInfo_t info;
    const char *names[] = {"First long filename.txt", "First long filenames.txt", "First long file notes.txt",
        "MixedCase.txt", "1234567890123", "12345678901234567890123456", ".hidden file", "multiple.parts.of.a.name.bin",
        "valid+comma,equals=brackets[].txt"};
    char aliases[9][13];
    const uint8_t payload[] = {0x18, 0xFA, 0x32, 0x90};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        afatfsFilePtr_t file = openFile(names[index], "x+");
        assert(file);
        writeBytes(file, payload, sizeof(payload));
        lfnAliasName(file, aliases[index]);
        lfnStat(file, &info);
        assert(!strcmp(info.name, names[index]) && info.size == sizeof(payload));
        closeFile(file);
    }
    assert(!strcmp(aliases[0], "FIRSTL~1.TXT"));
    assert(!strcmp(aliases[1], "FIRSTL~2.TXT"));
    assert(!strcmp(aliases[2], "FIRSTL~3.TXT"));
    unmount();
    lfnSaveImage("names");
    mount();
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        char spelling[256];
        strcpy(spelling, names[index]);
        for (unsigned offset = 0; spelling[offset]; offset++) {
            if (spelling[offset] >= 'A' && spelling[offset] <= 'Z') {
                spelling[offset] += 'a' - 'A';
            }
        }
        afatfsFilePtr_t file = openFile(spelling, "r");
        assert(file);
        uint8_t actual[sizeof(payload)];
        readBytes(file, actual, sizeof(actual));
        assert(!memcmp(actual, payload, sizeof(actual)));
        closeFile(file);
        file = openFile(aliases[index], "r");
        assert(file);
        lfnStat(file, &info);
        assert(!strcmp(info.name, names[index]));
        closeFile(file);
    }
    afatfsDirectory_t directory;
    API_TEST(afatfs_opendir(&request, ".", &directory, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    unsigned found = 0;
    for (;;) {
        afatfsOperationStatus_e status;
        unsigned retries = 0;
        while ((status = afatfs_readdir(&directory, &info)) == AFATFS_OPERATION_IN_PROGRESS) {
            assert(++retries < 100000);
            pump();
        }
        assert(status == AFATFS_OPERATION_SUCCESS);
        if (!info.name[0]) {
            break;
        }
        for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
            if (!strcmp(info.name, names[index])) {
                found++;
            }
        }
    }
    assert(found == sizeof(names) / sizeof(names[0]));
    assert(afatfs_closedir(&directory) == AFATFS_OPERATION_SUCCESS);
    API_TEST(afatfs_findfirst(&request, &directory, &info, ".", "First long file*.txt", apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, names[0]));
    assert(afatfs_closedir(&directory) == AFATFS_OPERATION_SUCCESS);
    API_TEST(afatfs_rename(&request, names[0], "Renamed long file.data", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!openFile(names[0], "r"));
    assert(!openFile(aliases[0], "r"));
    API_TEST(afatfs_stat(&request, "Renamed long file.data", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, "Renamed long file.data") && info.size == sizeof(payload));
    API_TEST(afatfs_rename(&request, "Renamed long file.data", "RENAMED LONG FILE.DATA", apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    API_TEST(afatfs_stat(&request, "Renamed long file.data", &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, "RENAMED LONG FILE.DATA") && info.size == sizeof(payload));
    API_TEST(
        afatfs_rename(&request, "Renamed long file.data", names[1], apiDone, &callbackContext), AFATFS_ERROR_EXISTS);
    API_TEST(
        afatfs_rename(&request, "Renamed long file.data", "SHORT.DAT", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_rename(&request, "SHORT.DAT", "Back to long filename.data", apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    API_TEST(afatfs_unlink(&request, "Back to long filename.data", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_rename(&request, names[5], aliases[5], apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_stat(&request, aliases[5], &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, aliases[5]));
    assert(!openFile(names[5], "r"));
    for (unsigned index = 1; index < sizeof(names) / sizeof(names[0]); index++) {
        // Deleting through an alias must also remove its LFN chain.
        API_TEST(afatfs_unlink(&request, aliases[index], apiDone, &callbackContext), AFATFS_ERROR_NONE);
        assert(!openFile(names[index], "r"));
    }
    char maximum[256];
    memset(maximum, 'X', 255);
    maximum[255] = 0;
    afatfsFilePtr_t file = openFile(maximum, "x+");
    assert(file);
    writeBytes(file, payload, sizeof(payload));
    closeFile(file);
    unmount();
    mount();
    API_TEST(afatfs_stat(&request, maximum, &info, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(info.name, maximum) && info.size == sizeof(payload));
    API_TEST(afatfs_unlink(&request, maximum, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    char tooLong[257];
    memset(tooLong, 'X', 256);
    tooLong[256] = 0;
    assert(!openFile(tooLong, "x"));
    assert(afatfs_getLastError() == AFATFS_ERROR_INVALID_NAME);
    const char *invalid[] = {
        "bad/name", "bad\\name", "bad*name", "bad?name", "bad:name", "tail.", "tail ", "..", "\xC3\xA1.txt"};
    for (unsigned index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
        assert(!openFile(invalid[index], "x"));
        assert(afatfs_getLastError() == AFATFS_ERROR_INVALID_NAME);
    }
    completed = false;
    assert(afatfs_mkdir("Long directory name", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    lfnDirectoryParent(opened, 0);
    lfnStat(opened, &info);
    uint32_t parentCluster = info.firstCluster;
    assert(afatfs_chdir(opened));
    closeFile(opened);
    completed = false;
    assert(afatfs_mkdir("Nested long directory", fileDone, &callbackContext));
    waitDone();
    assert(opened);
    lfnDirectoryParent(opened, parentCluster);
    closeFile(opened);
    API_TEST(afatfs_rmdir(&request, "Nested long directory", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    file = openFile("Child with long filename.bin", "x+");
    assert(file);
    closeFile(file);
    char path[256];
    API_TEST(afatfs_getcwd(&request, path, sizeof(path), apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!strcmp(path, "/Long directory name"));
    API_TEST(afatfs_unlink(&request, "Child with long filename.bin", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(afatfs_chdir(NULL));
    API_TEST(afatfs_rename(&request, "Long directory name", "Renamed long directory", apiDone, &callbackContext),
        AFATFS_ERROR_NONE);
    API_TEST(afatfs_open(&request, "Renamed long directory", "r", &file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(file);
    API_TEST(afatfs_sync(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    assert(!afatfs_truncate(&request, file, apiDone, &callbackContext));
    API_TEST(afatfs_close(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_open(&request, ".", "r", &file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_sync(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_close(&request, file, apiDone, &callbackContext), AFATFS_ERROR_NONE);
    API_TEST(afatfs_rmdir(&request, "Renamed long directory", apiDone, &callbackContext), AFATFS_ERROR_NONE);
    puts(
        "FAT LFN: aliases, collisions, case, enumeration, stat, rename, unlink, 255 characters and directories passed");
    exerciseLfnForeign();
    exerciseLfnConcurrent();
    exerciseLfnBoundary();
}
