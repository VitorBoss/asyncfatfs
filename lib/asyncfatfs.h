#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "fat_standard.h"

typedef struct afatfsFile_t *afatfsFilePtr_t;

typedef enum {
    AFATFS_FILESYSTEM_STATE_UNKNOWN,
    AFATFS_FILESYSTEM_STATE_FATAL,
    AFATFS_FILESYSTEM_STATE_INITIALIZATION,
    AFATFS_FILESYSTEM_STATE_READY,
    // Successfully deinitialized by afatfs_destroy(false); existing values are unchanged.
    AFATFS_FILESYSTEM_STATE_STOPPED,
    AFATFS_FILESYSTEM_STATE_STOPPING,
    AFATFS_FILESYSTEM_STATE_CLOSING,
    AFATFS_FILESYSTEM_STATE_REMOVED,
} afatfsFilesystemState_e;

typedef enum {
    AFATFS_OPERATION_IN_PROGRESS,
    AFATFS_OPERATION_SUCCESS,
    AFATFS_OPERATION_FAILURE,
} afatfsOperationStatus_e;

typedef enum {
    AFATFS_ERROR_NONE = 0,
    AFATFS_ERROR_GENERIC = 1,
    AFATFS_ERROR_BAD_MBR = 2,
    AFATFS_ERROR_BAD_FILESYSTEM_HEADER = 3,
    AFATFS_ERROR_RECOVERY_REQUIRED = 4,
    AFATFS_ERROR_BACKEND_IO = 5,
    AFATFS_ERROR_MEDIA_REMOVED = 6,
    AFATFS_ERROR_BUSY,
    AFATFS_ERROR_INVALID_OBJECT,
    AFATFS_ERROR_INVALID_NAME,
    AFATFS_ERROR_NOT_FOUND,
    AFATFS_ERROR_EXISTS,
    AFATFS_ERROR_DENIED,
    AFATFS_ERROR_NOT_EMPTY,
    AFATFS_ERROR_NO_SPACE,
    AFATFS_ERROR_UNSUPPORTED,
    AFATFS_ERROR_INVALID_PARAMETER,
    AFATFS_ERROR_CANCELED
} afatfsError_e;

typedef struct afatfsDirEntryPointer_t {
    uint32_t sectorNumberPhysical;
    int16_t entryIndex;
} afatfsDirEntryPointer_t;

typedef afatfsDirEntryPointer_t afatfsFinder_t;

typedef enum {
    AFATFS_SEEK_SET,
    AFATFS_SEEK_CUR,
    AFATFS_SEEK_END,
} afatfsSeek_e;

typedef void (*afatfsFileCallback_t)(afatfsFilePtr_t file, void *user);
typedef void (*afatfsCallback_t)(void *user);

bool afatfs_fopen(const char *fileName, const char *mode, afatfsFileCallback_t complete, void *user);
bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user);
bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback, void *user);
bool afatfs_fsync(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user);
bool afatfs_funlink(afatfsFilePtr_t file, afatfsCallback_t callback, void *user);

bool afatfs_feof(afatfsFilePtr_t file);
// Returns 1 when accepted, -1 on rejection; callers may retry BUSY.
int afatfs_fputc(afatfsFilePtr_t file, uint8_t character);
uint32_t afatfs_fwrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t len);
uint32_t afatfs_fread(afatfsFilePtr_t file, uint8_t *buffer, uint32_t len);
void afatfs_fprefetch(afatfsFilePtr_t file);
// Pure global status queries. Idle does not imply a cleanly unmounted volume.
bool afatfs_isIdle(void);
// True only after successful afatfs_destroy(false), until the next init/reset.
bool afatfs_canPowerOff(void);
afatfsOperationStatus_e afatfs_fseek(
    afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence, afatfsFileCallback_t callback, void *user);
bool afatfs_ftell(afatfsFilePtr_t file, uint32_t *position);
bool afatfs_fGetSize(afatfsFilePtr_t file, uint32_t *size);
bool afatfs_fGetFileInfo(afatfsFilePtr_t file, fatDirectoryEntry_t **entry);
bool afatfs_fGetExFatFileInfo(afatfsFilePtr_t file, const exFatFileEntry_t **entry, const exFatStreamEntry_t **stream);

bool afatfs_mkdir(const char *fileName, afatfsFileCallback_t complete, void *user);
bool afatfs_chdir(afatfsFilePtr_t dirHandle);

void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder);
afatfsOperationStatus_e afatfs_findNext(
    afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry);
void afatfs_findLast(afatfsFilePtr_t directory);

bool afatfs_flush();
void afatfs_init();
// Call on a detected media-removal/replacement event, serialized with other library calls.
// Cancels backend I/O, invalidates handles and prevents automatic reuse of the old session.
// Reinitialize the backend and call afatfs_init() after reinsertion.
void afatfs_notifyMediaRemoved(void);
bool afatfs_destroy(bool dirty);
void afatfs_poll();

// Reusable cache bytes plus writable tails owned by idle public file handles.
// This aggregate is not a guarantee for a single fwrite; no I/O is performed.
uint32_t afatfs_getFreeBufferSpace();
// Shared reusable cache bytes plus only this file's writable sector tail.
// Returns zero for NULL, closed, non-writable or busy handles, or a non-writable filesystem.
uint32_t afatfs_getFileFreeBufferSpace(afatfsFilePtr_t file);
uint32_t afatfs_getContiguousFreeSpace();
bool afatfs_isFull();
bool afatfs_isReadOnly(void);

afatfsFilesystemState_e afatfs_getFilesystemState();
afatfsError_e afatfs_getLastError();
afatfsError_e afatfs_ferror(afatfsFilePtr_t file);
void afatfs_clearerr(afatfsFilePtr_t file);

// New metadata API uses FAT packed date/time values, independent of on-disk entry format.
typedef struct afatfsFileInfo_t {
    char name[FAT_LONG_FILENAME_LENGTH];
    uint32_t size;
    uint32_t validSize;
    uint32_t allocatedSize;
    uint32_t firstCluster;
    uint32_t created;
    uint32_t modified;
    uint32_t accessed;
    uint8_t attributes;
    bool contiguous;
} afatfsFileInfo_t;

typedef struct afatfsDirectoryCursor_t {
    uint32_t firstCluster;
    uint32_t cluster;
    uint32_t clusterIndex;
    uint32_t offset;
    uint32_t clusters;
    uint8_t contiguous;
    uint8_t root16;
    uint8_t ended;
} afatfsDirectoryCursor_t;

typedef struct afatfsDirectory_t {
    // Keep this object alive until closedir; fields below are private.
    afatfsDirectoryCursor_t cursor;
    struct afatfsDirectory_t *next;
    const char *pattern;
    afatfsFileInfo_t *output;
    uint32_t generation;
    uint16_t checksum;
    uint16_t expectedChecksum;
    uint16_t nameHash;
    uint8_t remaining;
    uint8_t entryIndex;
    uint8_t nameLength;
    uint8_t nameWritten;
    // Runtime flags use C99 bool bit-fields; storage order is compiler-specific.
    bool registered : 1;
    bool reading : 1;
} afatfsDirectory_t;

typedef void (*afatfsResultCallback_t)(afatfsError_e result, void *user);
typedef uint32_t (*afatfsForwardCallback_t)(const uint8_t *data, uint32_t size, void *user);

#ifndef AFATFS_REQUEST_SCRATCH_SIZE
#define AFATFS_REQUEST_SCRATCH_SIZE 256
#endif
#if AFATFS_REQUEST_SCRATCH_SIZE < 32
#error "AFATFS_REQUEST_SCRATCH_SIZE must be at least 32 bytes."
#endif
// Caller-owned workspace, reusable after complete/callback. No heap or per-file workspace is added.
// Names, buffers and output objects must stay alive until completion. Private fields must not be modified.
typedef struct afatfsRequest_t {
    afatfsError_e result;
    uint32_t transferred;
    afatfsResultCallback_t callback;
    void *user;
    const char *name;
    const char *otherName;
    void *output;
    afatfsFilePtr_t file;
    afatfsFilePtr_t other;
    afatfsForwardCallback_t consumer;
    void *consumerUser;
    uint32_t value[16];
    afatfsFileInfo_t info;
    afatfsDirectory_t directory;
    // Keep scratch after word-aligned members: getcwd stores path frames here.
    uint8_t scratch[AFATFS_REQUEST_SCRATCH_SIZE];
    uint8_t operation;
    uint8_t phase;
    // Bit-fields are read/written by value; their addresses cannot be taken.
    bool complete : 1;
    bool waiting : 1;
    bool checkpoint : 1;
    bool driving : 1;
} afatfsRequest_t;

// Result-bearing alternatives to the legacy file callbacks. Read/write report accepted bytes in transferred.
bool afatfs_open(afatfsRequest_t *request, const char *name, const char *mode, afatfsFilePtr_t *file,
    afatfsResultCallback_t callback, void *user);
bool afatfs_close(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user);
bool afatfs_sync(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user);
bool afatfs_truncate(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user);
bool afatfs_read(afatfsRequest_t *request, afatfsFilePtr_t file, uint8_t *buffer, uint32_t size,
    afatfsResultCallback_t callback, void *user);
bool afatfs_write(afatfsRequest_t *request, afatfsFilePtr_t file, const uint8_t *buffer, uint32_t size,
    afatfsResultCallback_t callback, void *user);

typedef struct afatfsVolumeInfo_t {
    char label[12];
    uint32_t serial;
    uint32_t clusterSize;
    uint32_t totalClusters;
    uint32_t freeClusters;
} afatfsVolumeInfo_t;

afatfsOperationStatus_e afatfs_fstat(afatfsFilePtr_t file, afatfsFileInfo_t *info);
bool afatfs_stat(
    afatfsRequest_t *request, const char *name, afatfsFileInfo_t *info, afatfsResultCallback_t callback, void *user);
bool afatfs_opendir(afatfsRequest_t *request, const char *name, afatfsDirectory_t *directory,
    afatfsResultCallback_t callback, void *user);
afatfsOperationStatus_e afatfs_readdir(afatfsDirectory_t *directory, afatfsFileInfo_t *info);
afatfsOperationStatus_e afatfs_closedir(afatfsDirectory_t *directory);
afatfsOperationStatus_e afatfs_rewinddir(afatfsDirectory_t *directory);
bool afatfs_findfirst(afatfsRequest_t *request, afatfsDirectory_t *directory, afatfsFileInfo_t *info, const char *name,
    const char *pattern, afatfsResultCallback_t callback, void *user);
afatfsOperationStatus_e afatfs_findnext(afatfsDirectory_t *directory, afatfsFileInfo_t *info);
bool afatfs_rename(
    afatfsRequest_t *request, const char *oldName, const char *newName, afatfsResultCallback_t callback, void *user);
bool afatfs_unlink(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user);
bool afatfs_rmdir(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user);
bool afatfs_chmod(afatfsRequest_t *request, const char *name, uint8_t attributes, uint8_t mask,
    afatfsResultCallback_t callback, void *user);
bool afatfs_utime(
    afatfsRequest_t *request, const char *name, uint32_t modified, afatfsResultCallback_t callback, void *user);
bool afatfs_getfree(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user);
bool afatfs_getlabel(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user);
bool afatfs_setlabel(afatfsRequest_t *request, const char *label, afatfsResultCallback_t callback, void *user);
bool afatfs_getcwd(
    afatfsRequest_t *request, char *buffer, uint32_t capacity, afatfsResultCallback_t callback, void *user);
afatfsOperationStatus_e afatfs_chdrive(unsigned drive);  // Only logical drive 0 exists.
afatfsOperationStatus_e afatfs_setcp(unsigned codePage); // ASCII: 20127.
bool afatfs_expand(afatfsRequest_t *request, afatfsFilePtr_t file, uint32_t size, bool allocate,
    afatfsResultCallback_t callback, void *user);
bool afatfs_gets(afatfsRequest_t *request, afatfsFilePtr_t file, char *buffer, uint32_t capacity,
    afatfsResultCallback_t callback, void *user);
bool afatfs_puts(
    afatfsRequest_t *request, afatfsFilePtr_t file, const char *text, afatfsResultCallback_t callback, void *user);
bool afatfs_printf(afatfsRequest_t *request, afatfsFilePtr_t file, char *workspace, uint32_t capacity,
    afatfsResultCallback_t callback, void *user, const char *format, ...);
bool afatfs_forward(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsForwardCallback_t consumer,
    void *consumerUser, uint32_t count, afatfsResultCallback_t callback, void *user);
afatfsOperationStatus_e afatfs_rewind(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user);

typedef struct afatfsFormatOptions_t {
    fatFilesystemType_e type;
    uint32_t startSector;
    uint32_t sectorCount;
    uint32_t sectorsPerCluster; // Power of two; 0 chooses automatically.
    uint32_t serial;
} afatfsFormatOptions_t;
typedef struct afatfsPartitionOptions_t {
    uint32_t sectorCount;
    uint32_t start[4];
    uint32_t length[4];
    uint8_t type[4]; // MBR types; GPT partitions use Microsoft Basic Data.
    uint8_t diskGuid[16];
    bool gpt;
} afatfsPartitionOptions_t;
// Destructive operations require an already unmounted filesystem; only explicit ranges are written.
bool afatfs_mkfs(
    afatfsRequest_t *request, const afatfsFormatOptions_t *options, afatfsResultCallback_t callback, void *user);
bool afatfs_fdisk(
    afatfsRequest_t *request, const afatfsPartitionOptions_t *options, afatfsResultCallback_t callback, void *user);
