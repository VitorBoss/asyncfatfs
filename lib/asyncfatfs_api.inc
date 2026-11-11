/* Extended asynchronous API. One caller-owned request is driven by the original poll loop.
 * No heap, second cache, or synchronous storage path is used. */
#include <stdarg.h>
#include <stdio.h>

#ifndef AFATFS_USE_FREEFILE
uint32_t afatfs_getContiguousFreeSpace(void)
{
    return 0;
}
#endif

static afatfsOperationStatus_e afatfs_apiError(afatfsError_e error)
{
    if (afatfs_mediaSessionActive()) {
        afatfs.lastError = error;
    }
    return AFATFS_OPERATION_FAILURE;
}

static unsigned afatfs_apiUpper(unsigned character)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && character < 128) {
        return afatfs.upcaseAscii[character];
    }
#endif
    return character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character;
}

static void afatfs_apiShortName(char *out, const uint8_t *name, uint8_t caseFlags)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && name[i] != ' '; i++) {
        unsigned character = name[i];
        out[n++] = (char)((caseFlags & 0x08) && character >= 'A' && character <= 'Z' ? character + 32 : character);
    }
    if (name[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && name[i] != ' '; i++) {
            unsigned character = name[i];
            out[n++] = (char)((caseFlags & 0x10) && character >= 'A' && character <= 'Z' ? character + 32 : character);
        }
    }
    out[n] = 0;
}

static void afatfs_apiFatInfo(afatfsFileInfo_t *info, const fatDirectoryEntry_t *entry, bool longName)
{
    if (!longName) {
        memset(info, 0, sizeof(*info));
        afatfs_apiShortName(info->name, entry->fileName, entry->ntReserved);
    }
    info->contiguous = false;
    info->attributes = entry->attrib;
    info->firstCluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
    info->size = entry->fileSize;
    info->validSize = info->size;
    info->allocatedSize =
        info->size <= UINT32_MAX - afatfs_clusterSize() + 1 ? roundUpTo(info->size, afatfs_clusterSize()) : UINT32_MAX;
    info->created = ((uint32_t)entry->creationDate << 16) | entry->creationTime;
    info->modified = ((uint32_t)entry->lastWriteDate << 16) | entry->lastWriteTime;
    info->accessed = (uint32_t)entry->lastAccessDate << 16;
    if (info->attributes & FAT_FILE_ATTRIBUTE_DIRECTORY) {
        info->size = info->validSize = 0;
    }
}

afatfsOperationStatus_e afatfs_fstat(afatfsFilePtr_t file, afatfsFileInfo_t *info)
{
    if (!afatfs_mediaSessionActive() || !file || file->type == AFATFS_FILE_TYPE_NONE || !info) {
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    }
    if (afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (!file->directoryEntryPos.sectorNumberPhysical) {
        memset(info, 0, sizeof(*info));
        strcpy(info->name, "/");
        info->attributes = FAT_FILE_ATTRIBUTE_DIRECTORY;
        info->firstCluster = file->firstCluster;
        return AFATFS_OPERATION_SUCCESS;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatEntryCount) {
        const exFatFileEntry_t *primary = (const exFatFileEntry_t *)file->exfatEntries;
        const exFatStreamEntry_t *stream = (const exFatStreamEntry_t *)(file->exfatEntries + 32);
        memset(info, 0, sizeof(*info));
        info->attributes = (uint8_t)primary->fileAttirb;
        info->created = ((uint32_t)primary->creationDate << 16) | primary->creationTime;
        info->modified = ((uint32_t)primary->lastWriteDate << 16) | primary->lastWriteTime;
        info->accessed = ((uint32_t)primary->lastAccessDate << 16) | primary->lastAccessTime;
        for (unsigned n = 0; n < stream->nameLength; n++) {
            const uint8_t *buffer = file->exfatEntries + (2 + n / 15) * 32 + 2 + n % 15 * 2;
            if (buffer[1] || buffer[0] >= 128) {
                return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
            }
            info->name[n] = (char)buffer[0];
        }
    } else
#endif
    {
        fatDirectoryEntry_t *entry;
        if (!afatfs_fGetFileInfo(file, &entry)) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (entry) {
#ifdef ASYNCFS_ENABLE_LFN
            if (file->lfnOutput != info) {
                afatfs_apiFatInfo(info, entry, false);
            }
            if (file->lfnEntryCount && file->lfnAscii) {
                afatfsOperationStatus_e status = afatfs_lfnFileName(file, info);
                if (status == AFATFS_OPERATION_FAILURE) {
                    file->lfnOutput = NULL;
                    file->lfnProgress = 0;
                    return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                }
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
#else
            afatfs_apiFatInfo(info, entry, false);
#endif
        } else {
            memset(info, 0, sizeof(*info));
            strcpy(info->name, "/");
            info->attributes = FAT_FILE_ATTRIBUTE_DIRECTORY;
        }
    }
    afatfs_fileUpdateFilesize(file);
    info->firstCluster = file->firstCluster;
    info->allocatedSize = file->physicalSize;
    info->size = file->type == AFATFS_FILE_TYPE_NORMAL ? file->logicalSize : 0;
    info->validSize = info->size;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        info->validSize = MIN(info->size, file->validDataLength);
        info->contiguous = file->exfatNoFatChain;
    }
#endif
    return AFATFS_OPERATION_SUCCESS;
}

static void afatfs_apiDirectory(afatfsDirectory_t *dir, afatfsFilePtr_t file)
{
    memset(dir, 0, sizeof(*dir));
    dir->generation = afatfs_mediaGeneration;
    dir->cursor.firstCluster = dir->cursor.cluster = file ? file->firstCluster : afatfs.rootDirectoryCluster;
    dir->cursor.root16 =
        file ? file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY : afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (file && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        dir->cursor.contiguous = file->exfatNoFatChain;
        dir->cursor.clusters = file->physicalSize / afatfs_clusterSize();
    }
#endif
}

static afatfsOperationStatus_e afatfs_apiDirectoryEntry(
    afatfsDirectory_t *dir, uint8_t **entry, afatfsDirEntryPointer_t *position)
{
    afatfsDirectoryCursor_t *cursor = &dir->cursor;
    if (cursor->ended) {
        *entry = NULL;
        return AFATFS_OPERATION_SUCCESS;
    }
    uint32_t sectorOffset = cursor->offset / 16;
    uint32_t sector;
    if (cursor->root16) {
        if (sectorOffset >= afatfs.rootDirectorySectors) {
            cursor->ended = true;
            *entry = NULL;
            return AFATFS_OPERATION_SUCCESS;
        }
        sector = afatfs.clusterStartSector - afatfs.rootDirectorySectors + sectorOffset;
    } else {
        uint32_t wanted = sectorOffset / afatfs.sectorsPerCluster;
        if (wanted >= afatfs.numClusters) {
            return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
        }
        while (cursor->clusterIndex < wanted) {
            uint32_t next;
            if (cursor->contiguous) {
                next = cursor->clusterIndex + 1 >= cursor->clusters ? 0xFFFFFFFF : cursor->cluster + 1;
            } else {
                afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, cursor->cluster, &next);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
            if (afatfs_FATIsEndOfChainMarker(next)) {
                cursor->ended = true;
                *entry = NULL;
                return AFATFS_OPERATION_SUCCESS;
            }
            cursor->cluster = next;
            cursor->clusterIndex++;
        }
        if (cursor->cluster < 2 || cursor->cluster - 2 >= afatfs.numClusters) {
            return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
        }
        sector = afatfs_fileClusterToPhysical(cursor->cluster, sectorOffset % afatfs.sectorsPerCluster);
    }
    uint8_t *buffer;
    afatfsOperationStatus_e status = afatfs_cacheSector(sector, &buffer, AFATFS_CACHE_READ, 0);
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    position->sectorNumberPhysical = sector;
    position->entryIndex = (int16_t)(cursor->offset % 16);
    *entry = buffer + position->entryIndex * 32;
    cursor->offset++;
    return AFATFS_OPERATION_SUCCESS;
}

static bool afatfs_apiMatch(const char *pattern, const char *name)
{
    const char *star = NULL;
    const char *retry = NULL;
    while (*name) {
        if (*pattern == '?' || afatfs_apiUpper((uint8_t)*pattern) == afatfs_apiUpper((uint8_t)*name)) {
            pattern++;
            name++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = name;
        } else if (star) {
            pattern = star + 1;
            name = ++retry;
        } else {
            return false;
        }
    }
    while (*pattern == '*') {
        pattern++;
    }
    return !*pattern;
}

static afatfsOperationStatus_e afatfs_apiReadDirectory(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    if (!afatfs_mediaSessionActive() || !dir || !info || dir->generation != afatfs_mediaGeneration) {
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    }
    if (dir->reading && dir->output != info) {
        return afatfs_apiError(AFATFS_ERROR_INVALID_PARAMETER);
    }
    dir->output = info;
    for (unsigned budget = 0; budget < 16; budget++) {
        uint8_t *bytes;
        afatfsDirEntryPointer_t pos;
        afatfsOperationStatus_e status = afatfs_apiDirectoryEntry(dir, &bytes, &pos);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        if (!bytes || !bytes[0]) {
            dir->cursor.ended = true;
            memset(&dir->nameState, 0, sizeof(dir->nameState));
            memset(info, 0, sizeof(*info));
            return AFATFS_OPERATION_SUCCESS;
        }
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (!dir->nameState.exfat.remaining) {
                if (bytes[0] != EXFAT_DIR_FILEDIR) {
                    continue;
                }
                if (bytes[1] < 2 || bytes[1] >= AFATFS_EXFAT_MAX_ENTRIES) {
                    return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                }
                memset(info, 0, sizeof(*info));
                dir->output = info;
                const exFatFileEntry_t *primary = (const exFatFileEntry_t *)bytes;
                info->attributes = (uint8_t)primary->fileAttirb;
                info->created = ((uint32_t)primary->creationDate << 16) | primary->creationTime;
                info->modified = ((uint32_t)primary->lastWriteDate << 16) | primary->lastWriteTime;
                info->accessed = ((uint32_t)primary->lastAccessDate << 16) | primary->lastAccessTime;
                dir->nameState.exfat.expectedChecksum = primary->checksum;
                dir->nameState.exfat.checksum = 0;
                dir->nameState.exfat.remaining = bytes[1] + 1;
                dir->nameState.exfat.entryIndex = 0;
                dir->nameState.exfat.nameWritten = 0;
            }
            for (unsigned n = 0; n < 32; n++) {
                if (dir->nameState.exfat.entryIndex == 0 && (n == 2 || n == 3)) {
                    continue;
                }
                dir->nameState.exfat.checksum =
                    (uint16_t)((dir->nameState.exfat.checksum >> 1 | (uint32_t)dir->nameState.exfat.checksum << 15) +
                        bytes[n]);
            }
            if (dir->nameState.exfat.entryIndex == 1) {
                const exFatStreamEntry_t *stream = (const exFatStreamEntry_t *)bytes;
                if (bytes[0] != EXFAT_DIR_STREAM || !stream->nameLength || stream->dataSize > UINT32_MAX ||
                    stream->fileSize > stream->dataSize || stream->dataSize > UINT32_MAX - afatfs_clusterSize() + 1) {
                    return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                }
                dir->nameState.exfat.nameLength = stream->nameLength;
                dir->nameState.exfat.nameHash = stream->nameHash;
                info->firstCluster = stream->firstCluster;
                info->size = (uint32_t)stream->dataSize;
                info->validSize = (uint32_t)stream->fileSize;
                info->allocatedSize = roundUpTo(info->size, afatfs_clusterSize());
                info->contiguous = (stream->flags & 2) != 0;
            } else if (dir->nameState.exfat.entryIndex >= 2 &&
                dir->nameState.exfat.nameWritten < dir->nameState.exfat.nameLength) {
                if (bytes[0] != EXFAT_DIR_FILENAME) {
                    return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                }
                for (unsigned n = 0; n < 15 && dir->nameState.exfat.nameWritten < dir->nameState.exfat.nameLength;
                    n++) {
                    if (!bytes[2 + n * 2] || bytes[2 + n * 2] >= 128 || bytes[3 + n * 2]) {
                        return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                    }
                    info->name[dir->nameState.exfat.nameWritten++] = (char)bytes[2 + n * 2];
                }
            } else if (dir->nameState.exfat.entryIndex >= 2 && (bytes[0] & 0xE0) != 0xE0) {
                return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
            }
            dir->nameState.exfat.entryIndex++;
            if (--dir->nameState.exfat.remaining) {
                continue;
            }
            uint16_t hash = 0;
            for (unsigned n = 0; n < dir->nameState.exfat.nameWritten; n++) {
                uint16_t ch = afatfs.upcaseAscii[(uint8_t)info->name[n]];
                hash = (uint16_t)((hash >> 1 | (uint32_t)hash << 15) + (ch & 255));
                hash = (uint16_t)((hash >> 1 | (uint32_t)hash << 15) + (ch >> 8));
            }
            if (dir->nameState.exfat.checksum != dir->nameState.exfat.expectedChecksum ||
                dir->nameState.exfat.nameWritten != dir->nameState.exfat.nameLength ||
                hash != dir->nameState.exfat.nameHash) {
                return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
            }
            if (info->attributes & FAT_FILE_ATTRIBUTE_DIRECTORY) {
                info->size = info->validSize = 0;
            }
        } else
#endif
        {
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)bytes;
#ifdef ASYNCFS_ENABLE_LFN
            if (!fat_isDirectoryEntryEmpty(entry) && fat_isLfnEntry(entry)) {
                afatfs_lfnConsume(&dir->nameState.lfn, bytes, NULL, info->name);
                continue;
            }
            bool longName = afatfs_lfnAttached(&dir->nameState.lfn, bytes) && dir->nameState.lfn.ascii;
            memset(&dir->nameState, 0, sizeof(dir->nameState));
#else
            bool longName = false;
#endif
            if (fat_isDirectoryEntryEmpty(entry) || fat_isVolumeLabel(entry) || fat_isLfnEntry(entry) ||
                !memcmp(entry->fileName, ".          ", 11) || !memcmp(entry->fileName, "..         ", 11)) {
                continue;
            }
            afatfs_apiFatInfo(info, entry, longName);
        }
        if (!dir->pattern || afatfs_apiMatch(dir->pattern, info->name)) {
            return AFATFS_OPERATION_SUCCESS;
        }
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

afatfsOperationStatus_e afatfs_readdir(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    afatfsOperationStatus_e status = afatfs_apiReadDirectory(dir, info);
    if (dir && dir->generation == afatfs_mediaGeneration) {
        dir->reading = status == AFATFS_OPERATION_IN_PROGRESS;
    }
    return status;
}
afatfsOperationStatus_e afatfs_findnext(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    return afatfs_readdir(dir, info);
}
afatfsOperationStatus_e afatfs_rewinddir(afatfsDirectory_t *dir)
{
    if (!dir || dir->generation != afatfs_mediaGeneration) {
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    }
    dir->cursor.cluster = dir->cursor.firstCluster;
    dir->cursor.clusterIndex = dir->cursor.offset = 0;
    dir->cursor.ended = false;
    memset(&dir->nameState, 0, sizeof(dir->nameState));
    dir->output = NULL;
    dir->reading = false;
    return AFATFS_OPERATION_SUCCESS;
}
afatfsOperationStatus_e afatfs_closedir(afatfsDirectory_t *dir)
{
    if (!dir || dir->generation != afatfs_mediaGeneration) {
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    }
    afatfsDirectory_t **link = &afatfs_directories;
    while (*link && *link != dir) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = dir->next;
    }
    dir->registered = false;
    dir->reading = false;
    dir->generation = 0;
    return AFATFS_OPERATION_SUCCESS;
}

static bool afatfs_apiBegin(afatfsRequest_t *request, unsigned operation, afatfsResultCallback_t callback, void *user)
{
    if (!request || afatfs_request || afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        if (afatfs_mediaSessionActive()) {
            afatfs.lastError = AFATFS_ERROR_BUSY;
        }
        return false;
    }
    memset(request, 0, sizeof(*request));
    request->operation = (uint8_t)operation;
    request->callback = callback;
    request->user = user;
    request->checkpoint = true;
    afatfs_request = request;
    return true;
}

static void afatfs_apiFinish(afatfsError_e result)
{
    afatfsRequest_t *request = afatfs_request;
    afatfsResultCallback_t callback = request->callback;
    void *user = request->user;
    request->result = result;
    request->complete = true;
    request->driving = false;
    if (request->file && request->file->type != AFATFS_FILE_TYPE_NONE) {
        request->file->error = (uint8_t)result;
    }
    if (result) {
        afatfs.lastError = result;
    }
    afatfs_request = NULL;
    if (callback) {
        callback(result, user);
    }
}

static void afatfs_apiOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->file = file;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_NOT_FOUND;
    }
}
static void afatfs_apiClosed(void *user)
{
    ((afatfsRequest_t *)user)->waiting = false;
}
static void afatfs_apiSynced(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError;
    }
}

static bool afatfs_apiOpen(afatfsRequest_t *request, uint8_t mode)
{
    afatfsFilePtr_t file = afatfs_allocateFileHandle();
    if (!file) {
        return false;
    }
    request->waiting = true;
    afatfs_createFile(file, request->name, FAT_FILE_ATTRIBUTE_ARCHIVE, mode, afatfs_apiOpened, request);
    return true;
}

static void afatfs_apiClose(afatfsRequest_t *request)
{
    request->checkpoint = true;
    if (!request->file) {
        afatfs_apiFinish(request->result);
        return;
    }
    if (request->phase != 250) {
        request->waiting = true;
        if (!afatfs_fclose(request->file, afatfs_apiClosed, request)) {
            request->waiting = false;
            return;
        }
        request->phase = 250;
    }
    if (!request->waiting) {
        request->file = request->other;
        request->other = NULL;
        if (request->file) {
            request->phase = 249;
        } else {
            afatfs_apiFinish(request->result);
        }
    }
}

bool afatfs_stat(
    afatfsRequest_t *request, const char *name, afatfsFileInfo_t *info, afatfsResultCallback_t callback, void *user)
{
    if (!name || !info || !afatfs_apiBegin(request, AFATFS_API_STAT, callback, user)) {
        return false;
    }
    request->name = name;
    request->output = info;
    return true;
}
bool afatfs_opendir(
    afatfsRequest_t *request, const char *name, afatfsDirectory_t *dir, afatfsResultCallback_t callback, void *user)
{
    for (afatfsDirectory_t *open = afatfs_directories; open; open = open->next) {
        if (open == dir) {
            return false;
        }
    }
    if (!name || !dir || !afatfs_apiBegin(request, AFATFS_API_OPENDIR, callback, user)) {
        return false;
    }
    request->name = name;
    request->output = dir;
    return true;
}
bool afatfs_findfirst(afatfsRequest_t *request, afatfsDirectory_t *dir, afatfsFileInfo_t *info, const char *name,
    const char *pattern, afatfsResultCallback_t callback, void *user)
{
    for (afatfsDirectory_t *open = afatfs_directories; open; open = open->next) {
        if (open == dir) {
            return false;
        }
    }
    if (!name || !dir || !info || !pattern || !afatfs_apiBegin(request, AFATFS_API_FIND, callback, user)) {
        return false;
    }
    request->name = name;
    request->otherName = pattern;
    request->output = dir;
    request->consumerUser = info;
    return true;
}
afatfsOperationStatus_e afatfs_chdrive(unsigned drive)
{
    return drive ? afatfs_apiError(AFATFS_ERROR_UNSUPPORTED) : AFATFS_OPERATION_SUCCESS;
}
afatfsOperationStatus_e afatfs_setcp(unsigned codePage)
{
    return codePage != 20127 ? afatfs_apiError(AFATFS_ERROR_UNSUPPORTED) : AFATFS_OPERATION_SUCCESS;
}
afatfsOperationStatus_e afatfs_rewind(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)
{
    return afatfs_fseek(file, 0, AFATFS_SEEK_SET, callback, user);
}

static bool afatfs_apiNamed(
    afatfsRequest_t *request, unsigned operation, const char *name, afatfsResultCallback_t callback, void *user)
{
    if (!name || !afatfs_apiBegin(request, operation, callback, user)) {
        return false;
    }
    request->name = name;
    return true;
}
bool afatfs_unlink(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiNamed(request, AFATFS_API_UNLINK, name, callback, user);
}
bool afatfs_rmdir(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiNamed(request, AFATFS_API_RMDIR, name, callback, user);
}
bool afatfs_rename(
    afatfsRequest_t *request, const char *oldName, const char *newName, afatfsResultCallback_t callback, void *user)
{
    if (!newName || !afatfs_apiNamed(request, AFATFS_API_RENAME, oldName, callback, user)) {
        return false;
    }
    request->otherName = newName;
    return true;
}
bool afatfs_chmod(
    afatfsRequest_t *request, const char *name, uint8_t attr, uint8_t mask, afatfsResultCallback_t callback, void *user)
{
    if (!afatfs_apiNamed(request, AFATFS_API_CHMOD, name, callback, user)) {
        return false;
    }
    request->value[0] = attr;
    request->value[1] = mask &
        (FAT_FILE_ATTRIBUTE_READ_ONLY | FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM |
            FAT_FILE_ATTRIBUTE_ARCHIVE);
    return true;
}
bool afatfs_utime(
    afatfsRequest_t *request, const char *name, uint32_t modified, afatfsResultCallback_t callback, void *user)
{
    if (!afatfs_apiNamed(request, AFATFS_API_UTIME, name, callback, user)) {
        return false;
    }
    request->value[0] = modified;
    return true;
}
bool afatfs_getfree(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user)
{
    if (!info || !afatfs_apiBegin(request, AFATFS_API_FREE, callback, user)) {
        return false;
    }
    request->output = info;
    request->value[0] = 2;
    return true;
}
bool afatfs_getlabel(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user)
{
    if (!info || !afatfs_apiBegin(request, AFATFS_API_GETLABEL, callback, user)) {
        return false;
    }
    request->output = info;
    return true;
}
bool afatfs_setlabel(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user)
{
    if (!name || strlen(name) > 11) {
        return false;
    }
    for (unsigned n = 0; name[n]; n++) {
        if ((uint8_t)name[n] < 32 || (uint8_t)name[n] >= 127 || strchr("\"*+,./:;<=>?[\\]|", name[n])) {
            afatfs.lastError = AFATFS_ERROR_INVALID_NAME;
            return false;
        }
    }
    return afatfs_apiNamed(request, AFATFS_API_SETLABEL, name, callback, user);
}
bool afatfs_getcwd(
    afatfsRequest_t *request, char *buffer, uint32_t capacity, afatfsResultCallback_t callback, void *user)
{
    if (!buffer || capacity < 2 || !afatfs_apiBegin(request, AFATFS_API_CWD, callback, user)) {
        return false;
    }
    request->output = buffer;
    request->value[1] = capacity;
    return true;
}

static void afatfs_apiFree(afatfsRequest_t *request)
{
    if (!request->phase) {
        for (unsigned n = 0; n < AFATFS_MAX_OPEN_FILES; n++) {
            if (afatfs.openFiles[n].operation.active != AFATFS_FILE_OPERATION_NONE) {
                return;
            }
        }
        if (afatfs.cwdWalkMutex || afatfs.currentDirectory.operation.active != AFATFS_FILE_OPERATION_NONE) {
            return;
        }
        request->phase = 1;
    }
    for (unsigned budget = 0; budget < 128 && request->value[0] - 2 < afatfs.numClusters; budget++) {
        bool free;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            bool occupied;
            afatfsOperationStatus_e status = afatfs_exfatBitmap(request->value[0], false, false, &occupied);
            if (status == AFATFS_OPERATION_FAILURE) {
                afatfs_apiFinish(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                return;
            }
            if (status != AFATFS_OPERATION_SUCCESS) {
                return;
            }
            free = !occupied;
        } else
#endif
        {
            uint32_t next;
            afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, request->value[0], &next);
            if (status == AFATFS_OPERATION_FAILURE) {
                afatfs_apiFinish(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                return;
            }
            if (status != AFATFS_OPERATION_SUCCESS) {
                return;
            }
            free = next == 0;
        }
        if (free) {
            request->value[2]++;
        }
        request->value[0]++;
    }
    if (request->value[0] - 2 == afatfs.numClusters) {
        afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)request->output;
        info->clusterSize = afatfs_clusterSize();
        info->totalClusters = afatfs.numClusters;
        info->freeClusters = request->value[2];
        afatfs_apiFinish(AFATFS_ERROR_NONE);
    }
}

static void afatfs_apiLabel(afatfsRequest_t *request)
{
    if (!request->phase) {
        if (request->operation == AFATFS_API_SETLABEL && afatfs.readOnly) {
            afatfs_apiFinish(AFATFS_ERROR_DENIED);
            return;
        }
        if (afatfs.cwdWalkMutex) {
            return;
        }
        uint8_t *boot;
        if (afatfs_cacheSector(afatfs.partitionStartSector, &boot, AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        if (request->operation == AFATFS_API_GETLABEL) {
            afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)request->output;
            memset(info, 0, sizeof(*info));
            fatVolumeID_t *volume = (fatVolumeID_t *)boot;
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                info->serial = volume->fat.fatDescriptor.fat16.volumeID;
            } else if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
                info->serial = volume->fat.fatDescriptor.fat32.volumeID;
            }
#ifdef ASYNCFS_ENABLE_EXFAT
            else {
                info->serial = afatfs.journal.serial;
            }
#endif
            info->clusterSize = afatfs_clusterSize();
            info->totalClusters = afatfs.numClusters;
        }
        afatfs_apiDirectory(&request->directory, NULL);
        request->phase = 1;
    }
    if (request->phase == 1) {
        uint8_t *entry;
        afatfsDirEntryPointer_t pos;
        afatfsOperationStatus_e status = afatfs_apiDirectoryEntry(&request->directory, &entry, &pos);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        bool label = entry &&
            (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                    ? entry[0] == EXFAT_DIR_VLABEL
                    : !fat_isDirectoryEntryEmpty((fatDirectoryEntry_t *)entry) &&
                        ((fatDirectoryEntry_t *)entry)->attrib == FAT_FILE_ATTRIBUTE_VOLUME_ID);
        bool end = !entry || !entry[0];
        if (request->operation == AFATFS_API_GETLABEL && (label || end)) {
            afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)request->output;
            if (label) {
                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                    if (entry[1] > 11) {
                        afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED);
                        return;
                    }
                    for (unsigned n = 0; n < entry[1]; n++) {
                        if (entry[3 + 2 * n] || entry[2 + 2 * n] >= 128) {
                            afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED);
                            return;
                        }
                        info->label[n] = (char)entry[2 + 2 * n];
                    }
                } else {
                    memcpy(info->label, entry, 11);
                    for (int n = 10; n >= 0 && info->label[n] == ' '; n--) {
                        info->label[n] = 0;
                    }
                }
            }
            afatfs_apiFinish(AFATFS_ERROR_NONE);
            return;
        }
        if (request->operation == AFATFS_API_GETLABEL) {
            return;
        }
        if (entry && !request->value[4] &&
            (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT ? !(entry[0] & 0x80) : entry[0] == 0xE5)) {
            request->value[4] = pos.sectorNumberPhysical;
            request->value[5] = (uint32_t)pos.entryIndex;
        }
        if (label) {
            request->value[4] = pos.sectorNumberPhysical;
            request->value[5] = (uint32_t)pos.entryIndex;
            request->value[6] = 1;
        }
        if (end && entry && !request->value[4]) {
            request->value[4] = pos.sectorNumberPhysical;
            request->value[5] = (uint32_t)pos.entryIndex;
            request->value[7] = 1;
        }
        if (!label && !end) {
            return;
        }
        if (!request->name[0] && !label) {
            afatfs_apiFinish(AFATFS_ERROR_NONE);
            return;
        }
        if (!request->value[4]) {
            if (request->directory.cursor.root16) {
                afatfs_apiFinish(AFATFS_ERROR_NO_SPACE);
                return;
            }
            request->phase = 6;
        } else {
            request->phase = 5;
        }
    }
    if (request->phase == 6) {
        if (!request->file) {
            request->file = afatfs_allocateFileHandle();
            if (!request->file) {
                return;
            }
            afatfs_initFileHandle(request->file);
            request->file->type = AFATFS_FILE_TYPE_DIRECTORY;
            request->file->mode = AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE;
            request->file->attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
            request->file->firstCluster = afatfs.rootDirectoryCluster;
            request->file->cursorPreviousCluster = request->directory.cursor.cluster;
            request->file->physicalSize = (request->directory.cursor.clusterIndex + 1) * afatfs_clusterSize();
            request->file->cursorOffset = request->file->physicalSize;
            request->value[8] = request->file->physicalSize;
        }
        afatfsOperationStatus_e status = afatfs_extendSubdirectory(request->file, NULL, NULL, NULL);
        if (status == AFATFS_OPERATION_FAILURE) {
            request->result = AFATFS_ERROR_NO_SPACE;
            afatfs_apiClose(request);
            return;
        }
        request->phase = 7;
    }
    if (request->phase == 7) {
        if (request->file->operation.active != AFATFS_FILE_OPERATION_NONE) {
            return;
        }
        if (request->file->physicalSize == request->value[8]) {
            request->result = AFATFS_ERROR_NO_SPACE;
            afatfs_apiClose(request);
            return;
        }
        request->directory.cursor.ended = false;
        request->phase = 1;
        return;
    }
    if (request->phase == 5) {
        if (!afatfs_flush()) {
            return;
        }
        request->checkpoint = false;
        request->phase = request->value[7] ? 2 : 3;
    }
    if (request->phase == 2) {
        uint8_t *entry;
        afatfsDirEntryPointer_t pos;
        afatfsDirectoryCursor_t before = request->directory.cursor;
        if (afatfs_apiDirectoryEntry(&request->directory, &entry, &pos) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        if (entry) {
            uint8_t *sector;
            if (afatfs_cacheSector(pos.sectorNumberPhysical, &sector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) {
                request->directory.cursor = before;
                return;
            }
            memset(sector + pos.entryIndex * 32, 0, 32);
        }
        request->phase = 3;
    }
    if (request->phase == 3) {
        uint8_t *sector;
        if (afatfs_cacheSector(request->value[4], &sector,
                AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        uint8_t *entry = sector + request->value[5] * 32;
        if (!request->name[0]) {
            entry[0] = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT ? 0x03 : 0xE5;
        } else {
            memset(entry, 0, 32);
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                entry[0] = EXFAT_DIR_VLABEL;
                entry[1] = (uint8_t)strlen(request->name);
                for (unsigned n = 0; request->name[n]; n++) {
                    entry[2 + n * 2] = (uint8_t)request->name[n];
                }
            } else {
                memset(entry, ' ', 11);
                entry[11] = FAT_FILE_ATTRIBUTE_VOLUME_ID;
                for (unsigned n = 0; request->name[n]; n++) {
                    entry[n] = (uint8_t)afatfs_apiUpper((uint8_t)request->name[n]);
                }
            }
        }
        request->phase = 4;
        request->checkpoint = true;
    }
    if (request->phase == 4 && afatfs_flush()) {
        afatfs_apiClose(request);
    }
}

typedef struct afatfsPathFrame_t {
    afatfsDirectoryCursor_t cursor;
    uint32_t length;
} afatfsPathFrame_t;
static void afatfs_apiCwd(afatfsRequest_t *request)
{
    char *path = (char *)request->output;
    if (!request->phase) {
        strcpy(path, "/");
        request->value[2] = 1;
        request->value[3] = afatfs.currentDirectory.firstCluster;
        if (afatfs.currentDirectory.type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY ||
            request->value[3] == afatfs.rootDirectoryCluster) {
            afatfs_apiFinish(AFATFS_ERROR_NONE);
            return;
        }
        afatfs_apiDirectory(&request->directory, NULL);
        request->phase = 1;
    }
    afatfsOperationStatus_e status = afatfs_readdir(&request->directory, &request->info);
    if (status == AFATFS_OPERATION_FAILURE) {
        afatfs_apiFinish(afatfs.lastError);
        return;
    }
    if (status != AFATFS_OPERATION_SUCCESS) {
        return;
    }
    if (!request->info.name[0]) {
        if (!request->value[0]) {
            afatfs_apiFinish(AFATFS_ERROR_NOT_FOUND);
            return;
        }
        afatfsPathFrame_t frame;
        memcpy(&frame, request->scratch + --request->value[0] * sizeof(frame), sizeof(frame));
        memset(&request->directory, 0, sizeof(request->directory));
        request->directory.generation = afatfs_mediaGeneration;
        request->directory.cursor = frame.cursor;
        request->value[2] = frame.length;
        path[frame.length] = 0;
        return;
    }
    if (!(request->info.attributes & FAT_FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }
    uint32_t oldLength = request->value[2];
    uint32_t length = (uint32_t)strlen(request->info.name);
    bool separator = oldLength > 1;
    if (oldLength + separator + length >= request->value[1]) {
        afatfs_apiFinish(AFATFS_ERROR_INVALID_PARAMETER);
        return;
    }
    if (separator) {
        path[oldLength] = '/';
    }
    memcpy(path + oldLength + separator, request->info.name, length + 1);
    if (request->info.firstCluster == request->value[3]) {
        afatfs_apiFinish(AFATFS_ERROR_NONE);
        return;
    }
    if ((request->value[0] + 1) * sizeof(afatfsPathFrame_t) > sizeof(request->scratch)) {
        afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED);
        return;
    }
    afatfsPathFrame_t frame;
    frame.cursor = request->directory.cursor;
    frame.length = oldLength;
    memcpy(request->scratch + request->value[0]++ * sizeof(frame), &frame, sizeof(frame));
    memset(&request->directory, 0, sizeof(request->directory));
    request->directory.generation = afatfs_mediaGeneration;
    request->directory.cursor.cluster = request->directory.cursor.firstCluster = request->info.firstCluster;
    request->directory.cursor.contiguous = request->info.contiguous;
    request->directory.cursor.clusters = request->info.allocatedSize / afatfs_clusterSize();
    request->value[2] = oldLength + separator + length;
}

static void afatfs_apiOtherOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->other = file;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_EXISTS;
    }
}

static void afatfs_apiMetadata(afatfsRequest_t *request)
{
    afatfsFilePtr_t file = request->file;
    if (afatfs.readOnly || !file->directoryEntryPos.sectorNumberPhysical) {
        request->result = AFATFS_ERROR_DENIED;
        afatfs_apiClose(request);
        return;
    }
    if (request->phase == 1) {
        if (!afatfs_flush()) {
            return;
        }
        request->checkpoint = false;
        if (request->operation == AFATFS_API_CHMOD) {
            file->attrib = (uint8_t)((file->attrib & ~request->value[1]) | (request->value[0] & request->value[1]));
        }
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
            primary->fileAttirb = file->attrib;
            if (request->operation == AFATFS_API_UTIME) {
                primary->lastWriteDate = (uint16_t)(request->value[0] >> 16);
                primary->lastWriteTime = (uint16_t)request->value[0];
                primary->lastWriteTimeTenths = 0;
                primary->lastWriteUtcOffset = 0;
            }
            primary->checksum = exFat_entrySetChecksum(file->exfatEntries, file->exfatEntryCount * 32);
        }
#endif
        request->phase = 2;
    }
    if (request->phase == 2) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (afatfs_exfatWriteEntrySet(file) != AFATFS_OPERATION_SUCCESS) {
                return;
            }
        } else
#endif
        {
            uint8_t *buffer;
            if (afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &buffer,
                    AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) {
                return;
            }
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)buffer + file->directoryEntryPos.entryIndex;
            entry->attrib = file->attrib;
            if (request->operation == AFATFS_API_UTIME) {
                entry->lastWriteDate = (uint16_t)(request->value[0] >> 16);
                entry->lastWriteTime = (uint16_t)request->value[0];
            }
        }
        request->phase = 3;
        request->checkpoint = true;
    }
    afatfs_apiClose(request);
}

static void afatfs_apiRename(afatfsRequest_t *request)
{
    afatfsFilePtr_t source = request->file;
    if (request->phase == 1) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            unsigned extras = source->exfatEntryCount - 2 -
                (((exFatStreamEntry_t *)(source->exfatEntries + 32))->nameLength + 14) / 15;
            if (2 + (strlen(request->otherName) + 14) / 15 + extras > AFATFS_EXFAT_MAX_ENTRIES) {
                request->result = AFATFS_ERROR_UNSUPPORTED;
                afatfs_apiClose(request);
                return;
            }
        }
#endif
        if (afatfs.readOnly || !source->directoryEntryPos.sectorNumberPhysical ||
            (source->type == AFATFS_FILE_TYPE_DIRECTORY &&
                source->firstCluster == afatfs.currentDirectory.firstCluster)) {
            request->result = AFATFS_ERROR_DENIED;
            afatfs_apiClose(request);
            return;
        }
        if (source->type == AFATFS_FILE_TYPE_DIRECTORY) {
            for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next) {
                if (dir->cursor.firstCluster == source->firstCluster) {
                    request->result = AFATFS_ERROR_DENIED;
                    afatfs_apiClose(request);
                    return;
                }
            }
        }
        if (!afatfs_flush()) {
            return;
        }
        afatfsFilePtr_t target = afatfs_allocateFileHandle();
        if (!target) {
            return;
        }
        request->phase = 2;
        request->waiting = true;
        request->checkpoint = false;
        afatfs_createFile(target, request->otherName, FAT_FILE_ATTRIBUTE_ARCHIVE,
            AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_EXCLUSIVE_CREATE, afatfs_apiOtherOpened,
            request);
        return;
    }
    afatfsFilePtr_t target = request->other;
    if (request->phase == 2) {
        if (!target) {
            afatfs_apiClose(request);
            return;
        }
        target->firstCluster = source->firstCluster;
        target->physicalSize = source->physicalSize;
        target->logicalSize = source->logicalSize;
        target->attrib = source->attrib;
        target->type = source->type;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            target->validDataLength = source->validDataLength;
            target->committedDataLength = source->committedDataLength;
            target->exfatNoFatChain = source->exfatNoFatChain;
            memcpy(target->exfatEntries + 4, source->exfatEntries + 4, 28);
            exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(target->exfatEntries + 32);
            stream->flags = 1 | (target->exfatNoFatChain ? 2 : 0);
            stream->firstCluster = target->firstCluster;
            stream->dataSize = target->type == AFATFS_FILE_TYPE_DIRECTORY ? target->physicalSize : target->logicalSize;
            stream->fileSize =
                target->type == AFATFS_FILE_TYPE_DIRECTORY ? target->physicalSize : target->validDataLength;
            ((exFatFileEntry_t *)target->exfatEntries)->checksum =
                exFat_entrySetChecksum(target->exfatEntries, target->exfatEntryCount * 32);
        } else
#endif
        {
            fatDirectoryEntry_t *entry;
            if (!afatfs_fGetFileInfo(source, &entry)) {
                return;
            }
            memcpy(request->scratch, entry, 32);
        }
        request->phase = 3;
    }
    if (request->phase == 3) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (afatfs_exfatWriteEntrySet(target) != AFATFS_OPERATION_SUCCESS) {
                return;
            }
        } else
#endif
        {
            uint8_t *buffer;
            if (afatfs_cacheSector(target->directoryEntryPos.sectorNumberPhysical, &buffer,
                    AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) {
                return;
            }
            uint8_t *entry = buffer + target->directoryEntryPos.entryIndex * 32;
            uint8_t name[11];
            uint8_t caseFlags = entry[12];
            memcpy(name, entry, 11);
            memcpy(entry, request->scratch, 32);
            memcpy(entry, name, 11);
            entry[12] = caseFlags;
            if (source->directoryEntryPos.sectorNumberPhysical == target->directoryEntryPos.sectorNumberPhysical) {
                // Publish and retire both short entries in the same atomic-sector write.
                buffer[source->directoryEntryPos.entryIndex * 32] = FAT_DELETED_FILE_MARKER;
            }
        }
        request->phase = 4;
    }
    if (request->phase == 4) {
        if (!afatfs_flushCache()) {
            return;
        }
        if (afatfs_saveDirectoryEntry(source, AFATFS_SAVE_DIRECTORY_DELETED) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        request->phase = 5;
        request->checkpoint = true;
    }
    afatfs_apiClose(request);
}

static bool afatfs_apiFileRequest(
    afatfsRequest_t *request, unsigned operation, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    bool directory =
        file && (file->type == AFATFS_FILE_TYPE_DIRECTORY || file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY);
    if (!file ||
        (file->type != AFATFS_FILE_TYPE_NORMAL &&
            !(directory && (operation == AFATFS_API_CLOSE || operation == AFATFS_API_SYNC))) ||
        afatfs_fileIsBusy(file)) {
        return false;
    }
    if (!afatfs_apiBegin(request, operation, callback, user)) {
        return false;
    }
    request->file = file;
    file->error = AFATFS_ERROR_NONE;
    return true;
}
bool afatfs_open(afatfsRequest_t *request, const char *name, const char *mode, afatfsFilePtr_t *file,
    afatfsResultCallback_t callback, void *user)
{
    if (!name || !mode || !file || !afatfs_apiBegin(request, AFATFS_API_OPEN, callback, user)) {
        return false;
    }
    request->name = name;
    request->otherName = mode;
    request->output = file;
    *file = NULL;
    return true;
}
bool afatfs_close(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_CLOSE, file, callback, user);
}
bool afatfs_sync(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_SYNC, file, callback, user);
}
bool afatfs_truncate(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_TRUNCATE, file, callback, user);
}
bool afatfs_read(afatfsRequest_t *request, afatfsFilePtr_t file, uint8_t *buffer, uint32_t count,
    afatfsResultCallback_t callback, void *user)
{
    if ((!buffer && count) || !afatfs_apiFileRequest(request, AFATFS_API_READ, file, callback, user)) {
        return false;
    }
    request->output = buffer;
    request->value[0] = count;
    return true;
}
bool afatfs_write(afatfsRequest_t *request, afatfsFilePtr_t file, const uint8_t *buffer, uint32_t count,
    afatfsResultCallback_t callback, void *user)
{
    if ((!buffer && count) || !afatfs_apiFileRequest(request, AFATFS_API_WRITE, file, callback, user)) {
        return false;
    }
    request->name = (const char *)buffer;
    request->value[0] = count;
    return true;
}
bool afatfs_puts(
    afatfsRequest_t *request, afatfsFilePtr_t file, const char *text, afatfsResultCallback_t callback, void *user)
{
    if (!text || strlen(text) > UINT32_MAX || !afatfs_apiFileRequest(request, AFATFS_API_PUTS, file, callback, user)) {
        return false;
    }
    request->name = text;
    request->value[0] = (uint32_t)strlen(text);
    return true;
}
bool afatfs_gets(afatfsRequest_t *request, afatfsFilePtr_t file, char *buffer, uint32_t capacity,
    afatfsResultCallback_t callback, void *user)
{
    if (!buffer || !capacity || !afatfs_apiFileRequest(request, AFATFS_API_GETS, file, callback, user)) {
        return false;
    }
    request->output = buffer;
    request->value[0] = capacity;
    buffer[0] = 0;
    return true;
}
bool afatfs_printf(afatfsRequest_t *request, afatfsFilePtr_t file, char *workspace, uint32_t capacity,
    afatfsResultCallback_t callback, void *user, const char *format, ...)
{
    if (!workspace || !capacity || !format || afatfs_request) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(workspace, capacity, format, args);
    va_end(args);
    if (length < 0 || (uint32_t)length >= capacity) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        return false;
    }
    return afatfs_puts(request, file, workspace, callback, user);
}
bool afatfs_forward(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsForwardCallback_t consumer,
    void *consumerUser, uint32_t count, afatfsResultCallback_t callback, void *user)
{
    if (!consumer || !afatfs_apiFileRequest(request, AFATFS_API_FORWARD, file, callback, user)) {
        return false;
    }
    request->consumer = consumer;
    request->consumerUser = consumerUser;
    request->value[0] = count;
    return true;
}
static void afatfs_apiText(afatfsRequest_t *request)
{
    afatfsFilePtr_t file = request->file;
    if (file->operation.active != AFATFS_FILE_OPERATION_NONE) {
        return;
    }
    if (request->operation == AFATFS_API_PUTS || request->operation == AFATFS_API_WRITE) {
        if (afatfs.readOnly || !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) ||
            (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
            afatfs_apiFinish(AFATFS_ERROR_DENIED);
            return;
        }
        if (request->transferred < request->value[0]) {
            request->transferred += afatfs_fwrite(
                file, (const uint8_t *)request->name + request->transferred, request->value[0] - request->transferred);
        }
        if (request->transferred == request->value[0] && file->operation.active == AFATFS_FILE_OPERATION_NONE) {
            afatfs_apiFinish(AFATFS_ERROR_NONE);
        } else if (afatfs_isFull()) {
            afatfs_apiFinish(AFATFS_ERROR_NO_SPACE);
        }
    } else if (!(file->mode & AFATFS_FILE_MODE_READ)) {
        afatfs_apiFinish(AFATFS_ERROR_DENIED);
    } else if (request->operation == AFATFS_API_READ) {
        uint8_t *buffer = (uint8_t *)request->output;
        if (request->value[0]) {
            request->transferred +=
                afatfs_fread(file, buffer + request->transferred, request->value[0] - request->transferred);
        }
        if ((request->transferred == request->value[0] || afatfs_feof(file)) &&
            file->operation.active == AFATFS_FILE_OPERATION_NONE) {
            afatfs_apiFinish(AFATFS_ERROR_NONE);
        }
    } else if (request->operation == AFATFS_API_GETS) {
        char *buffer = (char *)request->output;
        for (unsigned budget = 0; budget < 64; budget++) {
            if (request->transferred + 1 >= request->value[0] || afatfs_feof(file)) {
                afatfs_apiFinish(AFATFS_ERROR_NONE);
                return;
            }
            uint8_t c;
            if (!afatfs_fread(file, &c, 1)) {
                return;
            }
            buffer[request->transferred++] = (char)c;
            buffer[request->transferred] = 0;
            if (c == '\n') {
                afatfs_apiFinish(AFATFS_ERROR_NONE);
                return;
            }
        }
    } else {
        if (!request->value[1]) {
            if (request->transferred == request->value[0] || afatfs_feof(file)) {
                afatfs_apiFinish(AFATFS_ERROR_NONE);
                return;
            }
            request->value[1] = afatfs_fread(file, request->scratch,
                MIN((uint32_t)sizeof(request->scratch), request->value[0] - request->transferred));
            request->value[2] = 0;
            if (!request->value[1]) {
                return;
            }
        }
        uint32_t accepted =
            request->consumer(request->scratch + request->value[2], request->value[1], request->consumerUser);
        if (accepted > request->value[1]) {
            afatfs_apiFinish(AFATFS_ERROR_INVALID_PARAMETER);
            return;
        }
        request->value[1] -= accepted;
        request->value[2] += accepted;
        request->transferred += accepted;
    }
}

bool afatfs_expand(afatfsRequest_t *request, afatfsFilePtr_t file, uint32_t size, bool allocate,
    afatfsResultCallback_t callback, void *user)
{
    if (!size || size > UINT32_MAX - afatfs_clusterSize() + 1 ||
        !afatfs_apiFileRequest(request, AFATFS_API_EXPAND, file, callback, user)) {
        return false;
    }
    request->value[0] = size;
    request->value[1] = allocate;
    request->value[2] = 2;
    return true;
}
static void afatfs_apiExpand(afatfsRequest_t *request)
{
    afatfsFilePtr_t file = request->file;
    if (!request->phase) {
        for (unsigned n = 0; n < AFATFS_MAX_OPEN_FILES; n++) {
            if (afatfs.openFiles[n].operation.active != AFATFS_FILE_OPERATION_NONE) {
                return;
            }
        }
        afatfs_fileUpdateFilesize(file);
        if (afatfs.readOnly || file->logicalSize || file->firstCluster ||
            !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) ||
            (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY) || (file->mode & AFATFS_FILE_MODE_CONTIGUOUS)) {
            afatfs_apiFinish(AFATFS_ERROR_DENIED);
            return;
        }
        if (!afatfs_flush()) {
            return;
        }
        request->value[5] = (request->value[0] - 1) / afatfs_clusterSize() + 1;
        request->phase = 1;
    }
    if (request->phase == 1) {
        for (unsigned budget = 0; budget < 128; budget++) {
            if (request->value[2] - 2 >= afatfs.numClusters) {
                afatfs_apiFinish(AFATFS_ERROR_NO_SPACE);
                return;
            }
            bool free;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                bool occupied;
                if (afatfs_exfatBitmap(request->value[2], false, false, &occupied) != AFATFS_OPERATION_SUCCESS) {
                    return;
                }
                free = !occupied;
            } else
#endif
            {
                uint32_t next;
                if (afatfs_FATGetNextCluster(0, request->value[2], &next) != AFATFS_OPERATION_SUCCESS) {
                    return;
                }
                free = !next;
            }
            if (free) {
                if (!request->value[4]) {
                    request->value[3] = request->value[2];
                }
                request->value[4]++;
            } else {
                request->value[4] = 0;
            }
            request->value[2]++;
            if (request->value[4] == request->value[5]) {
                if (!request->value[1]) {
                    afatfs.lastClusterAllocated = request->value[3];
                    afatfs_apiFinish(AFATFS_ERROR_NONE);
                    return;
                }
                request->value[6] = 0;
                request->phase = 2;
                request->checkpoint = false;
                break;
            }
        }
        if (request->phase == 1) {
            return;
        }
    }
    if (request->phase == 2) {
        if (request->value[6] < request->value[5]) {
            uint32_t cluster = request->value[3] + request->value[6];
            if (afatfs_FATSetNextCluster(
                    cluster, request->value[6] + 1 == request->value[5] ? 0xFFFFFFFF : cluster + 1) !=
                AFATFS_OPERATION_SUCCESS) {
                return;
            }
            request->phase = 3;
        } else {
            file->firstCluster = request->value[3];
            file->logicalSize = request->value[0];
            file->modified = true;
            file->physicalSize = request->value[5] * afatfs_clusterSize();
            file->cursorOffset = 0;
            file->cursorCluster = file->firstCluster;
            file->cursorPreviousCluster = 0;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                file->exfatNoFatChain = true;
                file->validDataLength = file->committedDataLength = 0;
            }
#endif
            request->phase = 4;
        }
    }
    if (request->phase == 3) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
            afatfs_exfatBitmap(request->value[3] + request->value[6], true, true, NULL) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
#endif
        request->value[6]++;
        request->phase = 2;
        return;
    }
    if (request->phase == 4) {
        if (afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        request->checkpoint = true;
        request->phase = 5;
    }
    if (request->phase == 5 && afatfs_flush()) {
        request->transferred = request->value[0];
        afatfs_apiFinish(AFATFS_ERROR_NONE);
    }
}

static void afatfs_apiFormat(afatfsRequest_t *request);
static void afatfs_apiStep(afatfsRequest_t *request)
{
    if (request->operation >= AFATFS_API_MKFS) {
        afatfs_apiFormat(request);
        return;
    }
    if (request->waiting) {
        return;
    }
    if (request->operation == AFATFS_API_OPEN) {
        if (!request->phase) {
            if (!afatfs_allocateFileHandle()) {
                afatfs_apiFinish(AFATFS_ERROR_BUSY);
                return;
            }
            request->phase = 1;
            request->waiting = true;
            if (!afatfs_fopen(request->name, request->otherName, afatfs_apiOpened, request) && request->waiting) {
                request->waiting = false;
                request->result = AFATFS_ERROR_BUSY;
            }
            return;
        }
        *(afatfsFilePtr_t *)request->output = request->file;
        request->file = NULL;
        afatfs_apiFinish(request->result);
        return;
    }
    if (request->operation >= AFATFS_API_CLOSE && request->operation <= AFATFS_API_TRUNCATE) {
        if (!request->phase) {
            request->waiting = true;
            bool accepted = request->operation == AFATFS_API_CLOSE
                ? afatfs_fclose(request->file, afatfs_apiClosed, request)
                : request->operation == AFATFS_API_SYNC ? afatfs_fsync(request->file, afatfs_apiSynced, request)
                                                        : afatfs_ftruncate(request->file, afatfs_apiSynced, request);
            if (!accepted) {
                request->waiting = false;
                afatfs_apiFinish((afatfsError_e)request->file->error);
                return;
            }
            request->phase = 1;
        }
        if (!request->waiting) {
            afatfs_apiFinish(request->result);
        }
        return;
    }
    if (request->phase >= 249) {
        afatfs_apiClose(request);
        return;
    }
    switch (request->operation) {
        case AFATFS_API_FREE:
            afatfs_apiFree(request);
            return;
        case AFATFS_API_GETLABEL:
        case AFATFS_API_SETLABEL:
            afatfs_apiLabel(request);
            return;
        case AFATFS_API_CWD:
            afatfs_apiCwd(request);
            return;
        case AFATFS_API_EXPAND:
            afatfs_apiExpand(request);
            return;
        case AFATFS_API_PUTS:
        case AFATFS_API_GETS:
        case AFATFS_API_READ:
        case AFATFS_API_WRITE:
        case AFATFS_API_FORWARD:
            afatfs_apiText(request);
            return;
        default:
            break;
    }
    if (!request->phase) {
        if (afatfs.cwdWalkMutex) {
            return;
        }
        uint8_t mode = AFATFS_FILE_MODE_READ;
        if (request->operation == AFATFS_API_UNLINK || request->operation == AFATFS_API_RMDIR) {
            mode |= AFATFS_FILE_MODE_WRITE;
        }
        if (request->operation == AFATFS_API_CHMOD || request->operation == AFATFS_API_UTIME ||
            request->operation == AFATFS_API_RENAME) {
            mode |= AFATFS_FILE_MODE_METADATA;
        }
        if (afatfs_apiOpen(request, mode)) {
            request->phase = 1;
        }
        return;
    }
    if (!request->file || request->result) {
        afatfs_apiClose(request);
        return;
    }
    if (request->operation == AFATFS_API_STAT) {
        afatfsOperationStatus_e status = afatfs_fstat(request->file, (afatfsFileInfo_t *)request->output);
        if (status == AFATFS_OPERATION_IN_PROGRESS) {
            return;
        }
        if (status == AFATFS_OPERATION_FAILURE) {
            request->result = afatfs.lastError;
        }
        afatfs_apiClose(request);
    } else if (request->operation == AFATFS_API_OPENDIR || request->operation == AFATFS_API_FIND) {
        if (request->file->type != AFATFS_FILE_TYPE_DIRECTORY &&
            request->file->type != AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
            request->result = AFATFS_ERROR_DENIED;
        } else {
            afatfsDirectory_t *dir = (afatfsDirectory_t *)request->output;
            if (request->phase == 1) {
                afatfs_apiDirectory(dir, request->file);
                dir->registered = true;
                dir->next = afatfs_directories;
                afatfs_directories = dir;
                dir->pattern = request->operation == AFATFS_API_FIND ? request->otherName : NULL;
                request->phase = 2;
            }
            if (request->operation == AFATFS_API_FIND) {
                afatfsOperationStatus_e status = afatfs_readdir(dir, (afatfsFileInfo_t *)request->consumerUser);
                if (status == AFATFS_OPERATION_IN_PROGRESS) {
                    return;
                }
                if (status == AFATFS_OPERATION_FAILURE) {
                    request->result = afatfs.lastError;
                    afatfs_closedir(dir);
                }
            }
        }
        afatfs_apiClose(request);
    } else if (request->operation == AFATFS_API_CHMOD || request->operation == AFATFS_API_UTIME) {
        afatfs_apiMetadata(request);
    } else if (request->operation == AFATFS_API_RENAME) {
        afatfs_apiRename(request);
    } else {
        if (request->phase == 1) {
            bool dir = request->file->type != AFATFS_FILE_TYPE_NORMAL;
            if (dir != (request->operation == AFATFS_API_RMDIR)) {
                request->result = AFATFS_ERROR_DENIED;
                afatfs_apiClose(request);
                return;
            }
            request->waiting = true;
            if (!afatfs_funlink(request->file, afatfs_apiClosed, request)) {
                request->waiting = false;
                request->result = (afatfsError_e)request->file->error;
                afatfs_apiClose(request);
                return;
            }
            request->phase = 2;
        } else {
            request->result = (afatfsError_e)request->file->error;
            if (request->file->type == AFATFS_FILE_TYPE_NONE) {
                request->file = NULL;
            }
            afatfs_apiClose(request);
        }
    }
}
static void afatfs_apiPoll(void)
{
    afatfsRequest_t *request = afatfs_request;
    if (!request) {
        return;
    }
    request->driving = true;
    afatfs_apiStep(request);
    if (afatfs_request == request) {
        request->driving = false;
    }
}

#include "asyncfatfs_format.inc"
