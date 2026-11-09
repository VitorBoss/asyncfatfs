/* VFAT support uses the original CWD walker, cache and per-file operation union.
 * Only the requested name lives in the operation workspace; sectors are streamed.
 * See Microsoft FAT specification, section 7 (ordinals, checksum and padding). */
static const uint8_t afatfs_lfnOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static unsigned afatfs_lfnUpper(unsigned character)
{
    return character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character;
}

static bool afatfs_lfnValidName(const char *name)
{
    size_t length = strlen(name);
    if (!length || length > 255 || !strcmp(name, "..") || name[length - 1] == '.' || name[length - 1] == ' ') {
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned character = (uint8_t)name[index];
        if (character < 32 || character >= 127 || strchr("\"*/:<>?\\|", character)) {
            return false;
        }
    }
    return true;
}

static uint16_t afatfs_lfnUnit(const uint8_t *entry, unsigned index)
{
    unsigned offset = afatfs_lfnOffsets[index];
    return entry[offset] | ((uint16_t)entry[offset + 1] << 8);
}

// Invalid/orphan chains fall back to the short entry; they never name another file.
static void afatfs_lfnConsume(afatfsLongNameState_t *state, const uint8_t *entry, const char *wanted, char *output)
{
    unsigned ordinal = entry[0] & 0x1F;
    if (entry[0] & FAT_LONG_FILENAME_ENTRY) {
        memset(state, 0, sizeof(*state));
        if (!ordinal || ordinal > 20) {
            return;
        }
        unsigned length = ordinal * 13;
        for (unsigned index = 0; index < 13; index++) {
            if (!afatfs_lfnUnit(entry, index)) {
                length = (ordinal - 1) * 13 + index;
                break;
            }
        }
        if (!length || length > 255 || length <= (ordinal - 1) * 13) {
            return;
        }
        state->remaining = (uint8_t)ordinal;
        state->count = (uint8_t)ordinal;
        state->length = (uint8_t)length;
        state->checksum = entry[13];
        state->matched = !wanted || strlen(wanted) == length;
        state->ascii = true;
        if (output) {
            output[length] = 0;
        }
    }
    if (!ordinal || ordinal != state->remaining || (entry[0] & 0xA0) || entry[11] != FAT_FILE_ATTRIBUTE_LONGNAME ||
        entry[12] || entry[26] || entry[27] || entry[13] != state->checksum) {
        memset(state, 0, sizeof(*state));
        return;
    }
    for (unsigned index = 0; index < 13; index++) {
        unsigned position = (ordinal - 1) * 13 + index;
        uint16_t character = afatfs_lfnUnit(entry, index);
        if (position >= state->length) {
            if (character != (position == state->length ? 0 : 0xFFFF)) {
                memset(state, 0, sizeof(*state));
                return;
            }
        } else {
            if (!character || character == 0xFFFF) {
                memset(state, 0, sizeof(*state));
                return;
            }
            if (character < 32 || character >= 127 || strchr("\"*/:<>?\\|", character)) {
                state->ascii = false;
                state->matched = false;
            }
            if (wanted && state->matched && afatfs_lfnUpper(character) != afatfs_lfnUpper((uint8_t)wanted[position])) {
                state->matched = false;
            }
            if (output) {
                output[position] = character < 128 ? (char)character : '?';
            }
        }
    }
    state->remaining--;
}

static bool afatfs_lfnAttached(const afatfsLongNameState_t *state, const uint8_t *entry)
{
    return state->count && !state->remaining && state->checksum == fat_createCheckSum((const char *)entry);
}

// Returns true for a legal 8.3 spelling, even if a case-preserving LFN is needed.
static bool afatfs_lfnShortName(const char *name, uint8_t *output)
{
    unsigned base = 0;
    unsigned extension = 0;
    bool dotted = false;
    memset(output, ' ', 11);
    for (unsigned index = 0; name[index]; index++) {
        unsigned character = (uint8_t)name[index];
        if (character == '.') {
            if (dotted || !base) {
                return false;
            }
            dotted = true;
        } else {
            if (character <= 32 || character >= 127 || strchr("\"*+,/:;<=>?[\\]|", character)) {
                return false;
            }
            if (dotted) {
                if (extension == 3) {
                    return false;
                }
                output[8 + extension++] = (uint8_t)afatfs_lfnUpper(character);
            } else {
                if (base == 8) {
                    return false;
                }
                output[base++] = (uint8_t)afatfs_lfnUpper(character);
            }
        }
    }
    return base && (!dotted || extension);
}

static void afatfs_lfnAlias(afatfsCreateFile_t *state)
{
    const char *name = (const char *)state->fileName;
    const char *extension = strrchr(name, '.');
    char digits[7];
    unsigned length = 0;
    uint32_t number = state->location.aliasNumber;
    do {
        digits[length++] = (char)('0' + number % 10);
        number /= 10;
    } while (number);
    memset(state->shortName, ' ', 11);
    unsigned written = 0;
    for (const char *cursor = name; *cursor && cursor != extension && written < 7 - length; cursor++) {
        unsigned character = (uint8_t)*cursor;
        if (character == ' ' || character == '.') {
            continue;
        }
        state->shortName[written++] = (uint8_t)(strchr("+,;=[]", character) ? '_' : afatfs_lfnUpper(character));
    }
    if (!written) {
        state->shortName[written++] = '_';
    }
    state->shortName[written++] = '~';
    while (length) {
        state->shortName[written++] = (uint8_t)digits[--length];
    }
    if (extension) {
        written = 8;
        for (extension++; *extension && written < 11; extension++) {
            unsigned character = (uint8_t)*extension;
            if (character != ' ' && character != '.') {
                state->shortName[written++] = (uint8_t)(strchr("+,;=[]", character) ? '_' : afatfs_lfnUpper(character));
            }
        }
    }
}

static void afatfs_lfnRememberStart(afatfsFilePtr_t file, const afatfsFinder_t *finder)
{
    afatfsFilePtr_t directory = &afatfs.currentDirectory;
    file->lfnCluster = directory->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY ? 0 : directory->cursorCluster;
    file->lfnOffset = directory->cursorOffset + (uint32_t)finder->entryIndex * 32;
    if (file->lfnCluster) {
        file->lfnOffset %= afatfs_clusterSize();
    }
}

// Resolve at most two cluster boundaries, including fragmented directories.
static afatfsOperationStatus_e afatfs_lfnEntry(afatfsFilePtr_t file, unsigned index, uint8_t **entry, bool writing)
{
    uint32_t offset = file->lfnOffset + index * 32;
    uint32_t cluster = file->lfnCluster;
    uint32_t sector;
    if (!cluster) {
        if (offset / AFATFS_SECTOR_SIZE >= afatfs.rootDirectorySectors) {
            if (index == (unsigned)file->lfnEntryCount + 1) {
                *entry = NULL;
                return AFATFS_OPERATION_SUCCESS;
            }
            return AFATFS_OPERATION_FAILURE;
        }
        sector = afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + offset / AFATFS_SECTOR_SIZE;
    } else {
        while (offset >= afatfs_clusterSize()) {
            uint32_t next;
            afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, cluster, &next);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (next < 2 || next - 2 >= afatfs.numClusters) {
                if (afatfs_FATIsEndOfChainMarker(next) && index == (unsigned)file->lfnEntryCount + 1) {
                    *entry = NULL;
                    return AFATFS_OPERATION_SUCCESS;
                }
                return AFATFS_OPERATION_FAILURE;
            }
            cluster = next;
            offset -= afatfs_clusterSize();
        }
        sector = afatfs_fileClusterToPhysical(cluster, offset / AFATFS_SECTOR_SIZE);
    }
    uint8_t *buffer;
    afatfsOperationStatus_e status = afatfs_cacheSector(
        sector, &buffer, AFATFS_CACHE_READ | (writing ? AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA : 0), 0);
    if (status == AFATFS_OPERATION_SUCCESS) {
        *entry = buffer + offset % AFATFS_SECTOR_SIZE;
    }
    return status;
}

static void afatfs_lfnWrite(uint8_t *entry, const char *name, unsigned ordinal, unsigned count, uint8_t checksum)
{
    size_t length = strlen(name);
    memset(entry, 0, 32);
    entry[0] = (uint8_t)(ordinal | (ordinal == count ? FAT_LONG_FILENAME_ENTRY : 0));
    entry[11] = FAT_FILE_ATTRIBUTE_LONGNAME;
    entry[13] = checksum;
    for (unsigned index = 0; index < 13; index++) {
        unsigned position = (ordinal - 1) * 13 + index;
        uint16_t character = position < length ? (uint8_t)name[position] : position == length ? 0 : 0xFFFF;
        entry[afatfs_lfnOffsets[index]] = (uint8_t)character;
        entry[afatfs_lfnOffsets[index] + 1] = (uint8_t)(character >> 8);
    }
}

static afatfsOperationStatus_e afatfs_lfnDelete(afatfsFilePtr_t file)
{
    // The short entry must be durable before its long-name slots can be reused.
    if (file->lfnDeleteIndex == 1 && !afatfs_flushCache()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    while (file->lfnDeleteIndex <= file->lfnEntryCount) {
        uint8_t *entry;
        afatfsOperationStatus_e status = afatfs_lfnEntry(file, file->lfnDeleteIndex - 1, &entry, true);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        if (entry[11] != FAT_FILE_ATTRIBUTE_LONGNAME ||
            (entry[0] & 0x1F) != file->lfnEntryCount - file->lfnDeleteIndex + 1) {
            afatfs_fileError(file, AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
            return AFATFS_OPERATION_FAILURE;
        }
        entry[0] = FAT_DELETED_FILE_MARKER;
        file->lfnDeleteIndex++;
    }
    file->lfnDeleteIndex = 0;
    file->lfnEntryCount = 0;
    file->lfnProgress = 0;
    file->lfnOutput = NULL;
    return AFATFS_OPERATION_SUCCESS;
}

// Caller-owned output survives cache misses; no 256-byte buffer is added to a handle.
static afatfsOperationStatus_e afatfs_lfnFileName(afatfsFilePtr_t file, afatfsFileInfo_t *info)
{
    if (file->lfnOutput != info) {
        file->lfnOutput = info;
        file->lfnProgress = 0;
    }
    while (file->lfnProgress < file->lfnEntryCount) {
        uint8_t *entry;
        afatfsOperationStatus_e status = afatfs_lfnEntry(file, file->lfnProgress, &entry, false);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        unsigned ordinal = file->lfnEntryCount - file->lfnProgress;
        if (entry[11] != FAT_FILE_ATTRIBUTE_LONGNAME || (entry[0] & 0x1F) != ordinal) {
            file->lfnOutput = NULL;
            file->lfnProgress = 0;
            return AFATFS_OPERATION_FAILURE;
        }
        for (unsigned index = 0; index < 13; index++) {
            unsigned position = (ordinal - 1) * 13 + index;
            uint16_t character = afatfs_lfnUnit(entry, index);
            if (position >= 256 || character == 0xFFFF) {
                continue;
            }
            if (character >= 128) {
                // This chain was ASCII when opened; metadata changed or a read was corrupted.
                file->lfnOutput = NULL;
                file->lfnProgress = 0;
                return AFATFS_OPERATION_FAILURE;
            }
            info->name[position] = (char)character;
        }
        if (!file->lfnProgress) {
            unsigned end = ordinal * 13;
            info->name[end < 256 ? end : 255] = 0;
        }
        file->lfnProgress++;
    }
    file->lfnOutput = NULL;
    file->lfnProgress = 0;
    return AFATFS_OPERATION_SUCCESS;
}

static afatfsOperationStatus_e afatfs_createLfnContinue(afatfsFilePtr_t file)
{
    afatfsCreateFile_t *state = &file->operation.state.createFile;
    const char *name = (const char *)state->fileName;
    fatDirectoryEntry_t *entry;
    afatfsOperationStatus_e status;
    for (unsigned budget = 0; budget < 32; budget++) {
        switch (state->phase) {
            case AFATFS_CREATEFILE_PHASE_INITIAL:
                if (afatfs_findFirstCWD(&afatfs.currentDirectory, &file->directoryEntryPos, file) !=
                    AFATFS_OPERATION_SUCCESS) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                memset(&state->lfn, 0, sizeof(state->lfn));
                state->phase = AFATFS_CREATEFILE_PHASE_FIND_FILE;
                break;
            case AFATFS_CREATEFILE_PHASE_FIND_FILE:
            case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE: {
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                bool finding = state->phase == AFATFS_CREATEFILE_PHASE_FIND_FILE;
                if (!entry || fat_isDirectoryEntryTerminator(entry)) {
                    if (finding && !(file->mode & AFATFS_FILE_MODE_CREATE)) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
                    memset(&state->lfn, 0, sizeof(state->lfn));
                    if (finding) {
                        state->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
                        break;
                    }
                    file->lfnEntryCount = state->location.aliasNumber ? (uint8_t)((strlen(name) + 12) / 13) : 0;
                    file->lfnAscii = true;
                    for (unsigned index = 0; name[index]; index++) {
                        if (name[index] >= 'a' && name[index] <= 'z') {
                            file->lfnEntryCount = (uint8_t)((strlen(name) + 12) / 13);
                        }
                    }
                    state->location.orphanSector = 0;
                    state->phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB;
                    break;
                }
                uint8_t *bytes = (uint8_t *)entry;
                if (fat_isDirectoryEntryEmpty(entry)) {
                    memset(&state->lfn, 0, sizeof(state->lfn));
                    break;
                }
                if (fat_isLfnEntry(entry)) {
                    if (bytes[0] & FAT_LONG_FILENAME_ENTRY) {
                        afatfs_lfnRememberStart(file, &file->directoryEntryPos);
                    }
                    char candidate[13];
                    unsigned written = 0;
                    for (unsigned index = 0; index < 11; index++) {
                        if (index == 8 && state->shortName[index] != ' ') {
                            candidate[written++] = '.';
                        }
                        if (state->shortName[index] != ' ') {
                            candidate[written++] = (char)state->shortName[index];
                        }
                    }
                    candidate[written] = 0;
                    afatfs_lfnConsume(&state->lfn, bytes, finding ? name : candidate, NULL);
                    break;
                }
                bool attached = afatfs_lfnAttached(&state->lfn, bytes);
                bool matched = (attached && state->lfn.matched) ||
                    ((!finding || !state->location.aliasNumber) && !memcmp(bytes, state->shortName, 11));
                if ((!finding || !fat_isVolumeLabel(entry)) && matched) {
                    // A rename may resolve the new spelling to its own source (case/alias change).
                    // Keep the source occupied during alias selection, then use the normal publish/delete flow.
                    if (finding && (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) && afatfs_request &&
                        afatfs_request->operation == AFATFS_API_RENAME && afatfs_request->file &&
                        afatfs_request->file->directoryEntryPos.sectorNumberPhysical ==
                            file->directoryEntryPos.sectorNumberPhysical &&
                        afatfs_request->file->directoryEntryPos.entryIndex == file->directoryEntryPos.entryIndex) {
                        memset(&state->lfn, 0, sizeof(state->lfn));
                        break;
                    }
                    if (!finding) {
                        if (++state->location.aliasNumber > 999999) {
                            file->error = AFATFS_ERROR_NO_SPACE;
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs_lfnAlias(state);
                        afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
                    } else {
                        uint32_t cluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
                        if (entry->fileSize > UINT32_MAX - afatfs_clusterSize() + 1 ||
                            ((entry->fileSize || (entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) &&
                                (cluster < 2 || cluster - 2 >= afatfs.numClusters))) {
                            file->error = AFATFS_ERROR_UNSUPPORTED;
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs_fileLoadDirectoryEntry(file, entry);
                        file->lfnEntryCount = attached ? state->lfn.count : 0;
                        file->lfnAscii = attached && state->lfn.ascii;
                        if (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) {
                            file->error = AFATFS_ERROR_EXISTS;
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs_findLastCWD(&afatfs.currentDirectory);
                        state->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                        return AFATFS_OPERATION_SUCCESS;
                    }
                }
                memset(&state->lfn, 0, sizeof(state->lfn));
                break;
            }
            case AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB:
                if (afatfs_fileIsBusy(&afatfs.currentDirectory)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!entry) {
                    status = afatfs_extendSubdirectory(&afatfs.currentDirectory, NULL, NULL, NULL);
                    if (status == AFATFS_OPERATION_FAILURE) {
                        file->error = AFATFS_ERROR_NO_SPACE;
                        return status;
                    }
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                if (fat_isDirectoryEntryTerminator(entry)) {
                    state->directoryEnd = true;
                }
                if (!state->directoryEnd && !fat_isDirectoryEntryEmpty(entry)) {
                    state->freeEntries = 0;
                    state->location.orphanSector =
                        fat_isLfnEntry(entry) ? file->directoryEntryPos.sectorNumberPhysical : 0;
                    state->orphanIndex = (uint8_t)file->directoryEntryPos.entryIndex;
                    break;
                }
                if (!state->freeEntries) {
                    afatfs_lfnRememberStart(file, &file->directoryEntryPos);
                }
                if (++state->freeEntries <= file->lfnEntryCount) {
                    break;
                }
                state->phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY;
                break;
            case AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY:
                // Break an orphan chain that could otherwise acquire the new short entry.
                if (state->location.orphanSector) {
                    uint8_t *buffer;
                    status = afatfs_cacheSector(state->location.orphanSector, &buffer,
                        AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    buffer[state->orphanIndex * 32] = FAT_DELETED_FILE_MARKER;
                    state->location.orphanSector = 0;
                }
                // If an end marker was consumed, preserve a following marker before publishing.
                if (state->directoryEnd) {
                    uint8_t *bytes;
                    afatfs_findLast(&afatfs.currentDirectory);
                    status = afatfs_lfnEntry(file, file->lfnEntryCount + 1, &bytes, true);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    if (bytes) {
                        memset(bytes, 0, 32);
                    }
                    state->directoryEnd = false;
                }
                afatfs_findLast(&afatfs.currentDirectory);
                while (state->writeIndex < file->lfnEntryCount) {
                    uint8_t *bytes;
                    status = afatfs_lfnEntry(file, state->writeIndex, &bytes, true);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    afatfs_lfnWrite(bytes, name, file->lfnEntryCount - state->writeIndex, file->lfnEntryCount,
                        fat_createCheckSum((const char *)state->shortName));
                    state->writeIndex++;
                }
                state->phase = AFATFS_CREATEFILE_PHASE_LFN_FLUSH;
                break;
            case AFATFS_CREATEFILE_PHASE_LFN_FLUSH:
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                state->phase = AFATFS_CREATEFILE_PHASE_LFN_PUBLISH;
                break;
            case AFATFS_CREATEFILE_PHASE_LFN_PUBLISH: {
                uint8_t *buffer;
                status = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &buffer,
                    AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                entry = (fatDirectoryEntry_t *)buffer + file->directoryEntryPos.entryIndex;
                memset(entry, 0, sizeof(*entry));
                memcpy(entry->fileName, state->shortName, 11);
                entry->attrib = file->attrib;
                uint32_t timestamp = afatfs_fatTime();
                entry->creationDate = (uint16_t)(timestamp >> 16);
                entry->creationTime = (uint16_t)timestamp;
                entry->lastWriteDate = (uint16_t)(timestamp >> 16);
                entry->lastWriteTime = (uint16_t)timestamp;
                afatfs_findLastCWD(&afatfs.currentDirectory);
                state->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                return AFATFS_OPERATION_SUCCESS;
            }
            default:
                return AFATFS_OPERATION_FAILURE;
        }
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
