/* Extended asynchronous API. One caller-owned request is driven by the original poll loop.
 * No heap, second cache, or synchronous storage path is used. */
#include <stdarg.h>
#include <stdio.h>

#ifndef AFATFS_USE_FREEFILE
uint32_t afatfs_getContiguousFreeSpace(void) { return 0; }
#endif

static afatfsOperationStatus_e afatfs_apiError(afatfsError_e error)
{
    if (afatfs_mediaSessionActive()) afatfs.lastError = error;
    return AFATFS_OPERATION_FAILURE;
}

static unsigned afatfs_apiUpper(unsigned c)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && c < 128) return afatfs.upcaseAscii[c];
#endif
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static void afatfs_apiShortName(char *out, const uint8_t *name)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 8 && name[i] != ' '; i++) out[n++] = (char)name[i];
    if (name[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && name[i] != ' '; i++) out[n++] = (char)name[i];
    }
    out[n] = 0;
}

static void afatfs_apiFatInfo(afatfsFileInfo_t *info, const fatDirectoryEntry_t *entry)
{
    memset(info, 0, sizeof(*info));
    afatfs_apiShortName(info->name, entry->fileName);
    info->attributes = entry->attrib;
    info->firstCluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
    info->size = entry->fileSize; info->validSize = info->size;
    info->allocatedSize = info->size <= UINT32_MAX - afatfs_clusterSize() + 1 ? roundUpTo(info->size, afatfs_clusterSize()) : UINT32_MAX;
    info->created = ((uint32_t)entry->creationDate << 16) | entry->creationTime;
    info->modified = ((uint32_t)entry->lastWriteDate << 16) | entry->lastWriteTime;
    info->accessed = (uint32_t)entry->lastAccessDate << 16;
    if (info->attributes & FAT_FILE_ATTRIBUTE_DIRECTORY) info->size = info->validSize = 0;
}

afatfsOperationStatus_e afatfs_fstat(afatfsFilePtr_t file, afatfsFileInfo_t *info)
{
    if (!afatfs_mediaSessionActive() || !file || file->type == AFATFS_FILE_TYPE_NONE || !info)
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    if (afatfs_fileIsBusy(file)) return AFATFS_OPERATION_IN_PROGRESS;
    if (!file->directoryEntryPos.sectorNumberPhysical) {
        memset(info, 0, sizeof(*info)); strcpy(info->name, "/");
        info->attributes = FAT_FILE_ATTRIBUTE_DIRECTORY; info->firstCluster = file->firstCluster;
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
            const uint8_t *p = file->exfatEntries + (2 + n / 15) * 32 + 2 + n % 15 * 2;
            if (p[1] || p[0] >= 128) return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
            info->name[n] = (char)p[0];
        }
    } else
#endif
    {
        fatDirectoryEntry_t *entry;
        if (!afatfs_fGetFileInfo(file, &entry)) return AFATFS_OPERATION_IN_PROGRESS;
        if (entry) afatfs_apiFatInfo(info, entry);
        else { memset(info, 0, sizeof(*info)); strcpy(info->name, "/"); info->attributes = FAT_FILE_ATTRIBUTE_DIRECTORY; }
    }
    afatfs_fileUpdateFilesize(file);
    info->firstCluster = file->firstCluster; info->allocatedSize = file->physicalSize;
    info->size = file->type == AFATFS_FILE_TYPE_NORMAL ? file->logicalSize : 0;
    info->validSize = info->size;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        info->validSize = MIN(info->size, file->validDataLength); info->contiguous = file->exfatNoFatChain;
    }
#endif
    return AFATFS_OPERATION_SUCCESS;
}

static void afatfs_apiDirectory(afatfsDirectory_t *dir, afatfsFilePtr_t file)
{
    memset(dir, 0, sizeof(*dir));
    dir->generation = afatfs_mediaGeneration;
    dir->cursor.firstCluster = dir->cursor.cluster = file ? file->firstCluster : afatfs.rootDirectoryCluster;
    dir->cursor.root16 = file ? file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY
        : afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (file && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        dir->cursor.contiguous = file->exfatNoFatChain;
        dir->cursor.clusters = file->physicalSize / afatfs_clusterSize();
    }
#endif
}

static afatfsOperationStatus_e afatfs_apiDirectoryEntry(afatfsDirectory_t *dir, uint8_t **entry, afatfsDirEntryPointer_t *position)
{
    afatfsDirectoryCursor_t *c = &dir->cursor;
    if (c->ended) { *entry = NULL; return AFATFS_OPERATION_SUCCESS; }
    uint32_t sectorOffset = c->offset / 16;
    uint32_t sector;
    if (c->root16) {
        if (sectorOffset >= afatfs.rootDirectorySectors) { c->ended = true; *entry = NULL; return AFATFS_OPERATION_SUCCESS; }
        sector = afatfs.clusterStartSector - afatfs.rootDirectorySectors + sectorOffset;
    } else {
        uint32_t wanted = sectorOffset / afatfs.sectorsPerCluster;
        if (wanted >= afatfs.numClusters) return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
        while (c->clusterIndex < wanted) {
            uint32_t next;
            if (c->contiguous) next = c->clusterIndex + 1 >= c->clusters ? 0xFFFFFFFF : c->cluster + 1;
            else {
                afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, c->cluster, &next);
                if (status != AFATFS_OPERATION_SUCCESS) return status;
            }
            if (afatfs_FATIsEndOfChainMarker(next)) { c->ended = true; *entry = NULL; return AFATFS_OPERATION_SUCCESS; }
            c->cluster = next; c->clusterIndex++;
        }
        if (c->cluster < 2 || c->cluster - 2 >= afatfs.numClusters) return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
        sector = afatfs_fileClusterToPhysical(c->cluster, sectorOffset % afatfs.sectorsPerCluster);
    }
    uint8_t *buffer;
    afatfsOperationStatus_e status = afatfs_cacheSector(sector, &buffer, AFATFS_CACHE_READ, 0);
    if (status != AFATFS_OPERATION_SUCCESS) return status;
    position->sectorNumberPhysical = sector; position->entryIndex = (int16_t)(c->offset % 16);
    *entry = buffer + position->entryIndex * 32;
    c->offset++;
    return AFATFS_OPERATION_SUCCESS;
}

static bool afatfs_apiMatch(const char *pattern, const char *name)
{
    const char *star = NULL, *retry = NULL;
    while (*name) {
        if (*pattern == '?' || afatfs_apiUpper((uint8_t)*pattern) == afatfs_apiUpper((uint8_t)*name)) { pattern++; name++; }
        else if (*pattern == '*') { star = pattern++; retry = name; }
        else if (star) { pattern = star + 1; name = ++retry; }
        else return false;
    }
    while (*pattern == '*') pattern++;
    return !*pattern;
}

static afatfsOperationStatus_e afatfs_apiReadDirectory(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    if (!afatfs_mediaSessionActive() || !dir || !info || dir->generation != afatfs_mediaGeneration)
        return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    if (dir->remaining && dir->output != info) return afatfs_apiError(AFATFS_ERROR_INVALID_PARAMETER);
    for (unsigned budget = 0; budget < 16; budget++) {
        uint8_t *bytes; afatfsDirEntryPointer_t pos;
        afatfsOperationStatus_e status = afatfs_apiDirectoryEntry(dir, &bytes, &pos);
        if (status != AFATFS_OPERATION_SUCCESS) return status;
        if (!bytes || !bytes[0]) {
            dir->cursor.ended = true; dir->remaining = 0; memset(info, 0, sizeof(*info));
            return AFATFS_OPERATION_SUCCESS;
        }
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (!dir->remaining) {
                if (bytes[0] != EXFAT_DIR_FILEDIR) continue;
                if (bytes[1] < 2 || bytes[1] >= AFATFS_EXFAT_MAX_ENTRIES) return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                memset(info, 0, sizeof(*info)); dir->output = info;
                const exFatFileEntry_t *primary = (const exFatFileEntry_t *)bytes;
                info->attributes = (uint8_t)primary->fileAttirb;
                info->created = ((uint32_t)primary->creationDate << 16) | primary->creationTime;
                info->modified = ((uint32_t)primary->lastWriteDate << 16) | primary->lastWriteTime;
                info->accessed = ((uint32_t)primary->lastAccessDate << 16) | primary->lastAccessTime;
                dir->expectedChecksum = primary->checksum; dir->checksum = 0;
                dir->remaining = bytes[1] + 1; dir->entryIndex = 0; dir->nameWritten = 0;
            }
            for (unsigned n = 0; n < 32; n++) {
                if (dir->entryIndex == 0 && (n == 2 || n == 3)) continue;
                dir->checksum = (uint16_t)((dir->checksum >> 1 | (uint32_t)dir->checksum << 15) + bytes[n]);
            }
            if (dir->entryIndex == 1) {
                const exFatStreamEntry_t *stream = (const exFatStreamEntry_t *)bytes;
                if (bytes[0] != EXFAT_DIR_STREAM || !stream->nameLength || stream->dataSize > UINT32_MAX
                    || stream->fileSize > stream->dataSize || stream->dataSize > UINT32_MAX - afatfs_clusterSize() + 1)
                    return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                dir->nameLength = stream->nameLength; dir->nameHash = stream->nameHash;
                info->firstCluster = stream->firstCluster; info->size = (uint32_t)stream->dataSize;
                info->validSize = (uint32_t)stream->fileSize; info->allocatedSize = roundUpTo(info->size, afatfs_clusterSize());
                info->contiguous = (stream->flags & 2) != 0;
            } else if (dir->entryIndex >= 2 && dir->nameWritten < dir->nameLength) {
                if (bytes[0] != EXFAT_DIR_FILENAME) return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                for (unsigned n = 0; n < 15 && dir->nameWritten < dir->nameLength; n++) {
                    if (!bytes[2 + n * 2] || bytes[2 + n * 2] >= 128 || bytes[3 + n * 2])
                        return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
                    info->name[dir->nameWritten++] = (char)bytes[2 + n * 2];
                }
            } else if (dir->entryIndex >= 2 && (bytes[0] & 0xE0) != 0xE0)
                return afatfs_apiError(AFATFS_ERROR_UNSUPPORTED);
            dir->entryIndex++;
            if (--dir->remaining) continue;
            uint16_t hash = 0;
            for (unsigned n = 0; n < dir->nameWritten; n++) {
                uint16_t ch = afatfs.upcaseAscii[(uint8_t)info->name[n]];
                hash = (uint16_t)((hash >> 1 | (uint32_t)hash << 15) + (ch & 255));
                hash = (uint16_t)((hash >> 1 | (uint32_t)hash << 15) + (ch >> 8));
            }
            if (dir->checksum != dir->expectedChecksum || dir->nameWritten != dir->nameLength || hash != dir->nameHash)
                return afatfs_apiError(AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
            if (info->attributes & FAT_FILE_ATTRIBUTE_DIRECTORY) info->size = info->validSize = 0;
        } else
#endif
        {
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)bytes;
            if (fat_isDirectoryEntryEmpty(entry) || fat_isVolumeLabel(entry) || fat_isLfnEntry(entry)
                || !memcmp(entry->fileName, ".          ", 11) || !memcmp(entry->fileName, "..         ", 11)) continue;
            afatfs_apiFatInfo(info, entry);
        }
        if (!dir->pattern || afatfs_apiMatch(dir->pattern, info->name)) return AFATFS_OPERATION_SUCCESS;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

afatfsOperationStatus_e afatfs_readdir(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    afatfsOperationStatus_e status = afatfs_apiReadDirectory(dir, info);
    if (dir && dir->generation == afatfs_mediaGeneration) dir->reading = status == AFATFS_OPERATION_IN_PROGRESS;
    return status;
}
afatfsOperationStatus_e afatfs_findnext(afatfsDirectory_t *dir, afatfsFileInfo_t *info) { return afatfs_readdir(dir, info); }
afatfsOperationStatus_e afatfs_rewinddir(afatfsDirectory_t *dir)
{
    if (!dir || dir->generation != afatfs_mediaGeneration) return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    dir->cursor.cluster = dir->cursor.firstCluster; dir->cursor.clusterIndex = dir->cursor.offset = 0;
    dir->cursor.ended = false; dir->remaining = 0; dir->output = NULL;
    dir->reading = false;
    return AFATFS_OPERATION_SUCCESS;
}
afatfsOperationStatus_e afatfs_closedir(afatfsDirectory_t *dir)
{
    if (!dir || dir->generation != afatfs_mediaGeneration) return afatfs_apiError(AFATFS_ERROR_INVALID_OBJECT);
    afatfsDirectory_t **link = &afatfs_directories;
    while (*link && *link != dir) link = &(*link)->next;
    if (*link) *link = dir->next;
    dir->registered = false; dir->reading = false; dir->generation = 0;
    return AFATFS_OPERATION_SUCCESS;
}

static bool afatfs_apiBegin(afatfsRequest_t *r, unsigned operation, afatfsResultCallback_t callback, void *user)
{
    if (!r || afatfs_request || afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        if (afatfs_mediaSessionActive()) afatfs.lastError = AFATFS_ERROR_BUSY;
        return false;
    }
    memset(r, 0, sizeof(*r)); r->operation = (uint8_t)operation;
    r->callback = callback; r->user = user; r->checkpoint = true;
    afatfs_request = r;
    return true;
}

static void afatfs_apiFinish(afatfsError_e result)
{
    afatfsRequest_t *r = afatfs_request;
    afatfsResultCallback_t callback = r->callback; void *user = r->user;
    r->result = result; r->complete = true; r->driving = false;
    if (r->file && r->file->type != AFATFS_FILE_TYPE_NONE) r->file->error = (uint8_t)result;
    if (result) afatfs.lastError = result;
    afatfs_request = NULL;
    if (callback) callback(result, user);
}

static void afatfs_apiOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *r = (afatfsRequest_t *)user;
    r->file = file; r->waiting = false;
    if (!file) r->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_NOT_FOUND;
}
static void afatfs_apiClosed(void *user) { ((afatfsRequest_t *)user)->waiting = false; }
static void afatfs_apiSynced(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *r = (afatfsRequest_t *)user; r->waiting = false;
    if (!file) r->result = afatfs.lastError;
}

static bool afatfs_apiOpen(afatfsRequest_t *r, uint8_t mode)
{
    afatfsFilePtr_t file = afatfs_allocateFileHandle();
    if (!file) return false;
    r->waiting = true;
    afatfs_createFile(file, r->name, FAT_FILE_ATTRIBUTE_ARCHIVE, mode, afatfs_apiOpened, r);
    return true;
}

static void afatfs_apiClose(afatfsRequest_t *r)
{
    r->checkpoint = true;
    if (!r->file) { afatfs_apiFinish(r->result); return; }
    if (r->phase != 250) {
        r->waiting = true;
        if (!afatfs_fclose(r->file, afatfs_apiClosed, r)) { r->waiting = false; return; }
        r->phase = 250;
    }
    if (!r->waiting) {
        r->file = r->other; r->other = NULL;
        if (r->file) r->phase = 249;
        else afatfs_apiFinish(r->result);
    }
}

bool afatfs_stat(afatfsRequest_t *r, const char *name, afatfsFileInfo_t *info, afatfsResultCallback_t cb, void *user)
{
    if (!name || !info || !afatfs_apiBegin(r, AFATFS_API_STAT, cb, user)) return false;
    r->name = name; r->output = info; return true;
}
bool afatfs_opendir(afatfsRequest_t *r, const char *name, afatfsDirectory_t *dir, afatfsResultCallback_t cb, void *user)
{
    for (afatfsDirectory_t *open = afatfs_directories; open; open = open->next) if (open == dir) return false;
    if (!name || !dir || !afatfs_apiBegin(r, AFATFS_API_OPENDIR, cb, user)) return false;
    r->name = name; r->output = dir; return true;
}
bool afatfs_findfirst(afatfsRequest_t *r, afatfsDirectory_t *dir, afatfsFileInfo_t *info,
    const char *name, const char *pattern, afatfsResultCallback_t cb, void *user)
{
    for (afatfsDirectory_t *open = afatfs_directories; open; open = open->next) if (open == dir) return false;
    if (!name || !dir || !info || !pattern || !afatfs_apiBegin(r, AFATFS_API_FIND, cb, user)) return false;
    r->name = name; r->otherName = pattern; r->output = dir; r->consumerUser = info; return true;
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

static bool afatfs_apiNamed(afatfsRequest_t *r, unsigned op, const char *name, afatfsResultCallback_t cb, void *user)
{
    if (!name || !afatfs_apiBegin(r, op, cb, user)) return false;
    r->name = name; return true;
}
bool afatfs_unlink(afatfsRequest_t *r, const char *name, afatfsResultCallback_t cb, void *user)
{ return afatfs_apiNamed(r, AFATFS_API_UNLINK, name, cb, user); }
bool afatfs_rmdir(afatfsRequest_t *r, const char *name, afatfsResultCallback_t cb, void *user)
{ return afatfs_apiNamed(r, AFATFS_API_RMDIR, name, cb, user); }
bool afatfs_rename(afatfsRequest_t *r, const char *oldName, const char *newName, afatfsResultCallback_t cb, void *user)
{
    if (!newName || !afatfs_apiNamed(r, AFATFS_API_RENAME, oldName, cb, user)) return false;
    r->otherName = newName; return true;
}
bool afatfs_chmod(afatfsRequest_t *r, const char *name, uint8_t attr, uint8_t mask, afatfsResultCallback_t cb, void *user)
{
    if (!afatfs_apiNamed(r, AFATFS_API_CHMOD, name, cb, user)) return false;
    r->value[0] = attr; r->value[1] = mask & (FAT_FILE_ATTRIBUTE_READ_ONLY | FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_ARCHIVE);
    return true;
}
bool afatfs_utime(afatfsRequest_t *r, const char *name, uint32_t modified, afatfsResultCallback_t cb, void *user)
{
    if (!afatfs_apiNamed(r, AFATFS_API_UTIME, name, cb, user)) return false;
    r->value[0] = modified; return true;
}
bool afatfs_getfree(afatfsRequest_t *r, afatfsVolumeInfo_t *info, afatfsResultCallback_t cb, void *user)
{
    if (!info || !afatfs_apiBegin(r, AFATFS_API_FREE, cb, user)) return false;
    r->output = info; r->value[0] = 2; return true;
}
bool afatfs_getlabel(afatfsRequest_t *r, afatfsVolumeInfo_t *info, afatfsResultCallback_t cb, void *user)
{
    if (!info || !afatfs_apiBegin(r, AFATFS_API_GETLABEL, cb, user)) return false;
    r->output = info; return true;
}
bool afatfs_setlabel(afatfsRequest_t *r, const char *name, afatfsResultCallback_t cb, void *user)
{
    if (!name || strlen(name) > 11) return false;
    for (unsigned n = 0; name[n]; n++) if ((uint8_t)name[n] < 32 || (uint8_t)name[n] >= 127 || strchr("\"*+,./:;<=>?[\\]|", name[n]))
        { afatfs.lastError = AFATFS_ERROR_INVALID_NAME; return false; }
    return afatfs_apiNamed(r, AFATFS_API_SETLABEL, name, cb, user);
}
bool afatfs_getcwd(afatfsRequest_t *r, char *buffer, uint32_t capacity, afatfsResultCallback_t cb, void *user)
{
    if (!buffer || capacity < 2 || !afatfs_apiBegin(r, AFATFS_API_CWD, cb, user)) return false;
    r->output = buffer; r->value[1] = capacity; return true;
}

static void afatfs_apiFree(afatfsRequest_t *r)
{
    if (!r->phase) {
        for (unsigned n = 0; n < AFATFS_MAX_OPEN_FILES; n++)
            if (afatfs.openFiles[n].operation.active != AFATFS_FILE_OPERATION_NONE) return;
        if (afatfs.cwdWalkMutex || afatfs.currentDirectory.operation.active != AFATFS_FILE_OPERATION_NONE) return;
        r->phase = 1;
    }
    for (unsigned budget = 0; budget < 128 && r->value[0] - 2 < afatfs.numClusters; budget++) {
        bool free;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            bool occupied;
            afatfsOperationStatus_e status = afatfs_exfatBitmap(r->value[0], false, false, &occupied);
            if (status == AFATFS_OPERATION_FAILURE) { afatfs_apiFinish(AFATFS_ERROR_BAD_FILESYSTEM_HEADER); return; }
            if (status != AFATFS_OPERATION_SUCCESS) return;
            free = !occupied;
        } else
#endif
        {
            uint32_t next;
            afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, r->value[0], &next);
            if (status == AFATFS_OPERATION_FAILURE) { afatfs_apiFinish(AFATFS_ERROR_BAD_FILESYSTEM_HEADER); return; }
            if (status != AFATFS_OPERATION_SUCCESS) return;
            free = next == 0;
        }
        if (free) r->value[2]++;
        r->value[0]++;
    }
    if (r->value[0] - 2 == afatfs.numClusters) {
        afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)r->output;
        info->clusterSize = afatfs_clusterSize(); info->totalClusters = afatfs.numClusters; info->freeClusters = r->value[2];
        afatfs_apiFinish(AFATFS_ERROR_NONE);
    }
}

static void afatfs_apiLabel(afatfsRequest_t *r)
{
    if (!r->phase) {
        if (r->operation == AFATFS_API_SETLABEL && afatfs.readOnly) { afatfs_apiFinish(AFATFS_ERROR_DENIED); return; }
        if (afatfs.cwdWalkMutex) return;
        uint8_t *boot;
        if (afatfs_cacheSector(afatfs.partitionStartSector, &boot, AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS) return;
        if (r->operation == AFATFS_API_GETLABEL) {
            afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)r->output;
            memset(info, 0, sizeof(*info));
            fatVolumeID_t *volume = (fatVolumeID_t *)boot;
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) info->serial = volume->fat.fatDescriptor.fat16.volumeID;
            else if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) info->serial = volume->fat.fatDescriptor.fat32.volumeID;
#ifdef ASYNCFS_ENABLE_EXFAT
            else info->serial = afatfs.volumeSerial;
#endif
            info->clusterSize = afatfs_clusterSize(); info->totalClusters = afatfs.numClusters;
        }
        afatfs_apiDirectory(&r->directory, NULL); r->phase = 1;
    }
    if (r->phase == 1) {
        uint8_t *entry; afatfsDirEntryPointer_t pos;
        afatfsOperationStatus_e status = afatfs_apiDirectoryEntry(&r->directory, &entry, &pos);
        if (status != AFATFS_OPERATION_SUCCESS) return;
        bool label = entry && (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT ? entry[0] == EXFAT_DIR_VLABEL
            : !fat_isDirectoryEntryEmpty((fatDirectoryEntry_t *)entry) && ((fatDirectoryEntry_t *)entry)->attrib == FAT_FILE_ATTRIBUTE_VOLUME_ID);
        bool end = !entry || !entry[0];
        if (r->operation == AFATFS_API_GETLABEL && (label || end)) {
            afatfsVolumeInfo_t *info = (afatfsVolumeInfo_t *)r->output;
            if (label) {
                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                    if (entry[1] > 11) { afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED); return; }
                    for (unsigned n = 0; n < entry[1]; n++) {
                        if (entry[3 + 2 * n] || entry[2 + 2 * n] >= 128) { afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED); return; }
                        info->label[n] = (char)entry[2 + 2 * n];
                    }
                } else {
                    memcpy(info->label, entry, 11);
                    for (int n = 10; n >= 0 && info->label[n] == ' '; n--) info->label[n] = 0;
                }
            }
            afatfs_apiFinish(AFATFS_ERROR_NONE); return;
        }
        if (r->operation == AFATFS_API_GETLABEL) return;
        if (entry && !r->value[4] && (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT ? !(entry[0] & 0x80) : entry[0] == 0xE5)) {
            r->value[4] = pos.sectorNumberPhysical; r->value[5] = (uint32_t)pos.entryIndex;
        }
        if (label) { r->value[4] = pos.sectorNumberPhysical; r->value[5] = (uint32_t)pos.entryIndex; r->value[6] = 1; }
        if (end && entry && !r->value[4]) {
            r->value[4] = pos.sectorNumberPhysical; r->value[5] = (uint32_t)pos.entryIndex; r->value[7] = 1;
        }
        if (!label && !end) return;
        if (!r->name[0] && !label) { afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
        if (!r->value[4]) {
            if (r->directory.cursor.root16) { afatfs_apiFinish(AFATFS_ERROR_NO_SPACE); return; }
            r->phase = 6;
        } else r->phase = 5;
    }
    if (r->phase == 6) {
        if (!r->file) {
            r->file = afatfs_allocateFileHandle(); if (!r->file) return;
            afatfs_initFileHandle(r->file); r->file->type = AFATFS_FILE_TYPE_DIRECTORY;
            r->file->mode = AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE;
            r->file->attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
            r->file->firstCluster = afatfs.rootDirectoryCluster;
            r->file->cursorPreviousCluster = r->directory.cursor.cluster;
            r->file->physicalSize = (r->directory.cursor.clusterIndex + 1) * afatfs_clusterSize();
            r->file->cursorOffset = r->file->physicalSize; r->value[8] = r->file->physicalSize;
        }
        afatfsOperationStatus_e status = afatfs_extendSubdirectory(r->file, NULL, NULL, NULL);
        if (status == AFATFS_OPERATION_FAILURE) { r->result = AFATFS_ERROR_NO_SPACE; afatfs_apiClose(r); return; }
        r->phase = 7;
    }
    if (r->phase == 7) {
        if (r->file->operation.active != AFATFS_FILE_OPERATION_NONE) return;
        if (r->file->physicalSize == r->value[8]) { r->result = AFATFS_ERROR_NO_SPACE; afatfs_apiClose(r); return; }
        r->directory.cursor.ended = false; r->phase = 1; return;
    }
    if (r->phase == 5) {
        if (!afatfs_flush()) return;
        r->checkpoint = false; r->phase = r->value[7] ? 2 : 3;
    }
    if (r->phase == 2) {
        uint8_t *entry; afatfsDirEntryPointer_t pos;
        afatfsDirectoryCursor_t before = r->directory.cursor;
        if (afatfs_apiDirectoryEntry(&r->directory, &entry, &pos) != AFATFS_OPERATION_SUCCESS) return;
        if (entry) {
            uint8_t *sector;
            if (afatfs_cacheSector(pos.sectorNumberPhysical, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) {
                r->directory.cursor = before; return;
            }
            memset(sector + pos.entryIndex * 32, 0, 32);
        }
        r->phase = 3;
    }
    if (r->phase == 3) {
        uint8_t *sector;
        if (afatfs_cacheSector(r->value[4], &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) return;
        uint8_t *entry = sector + r->value[5] * 32;
        if (!r->name[0]) entry[0] = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT ? 0x03 : 0xE5;
        else {
            memset(entry, 0, 32);
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                entry[0] = EXFAT_DIR_VLABEL; entry[1] = (uint8_t)strlen(r->name);
                for (unsigned n = 0; r->name[n]; n++) entry[2 + n * 2] = (uint8_t)r->name[n];
            } else {
                memset(entry, ' ', 11); entry[11] = FAT_FILE_ATTRIBUTE_VOLUME_ID;
                for (unsigned n = 0; r->name[n]; n++) entry[n] = (uint8_t)afatfs_apiUpper((uint8_t)r->name[n]);
            }
        }
        r->phase = 4; r->checkpoint = true;
    }
    if (r->phase == 4 && afatfs_flush()) afatfs_apiClose(r);
}

typedef struct afatfsPathFrame_t { afatfsDirectoryCursor_t cursor; uint32_t length; } afatfsPathFrame_t;
static void afatfs_apiCwd(afatfsRequest_t *r)
{
    char *path = (char *)r->output;
    if (!r->phase) {
        strcpy(path, "/"); r->value[2] = 1; r->value[3] = afatfs.currentDirectory.firstCluster;
        if (afatfs.currentDirectory.type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY || r->value[3] == afatfs.rootDirectoryCluster) {
            afatfs_apiFinish(AFATFS_ERROR_NONE); return;
        }
        afatfs_apiDirectory(&r->directory, NULL); r->phase = 1;
    }
    afatfsOperationStatus_e status = afatfs_readdir(&r->directory, &r->info);
    if (status == AFATFS_OPERATION_FAILURE) { afatfs_apiFinish(afatfs.lastError); return; }
    if (status != AFATFS_OPERATION_SUCCESS) return;
    if (!r->info.name[0]) {
        if (!r->value[0]) { afatfs_apiFinish(AFATFS_ERROR_NOT_FOUND); return; }
        afatfsPathFrame_t frame;
        memcpy(&frame, r->scratch + --r->value[0] * sizeof(frame), sizeof(frame));
        memset(&r->directory, 0, sizeof(r->directory)); r->directory.generation = afatfs_mediaGeneration;
        r->directory.cursor = frame.cursor; r->value[2] = frame.length; path[frame.length] = 0;
        return;
    }
    if (!(r->info.attributes & FAT_FILE_ATTRIBUTE_DIRECTORY)) return;
    uint32_t oldLength = r->value[2], length = (uint32_t)strlen(r->info.name);
    bool separator = oldLength > 1;
    if (oldLength + separator + length >= r->value[1]) { afatfs_apiFinish(AFATFS_ERROR_INVALID_PARAMETER); return; }
    if (separator) path[oldLength] = '/';
    memcpy(path + oldLength + separator, r->info.name, length + 1);
    if (r->info.firstCluster == r->value[3]) { afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
    if ((r->value[0] + 1) * sizeof(afatfsPathFrame_t) > sizeof(r->scratch)) { afatfs_apiFinish(AFATFS_ERROR_UNSUPPORTED); return; }
    afatfsPathFrame_t frame; frame.cursor = r->directory.cursor; frame.length = oldLength;
    memcpy(r->scratch + r->value[0]++ * sizeof(frame), &frame, sizeof(frame));
    memset(&r->directory, 0, sizeof(r->directory)); r->directory.generation = afatfs_mediaGeneration;
    r->directory.cursor.cluster = r->directory.cursor.firstCluster = r->info.firstCluster;
    r->directory.cursor.contiguous = r->info.contiguous; r->directory.cursor.clusters = r->info.allocatedSize / afatfs_clusterSize();
    r->value[2] = oldLength + separator + length;
}

static void afatfs_apiOtherOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *r = (afatfsRequest_t *)user;
    r->other = file; r->waiting = false;
    if (!file) r->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_EXISTS;
}

static void afatfs_apiMetadata(afatfsRequest_t *r)
{
    afatfsFilePtr_t file = r->file;
    if (afatfs.readOnly || !file->directoryEntryPos.sectorNumberPhysical) {
        r->result = AFATFS_ERROR_DENIED; afatfs_apiClose(r); return;
    }
    if (r->phase == 1) {
        if (!afatfs_flush()) return;
        r->checkpoint = false;
        if (r->operation == AFATFS_API_CHMOD)
            file->attrib = (uint8_t)((file->attrib & ~r->value[1]) | (r->value[0] & r->value[1]));
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
            primary->fileAttirb = file->attrib;
            if (r->operation == AFATFS_API_UTIME) {
                primary->lastWriteDate = (uint16_t)(r->value[0] >> 16); primary->lastWriteTime = (uint16_t)r->value[0];
                primary->lastWriteTimeTenths = 0; primary->lastWriteUtcOffset = 0;
            }
            primary->checksum = exFat_entrySetChecksum(file->exfatEntries, file->exfatEntryCount * 32);
        }
#endif
        r->phase = 2;
    }
    if (r->phase == 2) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (afatfs_exfatWriteEntrySet(file) != AFATFS_OPERATION_SUCCESS) return;
        } else
#endif
        {
            uint8_t *buffer;
            if (afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &buffer,
                AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) return;
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)buffer + file->directoryEntryPos.entryIndex;
            entry->attrib = file->attrib;
            if (r->operation == AFATFS_API_UTIME) { entry->lastWriteDate = (uint16_t)(r->value[0] >> 16); entry->lastWriteTime = (uint16_t)r->value[0]; }
        }
        r->phase = 3; r->checkpoint = true;
    }
    afatfs_apiClose(r);
}

static void afatfs_apiRename(afatfsRequest_t *r)
{
    afatfsFilePtr_t source = r->file;
    if (r->phase == 1) {
        #ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            unsigned extras = source->exfatEntryCount - 2 - (((exFatStreamEntry_t *)(source->exfatEntries + 32))->nameLength + 14) / 15;
            if (2 + (strlen(r->otherName) + 14) / 15 + extras > AFATFS_EXFAT_MAX_ENTRIES) {
                r->result = AFATFS_ERROR_UNSUPPORTED; afatfs_apiClose(r); return;
            }
        }
        #endif
        if (afatfs.readOnly || !source->directoryEntryPos.sectorNumberPhysical
            || (source->type == AFATFS_FILE_TYPE_DIRECTORY && source->firstCluster == afatfs.currentDirectory.firstCluster)) {
            r->result = AFATFS_ERROR_DENIED; afatfs_apiClose(r); return;
        }
        if (source->type == AFATFS_FILE_TYPE_DIRECTORY) {
            for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next)
                if (dir->cursor.firstCluster == source->firstCluster) { r->result = AFATFS_ERROR_DENIED; afatfs_apiClose(r); return; }
        }
        if (!afatfs_flush()) return;
        afatfsFilePtr_t target = afatfs_allocateFileHandle();
        if (!target) return;
        r->phase = 2; r->waiting = true; r->checkpoint = false;
        afatfs_createFile(target, r->otherName, FAT_FILE_ATTRIBUTE_ARCHIVE,
            AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_EXCLUSIVE_CREATE, afatfs_apiOtherOpened, r);
        return;
    }
    afatfsFilePtr_t target = r->other;
    if (r->phase == 2) {
        if (!target) { afatfs_apiClose(r); return; }
        target->firstCluster = source->firstCluster; target->physicalSize = source->physicalSize;
        target->logicalSize = source->logicalSize; target->attrib = source->attrib; target->type = source->type;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            target->validDataLength = source->validDataLength; target->committedDataLength = source->committedDataLength;
            target->exfatNoFatChain = source->exfatNoFatChain;
            memcpy(target->exfatEntries + 4, source->exfatEntries + 4, 28);
            exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(target->exfatEntries + 32);
            stream->flags = 1 | (target->exfatNoFatChain ? 2 : 0); stream->firstCluster = target->firstCluster;
            stream->dataSize = target->type == AFATFS_FILE_TYPE_DIRECTORY ? target->physicalSize : target->logicalSize;
            stream->fileSize = target->type == AFATFS_FILE_TYPE_DIRECTORY ? target->physicalSize : target->validDataLength;
            ((exFatFileEntry_t *)target->exfatEntries)->checksum = exFat_entrySetChecksum(target->exfatEntries, target->exfatEntryCount * 32);
        } else
#endif
        {
            fatDirectoryEntry_t *entry;
            if (!afatfs_fGetFileInfo(source, &entry)) return;
            memcpy(r->scratch, entry, 32);
        }
        r->phase = 3;
    }
    if (r->phase == 3) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            if (afatfs_exfatWriteEntrySet(target) != AFATFS_OPERATION_SUCCESS) return;
        } else
#endif
        {
            uint8_t *buffer;
            if (afatfs_cacheSector(target->directoryEntryPos.sectorNumberPhysical, &buffer,
                AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0) != AFATFS_OPERATION_SUCCESS) return;
            uint8_t *entry = buffer + target->directoryEntryPos.entryIndex * 32;
            uint8_t name[11]; memcpy(name, entry, 11); memcpy(entry, r->scratch, 32); memcpy(entry, name, 11);
        }
        r->phase = 4;
    }
    if (r->phase == 4) {
        if (!afatfs_flushCache()) return;
        if (afatfs_saveDirectoryEntry(source, AFATFS_SAVE_DIRECTORY_DELETED) != AFATFS_OPERATION_SUCCESS) return;
        r->phase = 5; r->checkpoint = true;
    }
    afatfs_apiClose(r);
}

static bool afatfs_apiFileRequest(afatfsRequest_t *r, unsigned op, afatfsFilePtr_t file, afatfsResultCallback_t cb, void *user)
{
    if (!file || file->type != AFATFS_FILE_TYPE_NORMAL || afatfs_fileIsBusy(file)) return false;
    if (!afatfs_apiBegin(r, op, cb, user)) return false;
    r->file = file; file->error = AFATFS_ERROR_NONE; return true;
}
bool afatfs_open(afatfsRequest_t *r, const char *name, const char *mode, afatfsFilePtr_t *file, afatfsResultCallback_t cb, void *user)
{
    if (!name || !mode || !file || !afatfs_apiBegin(r, AFATFS_API_OPEN, cb, user)) return false;
    r->name = name; r->otherName = mode; r->output = file; *file = NULL; return true;
}
bool afatfs_close(afatfsRequest_t *r, afatfsFilePtr_t file, afatfsResultCallback_t cb, void *user)
{ return afatfs_apiFileRequest(r, AFATFS_API_CLOSE, file, cb, user); }
bool afatfs_sync(afatfsRequest_t *r, afatfsFilePtr_t file, afatfsResultCallback_t cb, void *user)
{ return afatfs_apiFileRequest(r, AFATFS_API_SYNC, file, cb, user); }
bool afatfs_truncate(afatfsRequest_t *r, afatfsFilePtr_t file, afatfsResultCallback_t cb, void *user)
{ return afatfs_apiFileRequest(r, AFATFS_API_TRUNCATE, file, cb, user); }
bool afatfs_read(afatfsRequest_t *r, afatfsFilePtr_t file, uint8_t *buffer, uint32_t count, afatfsResultCallback_t cb, void *user)
{
    if ((!buffer && count) || !afatfs_apiFileRequest(r, AFATFS_API_READ, file, cb, user)) return false;
    r->output = buffer; r->value[0] = count; return true;
}
bool afatfs_write(afatfsRequest_t *r, afatfsFilePtr_t file, const uint8_t *buffer, uint32_t count, afatfsResultCallback_t cb, void *user)
{
    if ((!buffer && count) || !afatfs_apiFileRequest(r, AFATFS_API_WRITE, file, cb, user)) return false;
    r->name = (const char *)buffer; r->value[0] = count; return true;
}
bool afatfs_puts(afatfsRequest_t *r, afatfsFilePtr_t file, const char *text, afatfsResultCallback_t cb, void *user)
{
    if (!text || strlen(text) > UINT32_MAX || !afatfs_apiFileRequest(r, AFATFS_API_PUTS, file, cb, user)) return false;
    r->name = text; r->value[0] = (uint32_t)strlen(text); return true;
}
bool afatfs_gets(afatfsRequest_t *r, afatfsFilePtr_t file, char *buffer, uint32_t capacity, afatfsResultCallback_t cb, void *user)
{
    if (!buffer || !capacity || !afatfs_apiFileRequest(r, AFATFS_API_GETS, file, cb, user)) return false;
    r->output = buffer; r->value[0] = capacity; buffer[0] = 0; return true;
}
bool afatfs_printf(afatfsRequest_t *r, afatfsFilePtr_t file, char *workspace, uint32_t capacity,
    afatfsResultCallback_t cb, void *user, const char *format, ...)
{
    if (!workspace || !capacity || !format || afatfs_request) return false;
    va_list args; va_start(args, format);
    int length = vsnprintf(workspace, capacity, format, args); va_end(args);
    if (length < 0 || (uint32_t)length >= capacity) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
    return afatfs_puts(r, file, workspace, cb, user);
}
bool afatfs_forward(afatfsRequest_t *r, afatfsFilePtr_t file, afatfsForwardCallback_t consumer,
    void *consumerUser, uint32_t count, afatfsResultCallback_t cb, void *user)
{
    if (!consumer || !afatfs_apiFileRequest(r, AFATFS_API_FORWARD, file, cb, user)) return false;
    r->consumer = consumer; r->consumerUser = consumerUser; r->value[0] = count; return true;
}
static void afatfs_apiText(afatfsRequest_t *r)
{
    afatfsFilePtr_t file = r->file;
    if (file->operation.active != AFATFS_FILE_OPERATION_NONE) return;
    if (r->operation == AFATFS_API_PUTS || r->operation == AFATFS_API_WRITE) {
        if (afatfs.readOnly || !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) || (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
            afatfs_apiFinish(AFATFS_ERROR_DENIED); return;
        }
        if (r->transferred < r->value[0])
            r->transferred += afatfs_fwrite(file, (const uint8_t *)r->name + r->transferred, r->value[0] - r->transferred);
        if (r->transferred == r->value[0] && file->operation.active == AFATFS_FILE_OPERATION_NONE) afatfs_apiFinish(AFATFS_ERROR_NONE);
        else if (afatfs_isFull()) afatfs_apiFinish(AFATFS_ERROR_NO_SPACE);
    } else if (!(file->mode & AFATFS_FILE_MODE_READ)) afatfs_apiFinish(AFATFS_ERROR_DENIED);
    else if (r->operation == AFATFS_API_READ) {
        uint8_t *buffer = (uint8_t *)r->output;
        if (r->value[0]) r->transferred += afatfs_fread(file, buffer + r->transferred, r->value[0] - r->transferred);
        if ((r->transferred == r->value[0] || afatfs_feof(file)) && file->operation.active == AFATFS_FILE_OPERATION_NONE)
            afatfs_apiFinish(AFATFS_ERROR_NONE);
    } else if (r->operation == AFATFS_API_GETS) {
        char *buffer = (char *)r->output;
        for (unsigned budget = 0; budget < 64; budget++) {
            if (r->transferred + 1 >= r->value[0] || afatfs_feof(file)) { afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
            uint8_t c;
            if (!afatfs_fread(file, &c, 1)) return;
            buffer[r->transferred++] = (char)c; buffer[r->transferred] = 0;
            if (c == '\n') { afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
        }
    } else {
        if (!r->value[1]) {
            if (r->transferred == r->value[0] || afatfs_feof(file)) { afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
            r->value[1] = afatfs_fread(file, r->scratch, MIN((uint32_t)sizeof(r->scratch), r->value[0] - r->transferred));
            r->value[2] = 0;
            if (!r->value[1]) return;
        }
        uint32_t accepted = r->consumer(r->scratch + r->value[2], r->value[1], r->consumerUser);
        if (accepted > r->value[1]) { afatfs_apiFinish(AFATFS_ERROR_INVALID_PARAMETER); return; }
        r->value[1] -= accepted; r->value[2] += accepted; r->transferred += accepted;
    }
}

bool afatfs_expand(afatfsRequest_t *r, afatfsFilePtr_t file, uint32_t size, bool allocate, afatfsResultCallback_t cb, void *user)
{
    if (!size || size > UINT32_MAX - afatfs_clusterSize() + 1 || !afatfs_apiFileRequest(r, AFATFS_API_EXPAND, file, cb, user)) return false;
    r->value[0] = size; r->value[1] = allocate; r->value[2] = 2;
    return true;
}
static void afatfs_apiExpand(afatfsRequest_t *r)
{
    afatfsFilePtr_t file = r->file;
    if (!r->phase) {
        for (unsigned n = 0; n < AFATFS_MAX_OPEN_FILES; n++)
            if (afatfs.openFiles[n].operation.active != AFATFS_FILE_OPERATION_NONE) return;
        afatfs_fileUpdateFilesize(file);
        if (afatfs.readOnly || file->logicalSize || file->firstCluster || !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))
            || (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY) || (file->mode & AFATFS_FILE_MODE_CONTIGUOUS)) {
            afatfs_apiFinish(AFATFS_ERROR_DENIED); return;
        }
        if (!afatfs_flush()) return;
        r->value[5] = (r->value[0] - 1) / afatfs_clusterSize() + 1; r->phase = 1;
    }
    if (r->phase == 1) {
        for (unsigned budget = 0; budget < 128; budget++) {
            if (r->value[2] - 2 >= afatfs.numClusters) { afatfs_apiFinish(AFATFS_ERROR_NO_SPACE); return; }
            bool free;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                bool occupied;
                if (afatfs_exfatBitmap(r->value[2], false, false, &occupied) != AFATFS_OPERATION_SUCCESS) return;
                free = !occupied;
            } else
#endif
            {
                uint32_t next;
                if (afatfs_FATGetNextCluster(0, r->value[2], &next) != AFATFS_OPERATION_SUCCESS) return;
                free = !next;
            }
            if (free) { if (!r->value[4]) r->value[3] = r->value[2]; r->value[4]++; }
            else r->value[4] = 0;
            r->value[2]++;
            if (r->value[4] == r->value[5]) {
                if (!r->value[1]) { afatfs.lastClusterAllocated = r->value[3]; afatfs_apiFinish(AFATFS_ERROR_NONE); return; }
                r->value[6] = 0; r->phase = 2; r->checkpoint = false; break;
            }
        }
        if (r->phase == 1) return;
    }
    if (r->phase == 2) {
        if (r->value[6] < r->value[5]) {
            uint32_t cluster = r->value[3] + r->value[6];
            if (afatfs_FATSetNextCluster(cluster, r->value[6] + 1 == r->value[5] ? 0xFFFFFFFF : cluster + 1) != AFATFS_OPERATION_SUCCESS) return;
            r->phase = 3;
        } else {
            file->firstCluster = r->value[3]; file->logicalSize = r->value[0];
            file->modified = true;
            file->physicalSize = r->value[5] * afatfs_clusterSize();
            file->cursorOffset = 0; file->cursorCluster = file->firstCluster; file->cursorPreviousCluster = 0;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) { file->exfatNoFatChain = true; file->validDataLength = file->committedDataLength = 0; }
#endif
            r->phase = 4;
        }
    }
    if (r->phase == 3) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs_exfatBitmap(r->value[3] + r->value[6], true, true, NULL) != AFATFS_OPERATION_SUCCESS) return;
#endif
        r->value[6]++; r->phase = 2; return;
    }
    if (r->phase == 4) {
        if (afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE) != AFATFS_OPERATION_SUCCESS) return;
        r->checkpoint = true; r->phase = 5;
    }
    if (r->phase == 5 && afatfs_flush()) { r->transferred = r->value[0]; afatfs_apiFinish(AFATFS_ERROR_NONE); }
}

static void afatfs_apiFormat(afatfsRequest_t *r);
static void afatfs_apiStep(afatfsRequest_t *r)
{
    if (r->operation >= AFATFS_API_MKFS) { afatfs_apiFormat(r); return; }
    if (r->waiting) return;
    if (r->operation == AFATFS_API_OPEN) {
        if (!r->phase) {
            if (!afatfs_allocateFileHandle()) { afatfs_apiFinish(AFATFS_ERROR_BUSY); return; }
            r->phase = 1; r->waiting = true;
            if (!afatfs_fopen(r->name, r->otherName, afatfs_apiOpened, r) && r->waiting) { r->waiting = false; r->result = AFATFS_ERROR_BUSY; }
            return;
        }
        *(afatfsFilePtr_t *)r->output = r->file; r->file = NULL;
        afatfs_apiFinish(r->result); return;
    }
    if (r->operation >= AFATFS_API_CLOSE && r->operation <= AFATFS_API_TRUNCATE) {
        if (!r->phase) {
            r->waiting = true;
            bool accepted = r->operation == AFATFS_API_CLOSE ? afatfs_fclose(r->file, afatfs_apiClosed, r)
                : r->operation == AFATFS_API_SYNC ? afatfs_fsync(r->file, afatfs_apiSynced, r)
                : afatfs_ftruncate(r->file, afatfs_apiSynced, r);
            if (!accepted) { r->waiting = false; afatfs_apiFinish((afatfsError_e)r->file->error); return; }
            r->phase = 1;
        }
        if (!r->waiting) afatfs_apiFinish(r->result);
        return;
    }
    if (r->phase >= 249) { afatfs_apiClose(r); return; }
    switch (r->operation) {
    case AFATFS_API_FREE: afatfs_apiFree(r); return;
    case AFATFS_API_GETLABEL:
    case AFATFS_API_SETLABEL: afatfs_apiLabel(r); return;
    case AFATFS_API_CWD: afatfs_apiCwd(r); return;
    case AFATFS_API_EXPAND: afatfs_apiExpand(r); return;
    case AFATFS_API_PUTS:
    case AFATFS_API_GETS:
    case AFATFS_API_READ:
    case AFATFS_API_WRITE:
    case AFATFS_API_FORWARD: afatfs_apiText(r); return;
    default: break;
    }
    if (!r->phase) {
        if (afatfs.cwdWalkMutex) return;
        uint8_t mode = AFATFS_FILE_MODE_READ;
        if (r->operation == AFATFS_API_UNLINK || r->operation == AFATFS_API_RMDIR) mode |= AFATFS_FILE_MODE_WRITE;
        if (r->operation == AFATFS_API_CHMOD || r->operation == AFATFS_API_UTIME || r->operation == AFATFS_API_RENAME) mode |= AFATFS_FILE_MODE_METADATA;
        if (afatfs_apiOpen(r, mode)) r->phase = 1;
        return;
    }
    if (!r->file || r->result) { afatfs_apiClose(r); return; }
    if (r->operation == AFATFS_API_STAT) {
        afatfsOperationStatus_e status = afatfs_fstat(r->file, (afatfsFileInfo_t *)r->output);
        if (status == AFATFS_OPERATION_IN_PROGRESS) return;
        if (status == AFATFS_OPERATION_FAILURE) r->result = afatfs.lastError;
        afatfs_apiClose(r);
    } else if (r->operation == AFATFS_API_OPENDIR || r->operation == AFATFS_API_FIND) {
        if (r->file->type != AFATFS_FILE_TYPE_DIRECTORY && r->file->type != AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) r->result = AFATFS_ERROR_DENIED;
        else {
            afatfsDirectory_t *dir = (afatfsDirectory_t *)r->output;
            if (r->phase == 1) {
                afatfs_apiDirectory(dir, r->file); dir->registered = true; dir->next = afatfs_directories; afatfs_directories = dir;
                dir->pattern = r->operation == AFATFS_API_FIND ? r->otherName : NULL;
                r->phase = 2;
            }
            if (r->operation == AFATFS_API_FIND) {
                afatfsOperationStatus_e status = afatfs_readdir(dir, (afatfsFileInfo_t *)r->consumerUser);
                if (status == AFATFS_OPERATION_IN_PROGRESS) return;
                if (status == AFATFS_OPERATION_FAILURE) { r->result = afatfs.lastError; afatfs_closedir(dir); }
            }
        }
        afatfs_apiClose(r);
    } else if (r->operation == AFATFS_API_CHMOD || r->operation == AFATFS_API_UTIME) afatfs_apiMetadata(r);
    else if (r->operation == AFATFS_API_RENAME) afatfs_apiRename(r);
    else {
        if (r->phase == 1) {
            bool dir = r->file->type != AFATFS_FILE_TYPE_NORMAL;
            if (dir != (r->operation == AFATFS_API_RMDIR)) { r->result = AFATFS_ERROR_DENIED; afatfs_apiClose(r); return; }
            r->waiting = true;
            if (!afatfs_funlink(r->file, afatfs_apiClosed, r)) { r->waiting = false; r->result = (afatfsError_e)r->file->error; afatfs_apiClose(r); return; }
            r->phase = 2;
        } else {
            r->result = (afatfsError_e)r->file->error;
            if (r->file->type == AFATFS_FILE_TYPE_NONE) r->file = NULL;
            afatfs_apiClose(r);
        }
    }
}
static void afatfs_apiPoll(void)
{
    afatfsRequest_t *r = afatfs_request;
    if (!r) return;
    r->driving = true; afatfs_apiStep(r);
    if (afatfs_request == r) r->driving = false;
}

#include "asyncfatfs_format.inc"
