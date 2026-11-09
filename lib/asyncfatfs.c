/**
 * This is a FAT16/FAT32/exFAT filesystem for SD cards which uses asynchronous operations: The caller need never wait
 * for the SD card to be ready.
 *
 * On top of the regular FAT32 concepts, we add the idea of a "super cluster". Given one FAT sector, a super cluster is
 * the series of clusters which corresponds to all of the cluster entries in that FAT sector. If files are allocated
 * on super-cluster boundaries, they will have FAT sectors which are dedicated to them and independent of all other
 * files.
 *
 * We can pre-allocate a "freefile" which is a file on disk made up of contiguous superclusters. Then when we want
 * to allocate a file on disk, we can carve it out of the freefile, and know that the clusters will be contiguous
 * without needing to read the FAT at all (the freefile's FAT is completely determined from its start cluster and file
 * size, which we get from the directory entry). This allows for extremely fast append-only logging.
 */

// Define this in your build system to use a user-supplied file for compile-time settings.
// Useful if you want to keep AsyncFatFS as a git submodule without modifying it.
#ifndef AFATFS_EXTERNAL_CONFIG

// === Compile-time settings ===
// For information about these settings, see the documentation in doc/Using_asyncfatfs.md
//#define AFATFS_DEBUG
//#define AFATFS_DEBUG_VERBOSE
//#define AFATFS_USE_INTROSPECTIVE_LOGGING
#define AFATFS_NUM_CACHE_SECTORS 4
#define AFATFS_MAX_OPEN_FILES 3
#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2
//#define AFATFS_USE_FREEFILE
//#define ASYNCFS_ENABLE_EXFAT
//#define ASYNCFS_ENABLE_LBA64 // Reserved: requires widening the backend and public APIs.
#define ASYNCFS_ENABLE_LFN
#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100
#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"
#define AFATFS_INTROSPEC_LOG_FILENAME "ASYNCFAT.LOG"
// === End of compile-time settings ===

#else
#include "afatfs_config.h"
#endif

#ifdef ASYNCFS_ENABLE_LBA64
#error "ASYNCFS_ENABLE_LBA64 requires a 64-bit storage backend and finder API; the current interfaces use uint32_t."
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef AFATFS_DEBUG
#include <signal.h>
#include <stdio.h>
#endif

#include "asyncfatfs.h"
#include "builddefs.h"

#include "fat_standard.h"
#include "sdcard.h"

#ifdef _MSC_VER
#define AFATFS_WEAK
#define AFATFS_ALIGNED __declspec(align(4))
#else
#define AFATFS_WEAK __attribute__((weak))
#define AFATFS_ALIGNED __attribute__((aligned(4)))
#endif

#define AFATFS_EXFAT_MAX_ENTRIES 19 // Primary, stream and 17 name entries (255 UTF-16 units)

#ifdef AFATFS_DEBUG
#define ONLY_EXPOSE_FOR_TESTING
#else
#define ONLY_EXPOSE_FOR_TESTING static
#endif

// FAT filesystems are allowed to differ from these parameters, but we choose not to support those weird filesystems:
#define AFATFS_SECTOR_SIZE 512
#define AFATFS_NUM_FATS 2

#define AFATFS_DEFAULT_FILE_DATE FAT_MAKE_DATE(BUILD_YEAR, BUILD_MONTH, BUILD_DAY)
#define AFATFS_DEFAULT_FILE_TIME FAT_MAKE_TIME(BUILD_HOUR, BUILD_MIN, BUILD_SEC)

#define AFATFS_FILES_PER_DIRECTORY_SECTOR (AFATFS_SECTOR_SIZE / sizeof(fatDirectoryEntry_t))

#define AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR (AFATFS_SECTOR_SIZE / sizeof(uint32_t))
#define AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR (AFATFS_SECTOR_SIZE / sizeof(uint16_t))

// We will read from the file
#define AFATFS_FILE_MODE_READ 1
// We will write to the file
#define AFATFS_FILE_MODE_WRITE 2
// We will append to the file, may not be combined with the write flag
#define AFATFS_FILE_MODE_APPEND 4
// File will occupy a series of superclusters (only valid for creating new files):
#define AFATFS_FILE_MODE_CONTIGUOUS 8
// File should be created if it doesn't exist:
#define AFATFS_FILE_MODE_CREATE 16
// The file's directory entry should be locked in cache so we can read it with no latency:
#define AFATFS_FILE_MODE_RETAIN_DIRECTORY 32
#define AFATFS_FILE_MODE_EXCLUSIVE_CREATE 64
#define AFATFS_FILE_MODE_METADATA 128

// Open the cache sector for read access (it will be read from disk)
#define AFATFS_CACHE_READ 1
// Open the cache sector for write access (it will be marked dirty)
#define AFATFS_CACHE_WRITE 2
// Lock this sector to prevent its state from transitioning (prevent flushes to disk)
#define AFATFS_CACHE_LOCK 4
// Discard this sector in preference to other sectors when it is in the in-sync state
#define AFATFS_CACHE_DISCARDABLE 8
// Increase the retain counter of the cache sector to prevent it from being discarded when in the in-sync state
#define AFATFS_CACHE_RETAIN 16
#define AFATFS_CACHE_METADATA 32

#define MIN(firstValue, secondValue) ((firstValue) < (secondValue) ? (firstValue) : (secondValue))
#define MAX(firstValue, secondValue) ((firstValue) > (secondValue) ? (firstValue) : (secondValue))

#ifdef ASYNCFS_ENABLE_LBA64
typedef uint64_t LBA_t;
#else
typedef uint32_t LBA_t;
#endif

#ifdef ASYNCFS_ENABLE_EXFAT
#include "asyncfatfs_journal.h"
#endif

typedef enum {
    AFATFS_SAVE_DIRECTORY_NORMAL,
    AFATFS_SAVE_DIRECTORY_FOR_CLOSE,
    AFATFS_SAVE_DIRECTORY_DELETED,
} afatfsSaveDirectoryEntryMode_e;

typedef enum {
    AFATFS_CACHE_STATE_EMPTY,
    AFATFS_CACHE_STATE_IN_SYNC,
    AFATFS_CACHE_STATE_READING,
    AFATFS_CACHE_STATE_WRITING,
    AFATFS_CACHE_STATE_DIRTY
} afatfsCacheBlockState_e;

typedef enum {
    AFATFS_FILE_TYPE_NONE,
    AFATFS_FILE_TYPE_NORMAL,
    AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY,
    AFATFS_FILE_TYPE_DIRECTORY
} afatfsFileType_e;

typedef enum {
    CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR,
    CLUSTER_SEARCH_FREE,
    CLUSTER_SEARCH_OCCUPIED,
} afatfsClusterSearchCondition_e;

enum {
    AFATFS_CREATEFILE_PHASE_INITIAL = 0,
    AFATFS_CREATEFILE_PHASE_FIND_FILE,
    AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE,
    AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB,
    AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY,
    AFATFS_CREATEFILE_PHASE_LFN_FLUSH,
    AFATFS_CREATEFILE_PHASE_LFN_PUBLISH,
    AFATFS_CREATEFILE_PHASE_SUCCESS,
    AFATFS_CREATEFILE_PHASE_FAILURE,
};

typedef enum {
    AFATFS_FIND_CLUSTER_IN_PROGRESS,
    AFATFS_FIND_CLUSTER_FOUND,
    AFATFS_FIND_CLUSTER_FATAL,
    AFATFS_FIND_CLUSTER_NOT_FOUND,
} afatfsFindClusterStatus_e;

struct afatfsFileOperation_t;

typedef union afatfsFATSector_t {
    uint8_t *bytes;
    uint16_t *fat16;
    uint32_t *fat32;
} afatfsFATSector_t;

typedef struct afatfsCacheBlockDescriptor_t {
    afatfsCacheBlockState_e state;
#ifdef ASYNCFS_ENABLE_EXFAT
    uint32_t journalEpoch;
#endif

    /*
     * The state of this block must not transition (do not flush to disk, do not discard). This is useful for a sector
     * which is currently being written to by the application (so flushing it would be a waste of time).
     *
     * This is a binary state rather than a counter because we assume that only one party will be responsible for and
     * so consider locking a given sector.
     */
    unsigned locked : 1;

    /*
     * A counter for how many parties want this sector to be retained in memory (not discarded). If this value is
     * non-zero, the sector may be flushed to disk if dirty but must remain in the cache. This is useful if we require
     * a directory sector to be cached in order to meet our response time requirements.
     */
    unsigned retainCount : 6;

    /*
     * If this block is in the In Sync state, it should be discarded from the cache in preference to other blocks.
     * This is useful for data that we don't expect to read again, e.g. data written to an append-only file. This hint
     * is overridden by the locked and retainCount flags.
     */
    unsigned discardable : 1;

    /*
     * The physical sector index on disk that this cached block corresponds to
     */
    LBA_t sectorIndex;

    // We use an increasing timestamp to identify cache access times.

    // This is the timestamp that this sector was first marked dirty at (so we can flush sectors in write-order).
    uint32_t writeTimestamp;

    // This is the last time the sector was accessed
    uint32_t accessTimestamp;

    /* This is set to non-zero when we expect to write a consecutive series of this many blocks (including this block),
     * so we will tell the SD-card to pre-erase those blocks.
     *
     * This counter only needs to be set on the first block of a consecutive write (though setting it, appropriately
     * decreased, on the subsequent blocks won't hurt).
     */
    uint16_t consecutiveEraseBlockCount;
} afatfsCacheBlockDescriptor_t;

typedef enum {
    AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN,
    AFATFS_FAT_PATTERN_TERMINATED_CHAIN,
    AFATFS_FAT_PATTERN_FREE
} afatfsFATPattern_e;

typedef enum {
    AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE,
    AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE
} afatfsFreeSpaceSearchPhase_e;

typedef struct afatfsFreeSpaceSearch_t {
    LBA_t candidateStart;
    LBA_t candidateEnd;
    LBA_t bestGapStart;
    uint32_t bestGapLength;
    afatfsFreeSpaceSearchPhase_e phase;
} afatfsFreeSpaceSearch_t;

typedef struct afatfsFreeSpaceFAT_t {
    LBA_t startCluster;
    LBA_t endCluster;
} afatfsFreeSpaceFAT_t;

typedef struct afatfsCreateFile_t {
    afatfsFileCallback_t callback;
    void *user;
#ifdef ASYNCFS_ENABLE_LFN
    // Alias search and free-slot reservation never run simultaneously.
    union {
        uint32_t aliasNumber;
        uint32_t orphanSector;
    } location;
    afatfsLongNameState_t lfn;
#endif

#if !defined(ASYNCFS_ENABLE_LFN) && !defined(ASYNCFS_ENABLE_EXFAT)
    uint8_t fileName[FAT_FILENAME_LENGTH];
#else
    uint8_t fileName[FAT_LONG_FILENAME_LENGTH];
    uint8_t extraEntryCount; // Reuses the former unused FAT-LFN short-name workspace.
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
    uint8_t entryCount;
    uint8_t entriesRead;
#endif
    uint8_t phase;
#ifdef ASYNCFS_ENABLE_LFN
    uint8_t shortName[FAT_FILENAME_LENGTH];
    uint8_t freeEntries;
    uint8_t writeIndex;
    uint8_t orphanIndex;
#endif
#if defined(ASYNCFS_ENABLE_EXFAT) || defined(ASYNCFS_ENABLE_LFN)
    bool directory : 1;
    bool directoryEnd : 1;
#else
    bool directory;
#endif
} afatfsCreateFile_t;

typedef struct afatfsSeek_t {
    afatfsFileCallback_t callback;
    void *user;

    uint32_t seekOffset;
} afatfsSeek_t;

typedef enum {
    AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT = 0,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
} afatfsAppendSuperclusterPhase_e;

typedef struct afatfsAppendSupercluster_t {
    LBA_t previousCluster;
    LBA_t fatRewriteStartCluster;
    LBA_t fatRewriteEndCluster;
    afatfsAppendSuperclusterPhase_e phase;
} afatfsAppendSupercluster_t;

typedef enum {
    AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL = 0,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE = 0,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP_DURABLE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE,
} afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeCluster_t {
    LBA_t previousCluster;
    LBA_t searchCluster;
    uint32_t convertCluster;
    uint32_t zeroSector;
    afatfsAppendFreeClusterPhase_e phase;
    bool wrapped;
} afatfsAppendFreeCluster_t;

typedef enum {
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL = 0,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER = 0,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE
} afatfsExtendSubdirectoryPhase_e;

typedef struct afatfsExtendSubdirectory_t {
    // We need to call this as a sub-operation so we have it as our first member to be compatible with its memory layout:
    afatfsAppendFreeCluster_t appendFreeCluster;
    afatfsFileCallback_t callback;
    void *user;
    LBA_t parentDirectoryCluster;
    afatfsExtendSubdirectoryPhase_e phase;
} afatfsExtendSubdirectory_t;

typedef enum {
    AFATFS_TRUNCATE_FILE_INITIAL = 0,
    AFATFS_TRUNCATE_FILE_FIND_TAIL = 0,
    AFATFS_TRUNCATE_FILE_FLUSH_DATA,
    AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY,
    AFATFS_TRUNCATE_FILE_DIRECTORY_DURABLE,
    AFATFS_TRUNCATE_FILE_TERMINATE_CHAIN,
    AFATFS_TRUNCATE_FILE_TAIL_DURABLE,
    AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL,
#ifdef AFATFS_USE_FREEFILE
    AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS,
    AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE,
#endif
    AFATFS_TRUNCATE_FILE_SUCCESS,
    AFATFS_TRUNCATE_FILE_CHECK_DIRECTORY,
} afatfsTruncateFilePhase_e;

typedef struct afatfsTruncateFile_t {
    afatfsFileCallback_t callback;
    void *user;

    LBA_t startCluster;   // First cluster to erase
    LBA_t currentCluster; // Used to mark progress
    LBA_t endCluster; // Optional, for contiguous files set to 1 past the end cluster of the file, otherwise set to 0
    uint32_t nextCluster;
    uint32_t targetSize;
    uint32_t keepCluster;
    uint32_t previousCluster;
    uint32_t clustersToKeep;
    afatfsTruncateFilePhase_e phase;
    bool nextClusterValid : 1;
    bool fatCleared : 1;
    bool linear : 1;
} afatfsTruncateFile_t;

typedef enum {
    AFATFS_DELETE_FILE_DELETE_DIRECTORY_ENTRY,
    AFATFS_DELETE_FILE_DEALLOCATE_CLUSTERS,
} afatfsDeleteFilePhase_e;

typedef struct afatfsDeleteFile_t {
    afatfsTruncateFile_t truncateFile;
    afatfsCallback_t callback;
    void *user;
} afatfsUnlinkFile_t;

typedef struct afatfsCloseFile_t {
    afatfsCallback_t callback;
    void *user;

    uint8_t phase;
} afatfsCloseFile_t;

typedef struct afatfsSyncFile_t {
    afatfsFileCallback_t callback;
    void *user;

    uint8_t phase;
} afatfsSyncFile_t;

typedef enum {
    AFATFS_FILE_OPERATION_NONE,
    AFATFS_FILE_OPERATION_CREATE_FILE,
    AFATFS_FILE_OPERATION_SEEK, // Seek the file's cursorCluster forwards by seekOffset bytes
    AFATFS_FILE_OPERATION_CLOSE,
    AFATFS_FILE_OPERATION_SYNC,
    AFATFS_FILE_OPERATION_TRUNCATE,
    AFATFS_FILE_OPERATION_UNLINK,
#ifdef AFATFS_USE_FREEFILE
    AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER,
    AFATFS_FILE_OPERATION_LOCKED,
#endif
    AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER,
    AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY,
} afatfsFileOperation_e;

typedef struct afatfsFileOperation_t {
    afatfsFileOperation_e active;
    union {
        afatfsCreateFile_t createFile;
        afatfsSeek_t seek;
        afatfsAppendSupercluster_t appendSupercluster;
        afatfsAppendFreeCluster_t appendFreeCluster;
        afatfsExtendSubdirectory_t extendSubdirectory;
        afatfsUnlinkFile_t unlinkFile;
        afatfsTruncateFile_t truncateFile;
        afatfsCloseFile_t closeFile;
        afatfsSyncFile_t syncFile;
    } state;
} afatfsFileOperation_t;

typedef struct afatfsFile_t {
    // State for a queued operation on the file
    struct afatfsFileOperation_t operation;
#ifdef ASYNCFS_ENABLE_EXFAT
    afatfsCallback_t closeAfterTruncateCallback;
    void *closeAfterTruncateUser;
    afatfsDirEntryPointer_t exfatPositions[AFATFS_EXFAT_MAX_ENTRIES];
#endif
    // The position of our directory entry on the disk (so we can update it without consulting a parent directory file)
    afatfsDirEntryPointer_t directoryEntryPos;
#ifdef ASYNCFS_ENABLE_LFN
    afatfsFileInfo_t *lfnOutput;
    uint32_t lfnCluster;
    uint32_t lfnOffset;
#endif
    // The byte offset of the cursor within the file
    uint32_t cursorOffset;
    /* The file size in bytes as seen by users of the filesystem (the exact length of the file they've written).
     *
     * This is only used by users of the filesystem, not us, so it only needs to be up to date for fseek() (to clip
     * seeks to the EOF), fread(), feof(), and fclose() (which writes the logicalSize to the directory).
     *
     * It becomes out of date when we fwrite() to extend the length of the file. In this situation, feof() is properly
     * true, so we don't have to update the logicalSize for fread() or feof() to get the correct result. We only need
     * to update it when we seek backwards (so we don't forget the logical EOF position), or fclose().
     */
    uint32_t logicalSize;
    /* The allocated size in bytes based on how many clusters have been assigned to the file. Always a multiple of
     * the cluster size.
     *
     * This is an underestimate for existing files, because we don't bother to check precisely how long the chain is
     * at the time the file is opened (it might be longer than needed to contain the logical size), but assuming the
     * filesystem metadata is correct, it should always be at least as many clusters as needed to contain logicalSize.
     *
     * Since this is an estimate, we only use it to exaggerate the filesize in the directory entry of a file that is
     * currently being written (so that the final cluster of the file will be entirely readable if power is lost before
     * we can could update the directory entry with a new logicalSize).
     */
    uint32_t physicalSize;
#ifdef ASYNCFS_ENABLE_EXFAT
    uint32_t validDataLength;
    uint32_t committedDataLength;
    uint32_t zeroCursorCluster;
    uint32_t zeroCursorIndex;
#endif
    /*
     * The cluster that the file pointer is currently within. When seeking to the end of the file, this will be
     * set to zero.
     */
    LBA_t cursorCluster;
    /*
     * The cluster before the one the file pointer is inside. This is set to zero when at the start of the file.
     */
    LBA_t cursorPreviousCluster;
    // The first cluster number of the file, or 0 if this file is empty
    LBA_t firstCluster;
    afatfsFileType_e type;
#ifdef ASYNCFS_ENABLE_EXFAT
    // Keep entry sets outside the operation union: close, append and truncate all update them.
    uint8_t exfatEntries[AFATFS_EXFAT_MAX_ENTRIES * FAT_DIRECTORY_ENTRY_SIZE];
#endif
    uint8_t mode;   // A combination of AFATFS_FILE_MODE_* flags
    uint8_t attrib; // Combination of FAT_FILE_ATTRIBUTE_* flags for the directory entry of this file
    uint8_t error;  // Per-handle result, independent of another file's operations.
    /* We hold on to one sector entry in the cache and remember its index here. The cache is invalidated when we
     * seek across a sector boundary. This allows fwrite() to complete faster because it doesn't need to check the
     * cache on every call.
     */
    int8_t writeLockedCacheIndex;
    // Ditto for fread():
    int8_t readRetainCacheIndex;
#ifdef ASYNCFS_ENABLE_LFN
    uint8_t lfnEntryCount;
    uint8_t lfnProgress;
    uint8_t lfnDeleteIndex;
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
    uint8_t exfatEntryCount;
    uint8_t exfatWriteIndex;
    bool modified : 1;
#ifdef ASYNCFS_ENABLE_LFN
    bool lfnAscii : 1;
#endif
    bool exfatNoFatChain : 1;
    bool dataDirty : 1;
#else
#ifdef ASYNCFS_ENABLE_LFN
    bool modified : 1;
    bool lfnAscii : 1;
#else
    bool modified;
#endif
#endif
} afatfsFile_t;

typedef enum {
    AFATFS_INITIALIZATION_READ_MBR,
#ifdef ASYNCFS_ENABLE_EXFAT
    AFATFS_INITIALIZATION_READ_GPT_HEADER,
    AFATFS_INITIALIZATION_READ_GPT_ARRAY,
    AFATFS_INITIALIZATION_READ_GPT_ENTRY,
    AFATFS_INITIALIZATION_READ_GPT_VOLUME,
    AFATFS_INITIALIZATION_CHECK_GPT_OVERLAP,
#endif
    AFATFS_INITIALIZATION_READ_VOLUME_ID,
    AFATFS_INITIALIZATION_READ_ROOT,

#ifdef AFATFS_USE_FREEFILE
    AFATFS_INITIALIZATION_FREEFILE_CREATE,
    AFATFS_INITIALIZATION_FREEFILE_CREATING,
    AFATFS_INITIALIZATION_FREEFILE_FAT_SEARCH,
    AFATFS_INITIALIZATION_FREEFILE_UPDATE_FAT,
    AFATFS_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY,
    AFATFS_INITIALIZATION_FREEFILE_LAST = AFATFS_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY,
#endif

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    AFATFS_INITIALIZATION_INTROSPEC_LOG_CREATE,
    AFATFS_INITIALIZATION_INTROSPEC_LOG_CREATING,
#endif

    AFATFS_INITIALIZATION_DONE,
    AFATFS_SHUTDOWN_FSINFO_PRIMARY,
    AFATFS_SHUTDOWN_FSINFO_BACKUP,
    AFATFS_SHUTDOWN_FSINFO_FLUSH
} afatfsInitializationPhase_e;

#ifdef ASYNCFS_ENABLE_EXFAT
// This scratch state overlaps upcaseAscii, which is populated only after partition discovery.
typedef struct afatfsGpt_t {
    uint32_t headerSector;
    uint32_t lastSector;
    uint32_t firstUsable;
    uint32_t lastUsable;
    uint32_t tableSector;
    uint32_t tableBytes;
    uint32_t entrySize;
    uint32_t tableChecksum;
    uint32_t offset;
    uint32_t crc;
    uint32_t selectedOffset;
    bool readOnly : 1;
    bool backupKnown : 1;
} afatfsGpt_t;
// Internal partition marker; never interpreted as an on-disk MBR partition type.
#define AFATFS_PARTITION_EXFAT_READ_ONLY (MBR_PARTITION_TYPE_EXFAT | 0x80)
#endif

typedef struct afatfs_t {
    AFATFS_ALIGNED uint8_t cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
#ifdef ASYNCFS_ENABLE_EXFAT
    afatfsJournal_t journal;
#endif
    afatfsFile_t openFiles[AFATFS_MAX_OPEN_FILES];
#ifdef AFATFS_USE_FREEFILE
    afatfsFile_t freeFile;
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    afatfsFile_t introSpecLog;
#endif
    // The current working directory:
    afatfsFile_t currentDirectory;
#ifdef ASYNCFS_ENABLE_EXFAT
    afatfsFile_t journalFile;
#endif
    // The createFile operation has to walk the CWD, but there can only be one active walker at a time.
    // createFile must claim this mutex before beginning a directory walk (findFirst) on the global CWD.
    afatfsFile_t *cwdWalkMutex;
#ifdef ASYNCFS_ENABLE_EXFAT
    afatfsFile_t *allocationOwner;
#endif
    afatfsCacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_SECTORS];
#ifdef AFATFS_USE_FREEFILE
    // Used during initialization; only one member is live at a time.
    union {
        afatfsFreeSpaceSearch_t freeSpaceSearch;
        afatfsFreeSpaceFAT_t freeSpaceFAT;
    } initState;
#endif
    uint32_t cacheTimer;
    int cacheDirtyEntries; // The number of cache entries in the AFATFS_CACHE_STATE_DIRTY state
    uint32_t partitionSectors;
    LBA_t partitionStartSector; // The physical sector that the first partition on the device begins at
    LBA_t fatStartSector;       // The first sector of the first FAT
    LBA_t fatSectors;           // The size in sectors of a single FAT
    /*
     * Number of clusters available for storing user data. Note that clusters are numbered starting from 2, so the
     * index of the last cluster on the volume is numClusters + 1 !!!
     */
    uint32_t numClusters;
    LBA_t clusterStartSector; // The physical sector that the clusters area begins at
    uint32_t sectorsPerCluster;
    /*
     * Number of the cluster we last allocated (i.e. free->occupied). Searches for a free cluster will begin after this
     * cluster.
     */
    LBA_t lastClusterAllocated;
    /* Mask to be ANDed with a byte offset within a file to give the offset within the cluster */
    uint32_t byteInClusterMask;
    LBA_t rootDirectoryCluster;    // Present on FAT32 and set to zero for FAT16
    uint32_t rootDirectorySectors; // Zero on FAT32, for FAT16 the number of sectors that the root directory occupies
#ifdef ASYNCFS_ENABLE_EXFAT
    uint32_t bitmapCluster;
    uint32_t bitmapLength;
    uint32_t bitmapCursorCluster;
    uint32_t bitmapCursorIndex;
    uint32_t upcaseCluster;
    uint32_t upcaseLength;
    uint32_t upcaseChecksum;
    uint32_t mountOffset;
    uint32_t mountChecksum;
    uint32_t mountCluster;
    uint32_t mountClusterIndex;
    uint32_t upcaseCodepoint;
    afatfsFinder_t mountFinder;
    uint32_t rootEntriesRead;
    uint32_t bootChecksum;
    uint32_t volumeSerial;
    union {
        uint16_t upcaseAscii[128];
        afatfsGpt_t gpt;
    };
#endif
    fatFilesystemType_e fileSystemType;
    afatfsFilesystemState_e fileSystemState;
    afatfsInitializationPhase_e initPhase;
    afatfsError_e lastError;
    uint8_t partitionType; // MBR type, or the internal exFAT read-only marker selected by GPT.
#ifdef ASYNCFS_ENABLE_EXFAT
    uint8_t exfatMountPhase;
#endif
    bool cacheFlushInProgress : 1;
    bool filesystemFull : 1;
    bool readOnly : 1;
    bool fsInfoDirty : 1;
#ifdef ASYNCFS_ENABLE_EXFAT
    bool upcaseSkip : 1;
    bool exfatClean : 1;
#endif
} afatfs_t;

static afatfs_t afatfs;
static uint32_t afatfs_mediaGeneration; // Survives mount-state resets; no per-handle fields.
static afatfsRequest_t *afatfs_request;
static afatfsDirectory_t *afatfs_directories;
enum {
    AFATFS_API_OPEN = 1,
    AFATFS_API_CLOSE,
    AFATFS_API_SYNC,
    AFATFS_API_TRUNCATE,
    AFATFS_API_STAT,
    AFATFS_API_OPENDIR,
    AFATFS_API_FIND,
    AFATFS_API_RENAME,
    AFATFS_API_UNLINK,
    AFATFS_API_RMDIR,
    AFATFS_API_CHMOD,
    AFATFS_API_UTIME,
    AFATFS_API_FREE,
    AFATFS_API_GETLABEL,
    AFATFS_API_SETLABEL,
    AFATFS_API_CWD,
    AFATFS_API_EXPAND,
    AFATFS_API_GETS,
    AFATFS_API_PUTS,
    AFATFS_API_READ,
    AFATFS_API_WRITE,
    AFATFS_API_FORWARD,
    AFATFS_API_MKFS,
    AFATFS_API_FDISK
};
static void afatfs_apiPoll(void);

static bool afatfs_mediaSessionActive(void)
{
    return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION ||
        afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_READY ||
        afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_CLOSING;
}

static void afatfs_resetMediaSession(afatfsFilesystemState_e state)
{
    if (afatfs_request) {
        afatfs_request->result =
            state == AFATFS_FILESYSTEM_STATE_REMOVED ? AFATFS_ERROR_MEDIA_REMOVED : AFATFS_ERROR_CANCELED;
        afatfs_request->complete = true;
        afatfs_request = NULL;
    }
    afatfs_directories = NULL;
    uint32_t generation = afatfs_mediaGeneration + 1;
    if (!generation) {
        generation = 1;
    }
    afatfs_mediaGeneration = generation;                      // Retire callbacks before stopping DMA/the backend.
    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_REMOVED; // Block reentrant I/O during cancellation.
    sdcard_reset(); // Must stop all accesses to old buffers/media before returning.
    memset(&afatfs, 0, sizeof(afatfs));
    afatfs.fileSystemState = state;
}

#ifdef ASYNCFS_ENABLE_EXFAT
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index);
static void afatfs_journalPoll(void);
static bool afatfs_journalCommitted(void);
static bool afatfs_journalUnresolvedError(void)
{
    // A runtime validation error must not discard accepted application writes.
    // A read-only mount of an already-invalid volume has no accepted writes.
    return afatfs.journal.ready && afatfs.journal.phase == AFATFS_JOURNAL_ERROR;
}
#endif

static void afatfs_fileOperationContinue(afatfsFile_t *file);
static bool afatfs_truncateTo(afatfsFilePtr_t file, uint32_t size, afatfsFileCallback_t callback, void *user);
static uint8_t *afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file, bool isFullSector);
static uint8_t *afatfs_fileRetainCursorSectorForRead(afatfsFilePtr_t file);

AFATFS_WEAK uint32_t afatfs_fatTime(void)
{
    return ((uint32_t)AFATFS_DEFAULT_FILE_DATE << 16) | AFATFS_DEFAULT_FILE_TIME;
}

static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
    uint32_t remainder = value % rounding;

    if (remainder > 0) {
        value += rounding - remainder;
    }

    return value;
}

static bool isPowerOfTwo(unsigned int value)
{
    return ((value != 0) && ((value & (~value + 1)) == value));
}

/**
 * Check for conditions that should always be true (and if otherwise mean a bug or a corrupt filesystem).
 *
 * If the condition is false, the filesystem is marked as being in a fatal state.
 *
 * Returns the value of the condition.
 */
static bool afatfs_assert(bool condition)
{
    if (!condition) {
        if (afatfs.lastError == AFATFS_ERROR_NONE) {
            afatfs.lastError = AFATFS_ERROR_GENERIC;
        }
        afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
#ifdef AFATFS_DEBUG
#ifdef _MSC_VER
        raise(SIGABRT);
#else
        raise(SIGTRAP);
#endif
#endif
    }

    return condition;
}

static bool afatfs_fileIsBusy(afatfsFilePtr_t file)
{
    return file->operation.active != AFATFS_FILE_OPERATION_NONE ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->file == file &&
            afatfs_request->operation >= AFATFS_API_CLOSE && afatfs_request->operation <= AFATFS_API_TRUNCATE) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->operation == AFATFS_API_FREE &&
            afatfs_request->phase && file->type == AFATFS_FILE_TYPE_NORMAL) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->operation == AFATFS_API_EXPAND &&
            afatfs_request->phase && file->type == AFATFS_FILE_TYPE_NORMAL) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->file == file &&
            afatfs_request->operation >= AFATFS_API_EXPAND && afatfs_request->operation <= AFATFS_API_FORWARD);
}

static bool afatfs_fileError(afatfsFilePtr_t file, afatfsError_e error)
{
    if (file) {
        file->error = (uint8_t)error;
    }
    afatfs.lastError = error;
    return false;
}

afatfsError_e afatfs_ferror(afatfsFilePtr_t file)
{
    return file && file->type != AFATFS_FILE_TYPE_NONE ? (afatfsError_e)file->error : AFATFS_ERROR_INVALID_OBJECT;
}

void afatfs_clearerr(afatfsFilePtr_t file)
{
    if (file && file->type != AFATFS_FILE_TYPE_NONE) {
        file->error = AFATFS_ERROR_NONE;
    }
}

/**
 * The number of FAT table entries that fit within one AFATFS sector size.
 *
 * Note that this is the same as the number of clusters in an AFATFS supercluster.
 */
static uint32_t afatfs_fatEntriesPerSector()
{
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16 ? AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR
                                                              : AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
}

/**
 * Size of a FAT cluster in bytes
 */
ONLY_EXPOSE_FOR_TESTING
LBA_t afatfs_clusterSize()
{
    return afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;
}

/**
 * Given a byte offset within a file, return the byte offset of that position within the cluster it belongs to.
 */
static LBA_t afatfs_byteIndexInCluster(LBA_t byteOffset)
{
    return afatfs.byteInClusterMask & byteOffset;
}

/**
 * Given a byte offset within a file, return the index of the sector within the cluster it belongs to.
 */
static LBA_t afatfs_sectorIndexInCluster(LBA_t byteOffset)
{
    return afatfs_byteIndexInCluster(byteOffset) / AFATFS_SECTOR_SIZE;
}

// Get the buffer memory for the cache entry of the given index.
static uint8_t *afatfs_cacheSectorGetMemory(int cacheEntryIndex)
{
    return afatfs.cache + cacheEntryIndex * AFATFS_SECTOR_SIZE;
}

static int afatfs_getCacheDescriptorIndexForBuffer(uint8_t *memory)
{
    int index = (int)((memory - afatfs.cache) / AFATFS_SECTOR_SIZE);

    if (afatfs_assert(index >= 0 && index < AFATFS_NUM_CACHE_SECTORS)) {
        return index;
    } else {
        return -1;
    }
}

static afatfsCacheBlockDescriptor_t *afatfs_getCacheDescriptorForBuffer(uint8_t *memory)
{
    return afatfs.cacheDescriptor + afatfs_getCacheDescriptorIndexForBuffer(memory);
}

static void afatfs_cacheSectorMarkDirty(afatfsCacheBlockDescriptor_t *descriptor)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        afatfs.fsInfoDirty = true;
    }
    if (descriptor->state != AFATFS_CACHE_STATE_DIRTY) {
        descriptor->writeTimestamp = ++afatfs.cacheTimer;
        descriptor->state = AFATFS_CACHE_STATE_DIRTY;
        afatfs.cacheDirtyEntries++;
    }
}

static void afatfs_cacheSectorInit(afatfsCacheBlockDescriptor_t *descriptor, LBA_t sectorIndex, bool locked)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    descriptor->journalEpoch = 0;
#endif
    descriptor->sectorIndex = sectorIndex;

    descriptor->accessTimestamp = descriptor->writeTimestamp = ++afatfs.cacheTimer;

    descriptor->consecutiveEraseBlockCount = 0;

    descriptor->state = AFATFS_CACHE_STATE_EMPTY;

    descriptor->locked = locked;
    descriptor->retainCount = 0;
    descriptor->discardable = 0;
}

/**
 * Called by the SD card driver when one of our read operations completes.
 */
static void afatfs_sdcardReadComplete(
    sdcardBlockOperation_e operation, LBA_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
    (void)operation;
    if (callbackData != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) {
        return;
    }
    if (buffer == NULL) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_EMPTY &&
            afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            if (buffer == NULL) {
                // Read failed, mark the sector as empty and whoever asked for it will ask for it again later to retry
                afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_EMPTY;
            } else {
                afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer &&
                    afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING);

                afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            }

            break;
        }
    }
}

/**
 * Called by the SD card driver when one of our write operations completes.
 */
static void afatfs_sdcardWriteComplete(
    sdcardBlockOperation_e operation, LBA_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
    (void)operation;
    if (callbackData != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) {
        return;
    }
    if (buffer == NULL) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }

    afatfs.cacheFlushInProgress = false;

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        /* Keep in mind that someone may have marked the sector as dirty after writing had already begun. In this case we must leave
         * it marked as dirty because those modifications may have been made too late to make it to the disk!
         */
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex &&
            afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_WRITING) {
            if (buffer == NULL) {
                // Write failed, remark the sector as dirty
                afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_DIRTY;
                afatfs.cacheDirtyEntries++;
            } else {
                afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer);

                afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            }
            break;
        }
    }
}

/**
 * Attempt to flush the dirty cache entry with the given index to the SDcard.
 */
static void afatfs_cacheFlushSector(int cacheIndex)
{
    afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[cacheIndex];

#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    if (cacheDescriptor->consecutiveEraseBlockCount) {
        sdcard_beginWriteBlocks(cacheDescriptor->sectorIndex, cacheDescriptor->consecutiveEraseBlockCount);
    }
#endif

    switch (sdcard_writeBlock(cacheDescriptor->sectorIndex, afatfs_cacheSectorGetMemory(cacheIndex),
        afatfs_sdcardWriteComplete, afatfs_mediaGeneration)) {
        case SDCARD_OPERATION_IN_PROGRESS:
            // The card will call us back later when the buffer transmission finishes
            afatfs.cacheDirtyEntries--;
            cacheDescriptor->state = AFATFS_CACHE_STATE_WRITING;
            afatfs.cacheFlushInProgress = true;
            break;

        case SDCARD_OPERATION_SUCCESS:
            // Buffer is already transmitted
            afatfs.cacheDirtyEntries--;
            cacheDescriptor->state = AFATFS_CACHE_STATE_IN_SYNC;
            break;

        case SDCARD_OPERATION_FAILURE:
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            break;
        case SDCARD_OPERATION_BUSY:
        default:;
    }
}

/**
 * Find a sector in the cache which corresponds to the given physical sector index, or NULL if the sector isn't
 * cached. Note that the cached sector could be in any state including completely empty.
 */
static afatfsCacheBlockDescriptor_t *afatfs_findCacheSector(uint32_t sectorIndex)
{
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            return &afatfs.cacheDescriptor[i];
        }
    }

    return NULL;
}

/**
 * Find or allocate a cache sector for the given sector index on disk. Returns a block which matches one of these
 * conditions (in descending order of preference):
 *
 * - The requested sector that already exists in the cache
 * - The index of an empty sector
 * - The index of a synced discardable sector
 * - The index of the oldest synced sector
 *
 * Otherwise it returns -1 to signal failure (cache is full!)
 */
static int afatfs_allocateCacheSector(LBA_t sectorIndex)
{
    int allocateIndex;
    int emptyIndex = -1;
    int discardableIndex = -1;

    LBA_t oldestSyncedSectorLastUse = 0xFFFFFFFF;
    int oldestSyncedSectorIndex = -1;

    if (!afatfs_assert(afatfs.numClusters ==
                0 // We're unable to check sector bounds during startup since we haven't read volume label yet
            || sectorIndex < afatfs.clusterStartSector + afatfs.numClusters * afatfs.sectorsPerCluster)) {
        return -1;
    }

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            /*
             * If the sector is actually empty then do a complete re-init of it just like the standard
             * empty case. (Sectors marked as empty should be treated as if they don't have a block index assigned)
             */
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_EMPTY) {
                emptyIndex = i;
                break;
            }

            // Bump the last access time
            afatfs.cacheDescriptor[i].accessTimestamp = ++afatfs.cacheTimer;
            return i;
        }

        switch (afatfs.cacheDescriptor[i].state) {
            case AFATFS_CACHE_STATE_EMPTY:
                emptyIndex = i;
                break;
            case AFATFS_CACHE_STATE_IN_SYNC:
                // Is this a synced sector that we could evict from the cache?
                if (!afatfs.cacheDescriptor[i].locked && afatfs.cacheDescriptor[i].retainCount == 0) {
                    if (afatfs.cacheDescriptor[i].discardable) {
                        discardableIndex = i;
                    } else if (afatfs.cacheDescriptor[i].accessTimestamp < oldestSyncedSectorLastUse) {
                        // This is older than last block we decided to evict, so evict this one in preference
                        oldestSyncedSectorLastUse = afatfs.cacheDescriptor[i].accessTimestamp;
                        oldestSyncedSectorIndex = i;
                    }
                }
                break;
            default:;
        }
    }

    if (emptyIndex > -1) {
        allocateIndex = emptyIndex;
    } else if (discardableIndex > -1) {
        allocateIndex = discardableIndex;
    } else if (oldestSyncedSectorIndex > -1) {
        allocateIndex = oldestSyncedSectorIndex;
    } else {
        allocateIndex = -1;
    }

    if (allocateIndex > -1) {
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
    }

    return allocateIndex;
}

/**
 * Attempt to flush dirty cache pages out to the sdcard, returning true if all flushable data has been flushed.
 */
static bool afatfs_flushCache()
{
    if (!afatfs_mediaSessionActive()) {
        return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs_journalUnresolvedError()) {
        return false;
    }
#endif
    if (afatfs.cacheFlushInProgress) {
        return false;
    }
    if (afatfs.cacheDirtyEntries > 0) {
        // Flush the oldest flushable sector
        uint32_t earliestSectorTime = 0xFFFFFFFF;
        int earliestSectorIndex = -1;

        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY && !afatfs.cacheDescriptor[i].locked &&
                (earliestSectorIndex == -1 || afatfs.cacheDescriptor[i].writeTimestamp < earliestSectorTime)) {
                earliestSectorIndex = i;
                earliestSectorTime = afatfs.cacheDescriptor[i].writeTimestamp;
            }
        }

        if (earliestSectorIndex > -1) {
            afatfs_cacheFlushSector(earliestSectorIndex);

            // That flush will take time to complete so we may as well tell caller to come back later
            return false;
        }
    }

    return true;
}

/**
 * Flush cache pages and wait for the exFAT metadata checkpoint, if enabled.
 */
bool afatfs_flush(void)
{
    if (!afatfs_flushCache()) {
        return false;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (!afatfs_journalCommitted()) {
        return false;
    }
#endif
    return true;
}

/**
 * Returns true if either the freefile or the regular cluster pool has been exhausted during a previous write operation.
 */
bool afatfs_isFull()
{
    return afatfs.filesystemFull;
}

bool afatfs_isReadOnly(void)
{
    return afatfs.readOnly;
}

/**
 * Get the physical sector number that corresponds to the FAT sector of the given fatSectorIndex within the given
 * FAT (fatIndex may be 0 or 1). (0, 0) gives the first sector of the first FAT.
 */
static LBA_t afatfs_fatSectorToPhysical(int fatIndex, LBA_t fatSectorIndex)
{
    return afatfs.fatStartSector + (fatIndex ? afatfs.fatSectors : 0) + fatSectorIndex;
}

static LBA_t afatfs_fileClusterToPhysical(LBA_t clusterNumber, LBA_t sectorIndex)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

static LBA_t afatfs_fileGetCursorPhysicalSector(afatfsFilePtr_t file)
{
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + file->cursorOffset / AFATFS_SECTOR_SIZE;
    } else {
        LBA_t cursorSectorInCluster = afatfs_sectorIndexInCluster(file->cursorOffset);
        return afatfs_fileClusterToPhysical(file->cursorCluster, cursorSectorInCluster);
    }
}

/**
 * Sector here is the sector index within the cluster.
 */
static void afatfs_fileGetCursorClusterAndSector(afatfsFilePtr_t file, LBA_t *cluster, uint16_t *sector)
{
    *cluster = file->cursorCluster;

    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        *sector = file->cursorOffset / AFATFS_SECTOR_SIZE;
    } else {
        *sector = afatfs_sectorIndexInCluster(file->cursorOffset);
    }
}

/**
 * Get a cache entry for the given sector and store a pointer to the cached memory in *buffer.
 *
 * physicalSectorIndex - The index of the sector in the SD card to cache
 * sectorflags         - A union of AFATFS_CACHE_* constants that says which operations the sector will be cached for.
 * buffer              - A pointer to the 512-byte memory buffer for the sector will be stored here upon success
 * eraseCount          - For write operations, set to a non-zero number to hint that we plan to write that many sectors
 *                       consecutively (including this sector)
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 */
static afatfsOperationStatus_e afatfs_cacheSector(
    LBA_t physicalSectorIndex, uint8_t **buffer, uint8_t sectorFlags, uint32_t eraseCount)
{
    if (!afatfs_mediaSessionActive()) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (afatfs.readOnly && (sectorFlags & AFATFS_CACHE_WRITE)) {
        return AFATFS_OPERATION_FAILURE;
    }
    // We never write to the MBR, so any attempt to write there is an asyncfatfs bug
    if (!afatfs_assert((sectorFlags & AFATFS_CACHE_WRITE) == 0 || physicalSectorIndex != 0)) {
        return AFATFS_OPERATION_FAILURE;
    }

    int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex);

    if (cacheSectorIndex == -1) {
        // We don't have enough free cache to service this request right now, try again later
        return AFATFS_OPERATION_IN_PROGRESS;
    }

#ifdef ASYNCFS_ENABLE_EXFAT
    if ((sectorFlags & (AFATFS_CACHE_METADATA | AFATFS_CACHE_WRITE)) == (AFATFS_CACHE_METADATA | AFATFS_CACHE_WRITE) &&
        afatfs.journal.ready) {
        afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[cacheSectorIndex];
        if (cacheDescriptor->state == AFATFS_CACHE_STATE_EMPTY) {
            sectorFlags |= AFATFS_CACHE_READ;
        } else if (cacheDescriptor->state != AFATFS_CACHE_STATE_READING) {
            afatfsOperationStatus_e status = afatfs_journalBeforeWrite(cacheSectorIndex);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
        }
    }
#endif

    switch (afatfs.cacheDescriptor[cacheSectorIndex].state) {
        case AFATFS_CACHE_STATE_READING:
            return AFATFS_OPERATION_IN_PROGRESS;
            break;

        case AFATFS_CACHE_STATE_EMPTY:
            if ((sectorFlags & AFATFS_CACHE_READ) != 0) {
                if (sdcard_readBlock(physicalSectorIndex, afatfs_cacheSectorGetMemory(cacheSectorIndex),
                        afatfs_sdcardReadComplete, afatfs_mediaGeneration)) {
                    afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_READING;
                }
                return AFATFS_OPERATION_IN_PROGRESS;
            }

            // We only get to decide these fields if we're the first ones to cache the sector:
            afatfs.cacheDescriptor[cacheSectorIndex].discardable =
                (sectorFlags & AFATFS_CACHE_DISCARDABLE) != 0 ? 1 : 0;

#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
            // Don't bother pre-erasing for small block sequences
            if (eraseCount < AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT) {
                eraseCount = 0;
            } else {
                eraseCount = MIN(eraseCount,
                    UINT16_MAX); // If caller asked for a longer chain of sectors we silently truncate that here
            }

            afatfs.cacheDescriptor[cacheSectorIndex].consecutiveEraseBlockCount = eraseCount;
#endif

            // Fall through

        case AFATFS_CACHE_STATE_WRITING:
        case AFATFS_CACHE_STATE_IN_SYNC:
            if ((sectorFlags & AFATFS_CACHE_WRITE) != 0) {
                afatfs_cacheSectorMarkDirty(&afatfs.cacheDescriptor[cacheSectorIndex]);
            }
            // Fall through

        case AFATFS_CACHE_STATE_DIRTY:
            if ((sectorFlags & AFATFS_CACHE_LOCK) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].locked = 1;
            }
            if ((sectorFlags & AFATFS_CACHE_RETAIN) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].retainCount++;
            }

            *buffer = afatfs_cacheSectorGetMemory(cacheSectorIndex);

            return AFATFS_OPERATION_SUCCESS;
            break;

        default:
            afatfs_assert(false);
            // Cache block in unknown state, should never happen
            return AFATFS_OPERATION_FAILURE;
    }
}

#ifdef ASYNCFS_ENABLE_EXFAT
// CRC-32/ISO-HDLC, reflected polynomial 0xEDB88320; initial/final XOR stay with callers.
// Constant byte lookup table: 1 KiB of read-only storage, no runtime initialization.
static const uint32_t afatfs_crc32Table[256] = {
    0x00000000U,
    0x77073096U,
    0xEE0E612CU,
    0x990951BAU,
    0x076DC419U,
    0x706AF48FU,
    0xE963A535U,
    0x9E6495A3U,
    0x0EDB8832U,
    0x79DCB8A4U,
    0xE0D5E91EU,
    0x97D2D988U,
    0x09B64C2BU,
    0x7EB17CBDU,
    0xE7B82D07U,
    0x90BF1D91U,
    0x1DB71064U,
    0x6AB020F2U,
    0xF3B97148U,
    0x84BE41DEU,
    0x1ADAD47DU,
    0x6DDDE4EBU,
    0xF4D4B551U,
    0x83D385C7U,
    0x136C9856U,
    0x646BA8C0U,
    0xFD62F97AU,
    0x8A65C9ECU,
    0x14015C4FU,
    0x63066CD9U,
    0xFA0F3D63U,
    0x8D080DF5U,
    0x3B6E20C8U,
    0x4C69105EU,
    0xD56041E4U,
    0xA2677172U,
    0x3C03E4D1U,
    0x4B04D447U,
    0xD20D85FDU,
    0xA50AB56BU,
    0x35B5A8FAU,
    0x42B2986CU,
    0xDBBBC9D6U,
    0xACBCF940U,
    0x32D86CE3U,
    0x45DF5C75U,
    0xDCD60DCFU,
    0xABD13D59U,
    0x26D930ACU,
    0x51DE003AU,
    0xC8D75180U,
    0xBFD06116U,
    0x21B4F4B5U,
    0x56B3C423U,
    0xCFBA9599U,
    0xB8BDA50FU,
    0x2802B89EU,
    0x5F058808U,
    0xC60CD9B2U,
    0xB10BE924U,
    0x2F6F7C87U,
    0x58684C11U,
    0xC1611DABU,
    0xB6662D3DU,
    0x76DC4190U,
    0x01DB7106U,
    0x98D220BCU,
    0xEFD5102AU,
    0x71B18589U,
    0x06B6B51FU,
    0x9FBFE4A5U,
    0xE8B8D433U,
    0x7807C9A2U,
    0x0F00F934U,
    0x9609A88EU,
    0xE10E9818U,
    0x7F6A0DBBU,
    0x086D3D2DU,
    0x91646C97U,
    0xE6635C01U,
    0x6B6B51F4U,
    0x1C6C6162U,
    0x856530D8U,
    0xF262004EU,
    0x6C0695EDU,
    0x1B01A57BU,
    0x8208F4C1U,
    0xF50FC457U,
    0x65B0D9C6U,
    0x12B7E950U,
    0x8BBEB8EAU,
    0xFCB9887CU,
    0x62DD1DDFU,
    0x15DA2D49U,
    0x8CD37CF3U,
    0xFBD44C65U,
    0x4DB26158U,
    0x3AB551CEU,
    0xA3BC0074U,
    0xD4BB30E2U,
    0x4ADFA541U,
    0x3DD895D7U,
    0xA4D1C46DU,
    0xD3D6F4FBU,
    0x4369E96AU,
    0x346ED9FCU,
    0xAD678846U,
    0xDA60B8D0U,
    0x44042D73U,
    0x33031DE5U,
    0xAA0A4C5FU,
    0xDD0D7CC9U,
    0x5005713CU,
    0x270241AAU,
    0xBE0B1010U,
    0xC90C2086U,
    0x5768B525U,
    0x206F85B3U,
    0xB966D409U,
    0xCE61E49FU,
    0x5EDEF90EU,
    0x29D9C998U,
    0xB0D09822U,
    0xC7D7A8B4U,
    0x59B33D17U,
    0x2EB40D81U,
    0xB7BD5C3BU,
    0xC0BA6CADU,
    0xEDB88320U,
    0x9ABFB3B6U,
    0x03B6E20CU,
    0x74B1D29AU,
    0xEAD54739U,
    0x9DD277AFU,
    0x04DB2615U,
    0x73DC1683U,
    0xE3630B12U,
    0x94643B84U,
    0x0D6D6A3EU,
    0x7A6A5AA8U,
    0xE40ECF0BU,
    0x9309FF9DU,
    0x0A00AE27U,
    0x7D079EB1U,
    0xF00F9344U,
    0x8708A3D2U,
    0x1E01F268U,
    0x6906C2FEU,
    0xF762575DU,
    0x806567CBU,
    0x196C3671U,
    0x6E6B06E7U,
    0xFED41B76U,
    0x89D32BE0U,
    0x10DA7A5AU,
    0x67DD4ACCU,
    0xF9B9DF6FU,
    0x8EBEEFF9U,
    0x17B7BE43U,
    0x60B08ED5U,
    0xD6D6A3E8U,
    0xA1D1937EU,
    0x38D8C2C4U,
    0x4FDFF252U,
    0xD1BB67F1U,
    0xA6BC5767U,
    0x3FB506DDU,
    0x48B2364BU,
    0xD80D2BDAU,
    0xAF0A1B4CU,
    0x36034AF6U,
    0x41047A60U,
    0xDF60EFC3U,
    0xA867DF55U,
    0x316E8EEFU,
    0x4669BE79U,
    0xCB61B38CU,
    0xBC66831AU,
    0x256FD2A0U,
    0x5268E236U,
    0xCC0C7795U,
    0xBB0B4703U,
    0x220216B9U,
    0x5505262FU,
    0xC5BA3BBEU,
    0xB2BD0B28U,
    0x2BB45A92U,
    0x5CB36A04U,
    0xC2D7FFA7U,
    0xB5D0CF31U,
    0x2CD99E8BU,
    0x5BDEAE1DU,
    0x9B64C2B0U,
    0xEC63F226U,
    0x756AA39CU,
    0x026D930AU,
    0x9C0906A9U,
    0xEB0E363FU,
    0x72076785U,
    0x05005713U,
    0x95BF4A82U,
    0xE2B87A14U,
    0x7BB12BAEU,
    0x0CB61B38U,
    0x92D28E9BU,
    0xE5D5BE0DU,
    0x7CDCEFB7U,
    0x0BDBDF21U,
    0x86D3D2D4U,
    0xF1D4E242U,
    0x68DDB3F8U,
    0x1FDA836EU,
    0x81BE16CDU,
    0xF6B9265BU,
    0x6FB077E1U,
    0x18B74777U,
    0x88085AE6U,
    0xFF0F6A70U,
    0x66063BCAU,
    0x11010B5CU,
    0x8F659EFFU,
    0xF862AE69U,
    0x616BFFD3U,
    0x166CCF45U,
    0xA00AE278U,
    0xD70DD2EEU,
    0x4E048354U,
    0x3903B3C2U,
    0xA7672661U,
    0xD06016F7U,
    0x4969474DU,
    0x3E6E77DBU,
    0xAED16A4AU,
    0xD9D65ADCU,
    0x40DF0B66U,
    0x37D83BF0U,
    0xA9BCAE53U,
    0xDEBB9EC5U,
    0x47B2CF7FU,
    0x30B5FFE9U,
    0xBDBDF21CU,
    0xCABAC28AU,
    0x53B39330U,
    0x24B4A3A6U,
    0xBAD03605U,
    0xCDD70693U,
    0x54DE5729U,
    0x23D967BFU,
    0xB3667A2EU,
    0xC4614AB8U,
    0x5D681B02U,
    0x2A6F2B94U,
    0xB40BBE37U,
    0xC30C8EA1U,
    0x5A05DF1BU,
    0x2D02EF8DU,
};

static uint32_t afatfs_crc32Update(uint32_t crc, const uint8_t *bytes, unsigned length)
{
    while (length--) {
        crc = (crc >> 8) ^ afatfs_crc32Table[(crc ^ *bytes++) & 0xFF];
    }
    return crc;
}

static uint32_t afatfs_gptGet32(const uint8_t *buffer)
{
    return buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}

static uint64_t afatfs_gptGet64(const uint8_t *buffer)
{
    return afatfs_gptGet32(buffer) | (uint64_t)afatfs_gptGet32(buffer + 4) << 32;
}

static bool afatfs_gptUsedEntry(const uint8_t *entry)
{
    return afatfs_gptGet64(entry) || afatfs_gptGet64(entry + 8);
}

static bool afatfs_gptHeader(const uint8_t *sector)
{
    afatfsGpt_t *gptState = &afatfs.gpt;
    uint32_t size = afatfs_gptGet32(sector + 12);
    uint32_t zero = 0;
    if (memcmp(sector, "EFI PART", 8) || afatfs_gptGet32(sector + 8) != 0x10000 || size < 92 ||
        size > AFATFS_SECTOR_SIZE || afatfs_gptGet32(sector + 20)) {
        return false;
    }
    uint32_t crc = afatfs_crc32Update(UINT32_MAX, sector, 16);
    crc = afatfs_crc32Update(crc, (const uint8_t *)&zero, 4);
    crc = afatfs_crc32Update(crc, sector + 20, size - 20);
    if (~crc != afatfs_gptGet32(sector + 16)) {
        return false;
    }
    if (!gptState->backupKnown) {
        uint64_t backup = afatfs_gptGet64(sector + 32);
        if (backup < 67 || backup > UINT32_MAX) {
            return false;
        }
        gptState->lastSector = (uint32_t)backup;
        gptState->backupKnown = true;
    }
    if (afatfs_gptGet64(sector + 24) != gptState->headerSector ||
        afatfs_gptGet64(sector + 32) != (gptState->headerSector == 1 ? gptState->lastSector : 1) ||
        !afatfs_gptUsedEntry(sector + 56)) {
        return false;
    }
    uint64_t first = afatfs_gptGet64(sector + 40);
    uint64_t last = afatfs_gptGet64(sector + 48);
    uint64_t table = afatfs_gptGet64(sector + 72);
    uint32_t count = afatfs_gptGet32(sector + 80);
    uint32_t entrySize = afatfs_gptGet32(sector + 84);
    uint64_t bytes = (uint64_t)count * entrySize;
    uint64_t sectors = (bytes + 511) / 512;
    uint64_t reservedSectors = MAX(UINT64_C(32), sectors);
    if (!count || entrySize < 128 || !isPowerOfTwo(entrySize) || bytes > UINT32_MAX || table < 2 ||
        table > UINT32_MAX || first < 2 + reservedSectors || first > last || last >= gptState->lastSector ||
        last + reservedSectors >= gptState->lastSector ||
        (gptState->headerSector == 1 ? table + reservedSectors > first
                                     : table <= last || table + sectors > gptState->lastSector)) {
        return false;
    }
    gptState->firstUsable = (uint32_t)first;
    gptState->lastUsable = (uint32_t)last;
    gptState->tableSector = (uint32_t)table;
    gptState->tableBytes = (uint32_t)bytes;
    gptState->entrySize = entrySize;
    gptState->tableChecksum = afatfs_gptGet32(sector + 88);
    gptState->offset = 0;
    gptState->crc = UINT32_MAX;
    return true;
}

static bool afatfs_gptTryBackup(void)
{
    afatfsGpt_t *gptState = &afatfs.gpt;
    if (gptState->headerSector != 1 || !gptState->backupKnown) {
        return false;
    }
    gptState->headerSector = gptState->lastSector;
    afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_HEADER;
    return true;
}

// One cached sector per step, without retaining pointers across asynchronous reads.
static afatfsOperationStatus_e afatfs_gptContinue(void)
{
    static const uint8_t basicData[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    afatfsGpt_t *gptState = &afatfs.gpt;
    uint8_t *sector;
    uint32_t lba = gptState->tableSector + gptState->offset / AFATFS_SECTOR_SIZE;
    if (afatfs.initPhase == AFATFS_INITIALIZATION_READ_GPT_HEADER) {
        lba = gptState->headerSector;
    } else if (afatfs.initPhase == AFATFS_INITIALIZATION_READ_GPT_VOLUME) {
        lba = afatfs.partitionStartSector;
    }
    afatfsOperationStatus_e status = afatfs_cacheSector(lba, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0);
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    switch (afatfs.initPhase) {
        case AFATFS_INITIALIZATION_READ_GPT_HEADER:
            if (!afatfs_gptHeader(sector)) {
                goto invalid;
            }
            afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_ARRAY;
            break;
        case AFATFS_INITIALIZATION_READ_GPT_ARRAY: {
            unsigned length = MIN(AFATFS_SECTOR_SIZE, gptState->tableBytes - gptState->offset);
            for (unsigned n = 0; n < length; n += 128) {
                if ((gptState->offset + n) % gptState->entrySize || !afatfs_gptUsedEntry(sector + n)) {
                    continue;
                }
                uint64_t first = afatfs_gptGet64(sector + n + 32);
                uint64_t last = afatfs_gptGet64(sector + n + 40);
                if (first < gptState->firstUsable || first > last || last > gptState->lastUsable ||
                    !afatfs_gptUsedEntry(sector + n + 16)) {
                    goto invalid;
                }
            }
            gptState->crc = afatfs_crc32Update(gptState->crc, sector, length);
            gptState->offset += length;
            if (gptState->offset == gptState->tableBytes) {
                if (~gptState->crc != gptState->tableChecksum) {
                    goto invalid;
                }
                gptState->offset = 0;
                afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_ENTRY;
            }
            break;
        }
        case AFATFS_INITIALIZATION_READ_GPT_ENTRY: {
            const uint8_t *entry = sector + gptState->offset % AFATFS_SECTOR_SIZE;
            gptState->selectedOffset = gptState->offset;
            gptState->offset += gptState->entrySize;
            if (!memcmp(entry, basicData, sizeof(basicData)) && !(entry[48] & 2)) {
                afatfs.partitionStartSector = (uint32_t)afatfs_gptGet64(entry + 32);
                afatfs.partitionSectors = (uint32_t)(afatfs_gptGet64(entry + 40) - afatfs.partitionStartSector + 1);
                gptState->readOnly = (entry[55] & 0x10) != 0;
                afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_VOLUME;
            } else if (gptState->offset == gptState->tableBytes) {
                return AFATFS_OPERATION_FAILURE;
            }
            break;
        }
        case AFATFS_INITIALIZATION_READ_GPT_VOLUME:
            // Basic Data also hosts NTFS/FAT. Only an exFAT VBR enters the existing volume parser.
            if (!memcmp(sector + 3, "EXFAT   ", 8)) {
                gptState->offset = 0;
                afatfs.initPhase = AFATFS_INITIALIZATION_CHECK_GPT_OVERLAP;
            } else {
                if (gptState->offset == gptState->tableBytes) {
                    return AFATFS_OPERATION_FAILURE;
                }
                afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_ENTRY;
            }
            break;
        case AFATFS_INITIALIZATION_CHECK_GPT_OVERLAP: {
            const uint8_t *entry = sector + gptState->offset % AFATFS_SECTOR_SIZE;
            if (gptState->offset != gptState->selectedOffset && afatfs_gptUsedEntry(entry) &&
                afatfs_gptGet64(entry + 32) < (uint64_t)afatfs.partitionStartSector + afatfs.partitionSectors &&
                afatfs_gptGet64(entry + 40) >= afatfs.partitionStartSector) {
                return AFATFS_OPERATION_FAILURE;
            }
            gptState->offset += gptState->entrySize;
            if (gptState->offset == gptState->tableBytes) {
                afatfs.partitionType = gptState->readOnly ? AFATFS_PARTITION_EXFAT_READ_ONLY : MBR_PARTITION_TYPE_EXFAT;
                afatfs.initPhase = AFATFS_INITIALIZATION_READ_VOLUME_ID;
            }
            break;
        }
        default:
            return AFATFS_OPERATION_FAILURE;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
invalid:
    return afatfs_gptTryBackup() ? AFATFS_OPERATION_IN_PROGRESS : AFATFS_OPERATION_FAILURE;
}
#endif

/** Parse a legacy or protective MBR, preserving the legacy partition selection order. */
static bool afatfs_parseMBR(const uint8_t *sector)
{
    // Check MBR signature
    if (sector[AFATFS_SECTOR_SIZE - 2] != FAT_VOLUME_ID_SIGNATURE_1 ||
        sector[AFATFS_SECTOR_SIZE - 1] != FAT_VOLUME_ID_SIGNATURE_2) {
        return false;
    }

    mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *)(sector + 446);

    for (int i = 0; i < 4; i++) {
#ifdef ASYNCFS_ENABLE_EXFAT
        // A protective entry takes precedence over hybrid/stale legacy entries.
        if (partition[i].type == MBR_PARTITION_TYPE_GPT) {
            if (partition[i].lbaBegin != 1 || partition[i].numSectors < 67) {
                return false;
            }
            afatfs.gpt.headerSector = 1;
            afatfs.gpt.lastSector = partition[i].numSectors;
            afatfs.gpt.backupKnown = partition[i].numSectors != UINT32_MAX;
            afatfs.partitionType = MBR_PARTITION_TYPE_GPT;

            return true;
        }
#endif
        if (partition[i].lbaBegin > 0 &&
            (partition[i].type == MBR_PARTITION_TYPE_FAT32 || partition[i].type == MBR_PARTITION_TYPE_FAT32_LBA ||
                partition[i].type == MBR_PARTITION_TYPE_FAT16 || partition[i].type == MBR_PARTITION_TYPE_FAT16_LBA
#ifdef ASYNCFS_ENABLE_EXFAT
                || partition[i].type == MBR_PARTITION_TYPE_EXFAT
#endif
                )) {
            afatfs.partitionStartSector = partition[i].lbaBegin;
            afatfs.partitionType = partition[i].type;
            afatfs.partitionSectors = partition[i].numSectors;

            return true;
        }
    }

    return false;
}

static bool afatfs_parseVolumeID(const uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *)sector;

    afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_INVALID;

    if (afatfs.partitionType != MBR_PARTITION_TYPE_EXFAT
#ifdef ASYNCFS_ENABLE_EXFAT
        && afatfs.partitionType != AFATFS_PARTITION_EXFAT_READ_ONLY
#endif
    ) {
        if (volume->fat.bytesPerSector != AFATFS_SECTOR_SIZE || volume->fat.numFATs != AFATFS_NUM_FATS ||
            sector[510] != FAT_VOLUME_ID_SIGNATURE_1 || sector[511] != FAT_VOLUME_ID_SIGNATURE_2) {
            return false;
        }

        afatfs.fatStartSector = afatfs.partitionStartSector + volume->fat.reservedSectorCount;

        afatfs.sectorsPerCluster = volume->fat.sectorsPerCluster;
        if (afatfs.sectorsPerCluster < 1 || afatfs.sectorsPerCluster > 128 || !isPowerOfTwo(afatfs.sectorsPerCluster)) {
            return false;
        }

        afatfs.byteInClusterMask = AFATFS_SECTOR_SIZE * afatfs.sectorsPerCluster - 1;

        afatfs.fatSectors =
            volume->fat.FATSize16 != 0 ? volume->fat.FATSize16 : volume->fat.fatDescriptor.fat32.FATSize32;

        // Always zero on FAT32 since rootEntryCount is always zero (this is non-zero on FAT16)
        afatfs.rootDirectorySectors =
            ((volume->fat.rootEntryCount * FAT_DIRECTORY_ENTRY_SIZE) + (volume->fat.bytesPerSector - 1)) /
            volume->fat.bytesPerSector;
        uint32_t totalSectors =
            volume->fat.totalSectors16 != 0 ? volume->fat.totalSectors16 : volume->fat.totalSectors32;
        uint32_t dataSectors = totalSectors -
            (volume->fat.reservedSectorCount + (AFATFS_NUM_FATS * afatfs.fatSectors) + afatfs.rootDirectorySectors);

        afatfs.numClusters = dataSectors / volume->fat.sectorsPerCluster;

        if (afatfs.numClusters <= FAT12_MAX_CLUSTERS) {
            afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_FAT12;

            return false; // FAT12 is not a supported filesystem
        } else if (afatfs.numClusters <= FAT16_MAX_CLUSTERS) {
            afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_FAT16;
        } else {
            afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_FAT32;
        }

        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
            afatfs.rootDirectoryCluster = volume->fat.fatDescriptor.fat32.rootCluster;
        } else {
            // FAT16 doesn't store the root directory in clusters
            afatfs.rootDirectoryCluster = 0;
        }

        uint32_t endOfFATs = afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors;

        afatfs.clusterStartSector = endOfFATs + afatfs.rootDirectorySectors;
    } else {
#ifdef ASYNCFS_ENABLE_EXFAT
        // TexFAT (two FATs) requires transaction semantics this library does not implement.
        if (memcmp(volume->oemName, "EXFAT   ", 8) != 0 || volume->exFat.numFATs != 1 || volume->jmpBoot[0] != 0xEB ||
            volume->jmpBoot[1] != 0x76 || volume->jmpBoot[2] != 0x90 || sector[510] != 0x55 || sector[511] != 0xAA ||
            (volume->exFat.percentInUse > 100 && volume->exFat.percentInUse != 0xFF) ||
            (volume->exFat.volumeFlags & 0x000D) != 0) {
            return false;
        }

        for (uint16_t i = 0; i < 53; i++) {
            if (volume->exFat.mustBeZero[i] != 0) {
                return false; //NTFS is not a supported filesystem
            }
        }

        // Check exFAT version (must be version 1.0)
        if (volume->exFat.fileSystemRevision != 0x100) {
            return false;
        }

        if (volume->exFat.bytesPerSectorShift != 9 || volume->exFat.sectorsPerClusterShift > 16) {
            return false; // Bytes per sector must be equal to the physical sector size
        }

        afatfs.sectorsPerCluster = UINT32_C(1) << volume->exFat.sectorsPerClusterShift;
        uint64_t heapEnd =
            (uint64_t)volume->exFat.clusterHeapOffset + (uint64_t)volume->exFat.clusterCount * afatfs.sectorsPerCluster;
        if (volume->exFat.clusterCount == 0 || volume->exFat.clusterCount > 0xFFFFFFF5 ||
            volume->exFat.rootDirectoryCluster < 2 ||
            volume->exFat.rootDirectoryCluster - 2 >= volume->exFat.clusterCount || volume->exFat.fatOffset < 24 ||
            (uint64_t)volume->exFat.fatLength * AFATFS_SECTOR_SIZE < ((uint64_t)volume->exFat.clusterCount + 2) * 4 ||
            (uint64_t)volume->exFat.fatOffset + volume->exFat.fatLength > volume->exFat.clusterHeapOffset ||
            heapEnd > volume->exFat.volumeLength || volume->exFat.volumeLength > afatfs.partitionSectors ||
            volume->exFat.volumeLength > UINT32_MAX - (uint64_t)afatfs.partitionStartSector) {
            return false; // The storage backend and public API currently use 32-bit addresses.
        }
        afatfs.byteInClusterMask = AFATFS_SECTOR_SIZE * afatfs.sectorsPerCluster - 1;

        afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_EXFAT;
        if (afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY) {
            afatfs.readOnly = true;
        }
        memcpy(&afatfs.volumeSerial, sector + 100, 4);
        afatfs.journal.startupDirty = (volume->exFat.volumeFlags & 2) != 0;

        afatfs.fatStartSector = afatfs.partitionStartSector + volume->exFat.fatOffset;
        afatfs.clusterStartSector = afatfs.partitionStartSector + volume->exFat.clusterHeapOffset;
        afatfs.fatSectors = volume->exFat.fatLength;
        afatfs.numClusters = volume->exFat.clusterCount;
        afatfs.rootDirectoryCluster = volume->exFat.rootDirectoryCluster;
        afatfs.rootDirectorySectors = 0;
        if (volume->exFat.volumeFlags & 2) {
            // Basic exFAT has no journal. Never clear an unverified dirty flag or modify a suspect volume.
            afatfs.readOnly = true;
            afatfs.lastError = AFATFS_ERROR_RECOVERY_REQUIRED;
        }
#else
        return false; // Should never fall here anyways
#endif
    }
    return true;
}

/**
 * Get the position of the FAT entry for the cluster with the given number.
 */
static void afatfs_getFATPositionForCluster(LBA_t cluster, LBA_t *fatSectorIndex, LBA_t *fatSectorEntryIndex)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
        uint32_t entriesPerFATSector = AFATFS_SECTOR_SIZE / sizeof(uint16_t);

        *fatSectorIndex = cluster / entriesPerFATSector;
        *fatSectorEntryIndex = cluster & (entriesPerFATSector - 1);
    } else {
        uint32_t entriesPerFATSector = AFATFS_SECTOR_SIZE / sizeof(uint32_t);

        *fatSectorIndex = cluster / entriesPerFATSector;
        *fatSectorEntryIndex = cluster & (entriesPerFATSector - 1);
    }
}

static bool afatfs_FATIsEndOfChainMarker(uint32_t clusterNumber)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        return fat32_isEndOfChainMarker(clusterNumber);
#ifdef ASYNCFS_ENABLE_EXFAT
    } else if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        return exFat_isEndOfChainMarker(clusterNumber);
#endif
    } else {
        return fat16_isEndOfChainMarker(clusterNumber);
    }
}

/**
 * Look up the FAT to find out which cluster follows the one with the given number and store it into *nextCluster.
 *
 * Use fat_isFreeSpace() and fat_isEndOfChainMarker() on nextCluster to distinguish those special values from regular
 * cluster numbers.
 *
 * Note that if you're trying to find the next cluster of a file, you should be calling afatfs_fileGetNextCluster()
 * instead, as that one supports contiguous freefile-based files (which needn't consult the FAT).
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - *nextCluster is set to the next cluster number
 */
static afatfsOperationStatus_e afatfs_FATGetNextCluster(int fatIndex, LBA_t cluster, LBA_t *nextCluster)
{
    LBA_t fatSectorIndex;
    LBA_t fatSectorEntryIndex;
    afatfsFATSector_t sector;

    afatfs_getFATPositionForCluster(cluster, &fatSectorIndex, &fatSectorEntryIndex);

    afatfsOperationStatus_e result =
        afatfs_cacheSector(afatfs_fatSectorToPhysical(fatIndex, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ, 0);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            *nextCluster = sector.fat16[fatSectorEntryIndex];
        } else {
            *nextCluster = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32
                ? fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex])
                : sector.fat32[fatSectorEntryIndex];
        }
    }

    return result;
}

/**
 * Set the cluster number that follows the given cluster. Pass 0xFFFFFFFF for nextCluster to terminate the FAT chain.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 */
static afatfsOperationStatus_e afatfs_FATSetNextCluster(LBA_t startCluster, LBA_t nextCluster)
{
    afatfsFATSector_t sector;
    LBA_t fatSectorIndex;
    LBA_t fatSectorEntryIndex;
    LBA_t fatPhysicalSector;
    afatfsOperationStatus_e result;

    if (!afatfs_assert(startCluster >= FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
        return AFATFS_OPERATION_FAILURE; // startCluster is not valid;
    }

    afatfs_getFATPositionForCluster(startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    result = afatfs_cacheSector(
        fatPhysicalSector, &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            sector.fat16[fatSectorEntryIndex] = nextCluster;
        } else {
            sector.fat32[fatSectorEntryIndex] = nextCluster;
        }
    }

    return result;
}

/**
 * Bring the logical filesize up to date with the current cursor position.
 */
#ifdef ASYNCFS_ENABLE_EXFAT
static bool afatfs_exfatValidCluster(uint32_t cluster)
{
    return cluster >= 2 && cluster - 2 < afatfs.numClusters;
}

// Walk a metadata stream without restarting the walk every time an asynchronous read yields.
static afatfsOperationStatus_e afatfs_exfatStreamSector(uint32_t firstCluster, uint32_t offset, uint32_t *cursorCluster,
    uint32_t *cursorIndex, uint8_t **sector, uint8_t flags)
{
    uint32_t target = offset / afatfs_clusterSize();
    if (*cursorCluster == 0 || target < *cursorIndex) {
        *cursorCluster = firstCluster;
        *cursorIndex = 0;
    }
    while (*cursorIndex < target) {
        uint32_t next;
        afatfsOperationStatus_e status = afatfs_FATGetNextCluster(0, *cursorCluster, &next);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        if (!afatfs_exfatValidCluster(next)) {
            return AFATFS_OPERATION_FAILURE;
        }
        *cursorCluster = next;
        ++*cursorIndex;
    }
    if (!afatfs_exfatValidCluster(*cursorCluster)) {
        return AFATFS_OPERATION_FAILURE;
    }
    return afatfs_cacheSector(
        afatfs_fileClusterToPhysical(*cursorCluster, (offset % afatfs_clusterSize()) / AFATFS_SECTOR_SIZE), sector,
        flags, 0);
}

static afatfsOperationStatus_e afatfs_exfatBitmap(uint32_t cluster, bool write, bool allocated, bool *value)
{
    uint8_t *sector;
    if (!afatfs_exfatValidCluster(cluster)) {
        return AFATFS_OPERATION_FAILURE;
    }
    uint32_t bit = cluster - 2;
    uint32_t offset = bit / 8;
    if (offset >= afatfs.bitmapLength) {
        return AFATFS_OPERATION_FAILURE;
    }
    afatfsOperationStatus_e status =
        afatfs_exfatStreamSector(afatfs.bitmapCluster, offset, &afatfs.bitmapCursorCluster, &afatfs.bitmapCursorIndex,
            &sector, AFATFS_CACHE_READ | (write ? AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA : 0));
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    uint8_t mask = 1U << (bit & 7);
    if (value) {
        *value = (sector[offset % AFATFS_SECTOR_SIZE] & mask) != 0;
    }
    if (write) {
        if (allocated) {
            sector[offset % AFATFS_SECTOR_SIZE] |= mask;
        } else {
            sector[offset % AFATFS_SECTOR_SIZE] &= (uint8_t)~mask;
        }
    }
    return AFATFS_OPERATION_SUCCESS;
}

static afatfsOperationStatus_e afatfs_exfatWriteEntrySet(afatfsFilePtr_t file)
{
    while (file->exfatWriteIndex < file->exfatEntryCount) {
        // Publish the primary last. Until then the old checksum (or an inactive primary) protects the set.
        unsigned index = file->exfatWriteIndex + 1;
        if (index == file->exfatEntryCount) {
            index = 0;
        }
        uint8_t *sector;
        afatfsDirEntryPointer_t *pos = &file->exfatPositions[index];
        afatfsOperationStatus_e status = afatfs_cacheSector(
            pos->sectorNumberPhysical, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        memcpy(sector + pos->entryIndex * FAT_DIRECTORY_ENTRY_SIZE,
            file->exfatEntries + index * FAT_DIRECTORY_ENTRY_SIZE, FAT_DIRECTORY_ENTRY_SIZE);
        ++file->exfatWriteIndex;
    }
    file->exfatWriteIndex = 0;
    return AFATFS_OPERATION_SUCCESS;
}
#endif

static void afatfs_fileUpdateFilesize(afatfsFile_t *file)
{
    file->logicalSize = MAX(file->logicalSize, file->cursorOffset);
}

static void afatfs_fileUnlockCacheSector(afatfsFilePtr_t file)
{
    if (file->writeLockedCacheIndex != -1) {
        afatfs.cacheDescriptor[file->writeLockedCacheIndex].locked = 0;
        file->writeLockedCacheIndex = -1;
    }
    if (file->readRetainCacheIndex != -1) {
        afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount =
            MAX((int)afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount - 1, 0);
        file->readRetainCacheIndex = -1;
    }
}

/**
 * Starting from and including the given cluster number, find the number of the first cluster which matches the given
 * condition.
 *
 * searchLimit - Last cluster to examine (exclusive). To search the entire volume, pass:
 *                   afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER
 *
 * Condition:
 *     CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR - Find a cluster marked as free in the FAT which lies at the
 *         beginning of its FAT sector. The passed initial search 'cluster' must correspond to the first entry of a FAT sector.
 *     CLUSTER_SEARCH_FREE            - Find a cluster marked as free in the FAT
 *     CLUSTER_SEARCH_OCCUPIED        - Find a cluster marked as occupied in the FAT.
 *
 * Returns:
 *     AFATFS_FIND_CLUSTER_FOUND       - A cluster matching the criteria was found and stored in *cluster
 *     AFATFS_FIND_CLUSTER_IN_PROGRESS - The search is not over, call this routine again later with the updated *cluster value to resume
 *     AFATFS_FIND_CLUSTER_FATAL       - An unexpected read error occurred, the volume should be abandoned
 *     AFATFS_FIND_CLUSTER_NOT_FOUND   - The entire device was searched without finding a suitable cluster (the
 *                                       *cluster points to just beyond the final cluster).
 */
static afatfsFindClusterStatus_e afatfs_findClusterWithCondition(
    afatfsClusterSearchCondition_e condition, LBA_t *cluster, uint32_t searchLimit)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        unsigned inspected = 0;
        while (*cluster < searchLimit) {
            bool allocated;
            afatfsOperationStatus_e status = afatfs_exfatBitmap(*cluster, false, false, &allocated);
            if (status == AFATFS_OPERATION_IN_PROGRESS) {
                return AFATFS_FIND_CLUSTER_IN_PROGRESS;
            }
            if (status != AFATFS_OPERATION_SUCCESS) {
                return AFATFS_FIND_CLUSTER_FATAL;
            }
            if (allocated == (condition == CLUSTER_SEARCH_OCCUPIED)) {
                return AFATFS_FIND_CLUSTER_FOUND;
            }
            ++*cluster;
            // A bitmap sector describes 4096 clusters; bound CPU work as well as disk I/O per poll.
            if (++inspected == 256) {
                return AFATFS_FIND_CLUSTER_IN_PROGRESS;
            }
        }
        return AFATFS_FIND_CLUSTER_NOT_FOUND;
    }
#endif
    afatfsFATSector_t sector;
    LBA_t fatSectorIndex;
    LBA_t fatSectorEntryIndex;

    LBA_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    bool lookingForFree =
        condition == CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR || condition == CLUSTER_SEARCH_FREE;

    int jump;

    // Get the FAT entry which corresponds to this cluster so we can begin our search there
    afatfs_getFATPositionForCluster(*cluster, &fatSectorIndex, &fatSectorEntryIndex);

    switch (condition) {
        case CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR:
            jump = fatEntriesPerSector;

            // We're supposed to call this routine with the cluster properly aligned
            if (!afatfs_assert(fatSectorEntryIndex == 0)) {
                return AFATFS_FIND_CLUSTER_FATAL;
            }
            break;
        case CLUSTER_SEARCH_OCCUPIED:
        case CLUSTER_SEARCH_FREE:
            jump = 1;
            break;
        default:
            afatfs_assert(false);
            return AFATFS_FIND_CLUSTER_FATAL;
    }

    while (*cluster < searchLimit) {

#ifdef AFATFS_USE_FREEFILE
        // If we're looking inside the freefile, we won't find any free clusters! Skip it!
        if (afatfs.freeFile.logicalSize > 0 && *cluster == afatfs.freeFile.firstCluster) {
            *cluster += (afatfs.freeFile.logicalSize + afatfs_clusterSize() - 1) / afatfs_clusterSize();

            // Maintain alignment
            *cluster = roundUpTo(*cluster, jump);
            continue; // Go back to check that the new cluster number is within the volume
        }
#endif

        afatfsOperationStatus_e status = afatfs_cacheSector(afatfs_fatSectorToPhysical(0, fatSectorIndex),
            &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0);

        switch (status) {
            case AFATFS_OPERATION_SUCCESS:
                do {
                    uint32_t clusterNumber;

                    switch (afatfs.fileSystemType) {
                        case FAT_FILESYSTEM_TYPE_FAT16:
                            clusterNumber = sector.fat16[fatSectorEntryIndex];
                            break;
                        case FAT_FILESYSTEM_TYPE_FAT32:
                            clusterNumber = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
                            break;
                        default:
                            return AFATFS_FIND_CLUSTER_FATAL;
                    }

                    if (fat_isFreeSpace(clusterNumber) == lookingForFree) {
                        /*
                         * The final FAT sector may have fewer than fatEntriesPerSector entries in it, so we need to
                         * check the cluster number is valid here before we report a bogus success!
                         */
                        if (*cluster < searchLimit) {
                            return AFATFS_FIND_CLUSTER_FOUND;
                        } else {
                            *cluster = searchLimit;
                            return AFATFS_FIND_CLUSTER_NOT_FOUND;
                        }
                    }

                    (*cluster) += jump;
                    fatSectorEntryIndex += jump;
                } while (fatSectorEntryIndex < fatEntriesPerSector);

                // Move on to the next FAT sector
                fatSectorIndex++;
                fatSectorEntryIndex = 0;
                break;
            case AFATFS_OPERATION_FAILURE:
                return AFATFS_FIND_CLUSTER_FATAL;
                break;
            case AFATFS_OPERATION_IN_PROGRESS:
                return AFATFS_FIND_CLUSTER_IN_PROGRESS;
                break;
        }
    }

    // We looked at every available cluster and didn't find one matching the condition
    *cluster = searchLimit;
    return AFATFS_FIND_CLUSTER_NOT_FOUND;
}

/**
 * Get the cluster that follows the currentCluster in the FAT chain for the given file.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - *nextCluster is set to the next cluster number
 */
static afatfsOperationStatus_e afatfs_fileGetNextCluster(
    afatfsFilePtr_t file, uint32_t currentCluster, uint32_t *nextCluster)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatNoFatChain) {
        uint32_t count = file->physicalSize / afatfs_clusterSize();
        if (!afatfs_exfatValidCluster(currentCluster) || currentCluster < file->firstCluster ||
            currentCluster - file->firstCluster >= count) {
            return AFATFS_OPERATION_FAILURE;
        }
        *nextCluster = currentCluster - file->firstCluster + 1 == count ? 0xFFFFFFFF : currentCluster + 1;
        return AFATFS_OPERATION_SUCCESS;
    }
#endif
#ifndef AFATFS_USE_FREEFILE
    (void)file;
#else
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        uint32_t freeFileStart = afatfs.freeFile.firstCluster;

        afatfs_assert(currentCluster + 1 <= freeFileStart);

        // Would the next cluster lie outside the allocated file? (i.e. beyond the end of the file into the start of the freefile)
        if (currentCluster + 1 == freeFileStart) {
            *nextCluster = 0;
        } else {
            *nextCluster = currentCluster + 1;
        }

        return AFATFS_OPERATION_SUCCESS;
    } else
#endif
    {
        return afatfs_FATGetNextCluster(0, currentCluster, nextCluster);
    }
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Update the FAT to fill the contiguous series of clusters with indexes [*startCluster...endCluster) with the
 * specified pattern.
 *
 * AFATFS_FAT_PATTERN_TERMINATED_CHAIN - Chain the clusters together in linear sequence and terminate the final cluster
 * AFATFS_FAT_PATTERN_CHAIN            - Chain the clusters together without terminating the final entry
 * AFATFS_FAT_PATTERN_FREE             - Mark the clusters as free space
 *
 * Returns -
 *     AFATFS_OPERATION_SUCCESS        - When the entire chain has been written
 *     AFATFS_OPERATION_IN_PROGRESS    - Call again later with the updated *startCluster value in order to resume writing.
 */
static afatfsOperationStatus_e afatfs_FATFillWithPattern(
    afatfsFATPattern_e pattern, LBA_t *startCluster, LBA_t endCluster)
{
    afatfsFATSector_t sector;
    LBA_t fatSectorIndex;
    LBA_t firstEntryIndex;
    LBA_t fatPhysicalSector;
    uint8_t fatEntrySize;
    LBA_t nextCluster;
    afatfsOperationStatus_e result;
    uint32_t eraseSectorCount;

    // Find the position of the initial cluster to begin our fill
    afatfs_getFATPositionForCluster(*startCluster, &fatSectorIndex, &firstEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    // How many consecutive FAT sectors will we be overwriting?
    eraseSectorCount = (endCluster - *startCluster + firstEntryIndex + afatfs_fatEntriesPerSector() - 1) /
        afatfs_fatEntriesPerSector();

    while (*startCluster < endCluster) {
        // The last entry we will fill inside this sector (exclusive):
        uint32_t lastEntryIndex = MIN(firstEntryIndex + (endCluster - *startCluster), afatfs_fatEntriesPerSector());

        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE;

        if (firstEntryIndex > 0 || lastEntryIndex < afatfs_fatEntriesPerSector()) {
            // We're not overwriting the entire FAT sector so we must read the existing contents
            cacheFlags |= AFATFS_CACHE_READ;
        }

        result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, cacheFlags, eraseSectorCount);

        if (result != AFATFS_OPERATION_SUCCESS) {
            return result;
        }

#ifdef AFATFS_DEBUG_VERBOSE
        if (pattern == AFATFS_FAT_PATTERN_FREE) {
            fprintf(stderr, "Marking cluster %u to %u as free in FAT sector %u...\n", *startCluster, endCluster,
                fatPhysicalSector);
        } else {
            fprintf(stderr, "Writing FAT chain from cluster %u to %u in FAT sector %u...\n", *startCluster, endCluster,
                fatPhysicalSector);
        }
#endif

        switch (pattern) {
            case AFATFS_FAT_PATTERN_TERMINATED_CHAIN:
            case AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN:
                nextCluster = *startCluster + 1;
                // Write all the "next cluster" pointers
                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat16[i] = nextCluster;
                    }
                } else {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat32[i] = nextCluster;
                    }
                }

                *startCluster += lastEntryIndex - firstEntryIndex;

                if (pattern == AFATFS_FAT_PATTERN_TERMINATED_CHAIN && *startCluster == endCluster) {
                    // We completed the chain! Overwrite the last entry we wrote with the terminator for the end of the chain
                    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                        sector.fat16[lastEntryIndex - 1] = 0xFFFF;
                    } else {
                        sector.fat32[lastEntryIndex - 1] = 0xFFFFFFFF;
                    }
                    break;
                }
                break;
            case AFATFS_FAT_PATTERN_FREE:
                fatEntrySize = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16 ? sizeof(uint16_t) : sizeof(uint32_t);

                memset(sector.bytes + firstEntryIndex * fatEntrySize, 0,
                    (lastEntryIndex - firstEntryIndex) * fatEntrySize);

                *startCluster += lastEntryIndex - firstEntryIndex;
                break;
        }

        fatPhysicalSector++;
        eraseSectorCount--;
        firstEntryIndex = 0;
    }

    return AFATFS_OPERATION_SUCCESS;
}

#endif

/**
 * Write the directory entry for the file into its `directoryEntryPos` position in its containing directory.
 *
 * mode:
 *     AFATFS_SAVE_DIRECTORY_NORMAL    - Store the file's physical size, not the logical size, in the directory entry
 *     AFATFS_SAVE_DIRECTORY_FOR_CLOSE - We're done extending the file so we can write the logical size now.
 *     AFATFS_SAVE_DIRECTORY_DELETED   - Mark the directory entry as deleted
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - The directory entry has been stored into the directory sector in cache.
 *     AFATFS_OPERATION_IN_PROGRESS - Cache is too busy, retry later
 *     AFATFS_OPERATION_FAILURE - If the filesystem enters the fatal state
 */
#ifdef ASYNCFS_ENABLE_LFN
static afatfsOperationStatus_e afatfs_lfnDelete(afatfsFilePtr_t file);
#endif

static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    if (file->directoryEntryPos.sectorNumberPhysical == 0) {
        return AFATFS_OPERATION_SUCCESS; // Root directories don't have a directory entry
    }
#ifdef ASYNCFS_ENABLE_LFN
    if (mode == AFATFS_SAVE_DIRECTORY_DELETED && file->lfnDeleteIndex) {
        return afatfs_lfnDelete(file);
    }
#endif
    if (file->modified && file->type == AFATFS_FILE_TYPE_NORMAL) {
        file->attrib |= FAT_FILE_ATTRIBUTE_ARCHIVE;
    }

#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (file->exfatEntryCount < 3) {
            return AFATFS_OPERATION_FAILURE;
        }
        if (file->exfatWriteIndex == 0) {
            exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
            exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE);
            bool directory = file->type == AFATFS_FILE_TYPE_DIRECTORY;
            stream->firstCluster = file->firstCluster;
            stream->flags = 1 | (file->exfatNoFatChain ? 2 : 0);
            stream->dataSize =
                directory || mode == AFATFS_SAVE_DIRECTORY_NORMAL ? file->physicalSize : file->logicalSize;
            stream->fileSize = directory
                ? file->physicalSize
                : MIN(file->logicalSize,
                      mode == AFATFS_SAVE_DIRECTORY_NORMAL ? file->committedDataLength : file->validDataLength);
            primary->fileAttirb = file->attrib;
            if (file->modified) {
                uint32_t timeStamp = afatfs_fatTime();
                primary->lastWriteDate = timeStamp >> 16;
                primary->lastWriteTime = timeStamp & 0xFFFF;
            }
            if (mode == AFATFS_SAVE_DIRECTORY_DELETED) {
                for (unsigned n = 0; n < file->exfatEntryCount; n++) {
                    file->exfatEntries[n * FAT_DIRECTORY_ENTRY_SIZE] &= (uint8_t)~EXFAT_DIR_MASK;
                }
            } else {
                primary->checksum =
                    exFat_entrySetChecksum(file->exfatEntries, file->exfatEntryCount * FAT_DIRECTORY_ENTRY_SIZE);
            }
        }
        result = afatfs_exfatWriteEntrySet(file);
        if (result == AFATFS_OPERATION_SUCCESS && mode == AFATFS_SAVE_DIRECTORY_FOR_CLOSE) {
            file->modified = false;
        }
        return result;
    }
#endif

    result = afatfs_cacheSector(
        file->directoryEntryPos.sectorNumberPhysical, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "Saving directory entry to sector %u...\n", file->directoryEntryPos.sectorNumberPhysical);
#endif

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs_assert(file->directoryEntryPos.entryIndex >= 0)) {
            fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)sector + file->directoryEntryPos.entryIndex;

            switch (mode) {
                case AFATFS_SAVE_DIRECTORY_NORMAL:
                    /* We exaggerate the length of the written file so that if power is lost, the end of the file will
                     * still be readable (though the very tail of the file will be uninitialized data).
                     *
                     * This way we can avoid updating the directory entry too many times during fwrites() on the file.
                     */
                    entry->fileSize = file->physicalSize;
                    uint32_t timeStamp = afatfs_fatTime();
                    entry->lastWriteDate = timeStamp >> 16;
                    entry->lastWriteTime = timeStamp & 0xFFFF;
                    break;
                case AFATFS_SAVE_DIRECTORY_DELETED:
                    entry->fileName[0] = FAT_DELETED_FILE_MARKER;
                    //Fall through

                case AFATFS_SAVE_DIRECTORY_FOR_CLOSE:
                    // We write the true length of the file on close.
                    entry->fileSize = file->logicalSize;
            }

            // (sub)directories don't store a filesize in their directory entry:
            if (file->type == AFATFS_FILE_TYPE_DIRECTORY) {
                entry->fileSize = 0;
            }

            entry->firstClusterHigh = file->firstCluster >> 16;
            entry->firstClusterLow = file->firstCluster & 0xFFFF;
            entry->attrib = file->attrib;
            if (file->modified && mode == AFATFS_SAVE_DIRECTORY_FOR_CLOSE) {
                uint32_t timeStamp = afatfs_fatTime();
                entry->lastWriteDate = timeStamp >> 16;
                entry->lastWriteTime = timeStamp & 0xFFFF;
                file->modified = false;
            }
#ifdef ASYNCFS_ENABLE_LFN
            if (mode == AFATFS_SAVE_DIRECTORY_DELETED && file->lfnEntryCount) {
                file->lfnDeleteIndex = 1;
                return afatfs_lfnDelete(file);
            }
#endif
        } else {
            return AFATFS_OPERATION_FAILURE;
        }
    }

    return result;
}

/**
 * Attempt to add a free cluster to the end of the given file. If the file was previously empty, the directory entry
 * is updated to point to the new cluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The cluster has been appended
 *     AFATFS_OPERATION_IN_PROGRESS - Cache was busy, so call again later to continue
 *     AFATFS_OPERATION_FAILURE     - Cluster could not be appended because the filesystem ran out of space
 *                                    (afatfs.filesystemFull is set to true)
 *
 * If the file's operation was AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER, the file operation is cleared upon completion,
 * otherwise it is left alone so that this operation can be called as a sub-operation of some other operation on the
 * file.
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeClusterContinue(afatfsFile_t *file)
{
    afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;
    afatfsOperationStatus_e status;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (afatfs.journal.blockAllocation) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (afatfs.allocationOwner && afatfs.allocationOwner != file) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        afatfs.allocationOwner = file;
    }
#endif

doMore:

    switch (opState->phase) {
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
            switch (afatfs_findClusterWithCondition(
                CLUSTER_SEARCH_FREE, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                case AFATFS_FIND_CLUSTER_FOUND:
                    afatfs.lastClusterAllocated = opState->searchCluster;

                    // Make the cluster available for us to write in
                    file->cursorCluster = opState->searchCluster;
                    file->physicalSize += afatfs_clusterSize();

                    if (opState->previousCluster == 0) {
                        // This is the new first cluster in the file
                        file->firstCluster = opState->searchCluster;
                    }

                    opState->convertCluster = file->firstCluster;
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN;
                    goto doMore;
                    break;
                case AFATFS_FIND_CLUSTER_FATAL:
                case AFATFS_FIND_CLUSTER_NOT_FOUND:
#ifdef ASYNCFS_ENABLE_EXFAT
                    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && !opState->wrapped) {
                        opState->wrapped = true;
                        opState->searchCluster = 2;
                        goto doMore;
                    }
#endif
                    // We couldn't find an empty cluster to append to the file
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
                    goto doMore;
                    break;
                case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                    break;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                status = afatfs_exfatBitmap(opState->searchCluster, true, true, NULL);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP_DURABLE;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP_DURABLE:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && !afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatNoFatChain) {
                while (opState->convertCluster < opState->previousCluster) {
                    status = afatfs_FATSetNextCluster(opState->convertCluster, opState->convertCluster + 1);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    ++opState->convertCluster;
                }
                file->exfatNoFatChain = false;
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->type == AFATFS_FILE_TYPE_DIRECTORY) {
                while (opState->zeroSector < afatfs.sectorsPerCluster) {
                    uint8_t *sector;
                    status =
                        afatfs_cacheSector(afatfs_fileClusterToPhysical(opState->searchCluster, opState->zeroSector),
                            &sector, AFATFS_CACHE_WRITE, 0);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    memset(sector, 0, AFATFS_SECTOR_SIZE);
                    ++opState->zeroSector;
                }
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
            // Terminate the new cluster
            status = afatfs_FATSetNextCluster(opState->searchCluster, 0xFFFFFFFF);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (opState->previousCluster) {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
                } else {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
                }

                goto doMore;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2:
            // Add the new cluster to the pre-existing chain
            status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
                goto doMore;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP;
                goto doMore;
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            if (afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_NORMAL) == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                goto doMore;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.allocationOwner == file) {
                afatfs.allocationOwner = NULL;
            }
#endif
            if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
            }

            return AFATFS_OPERATION_SUCCESS;
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.allocationOwner == file) {
                afatfs.allocationOwner = NULL;
            }
#endif
            if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
            }

            afatfs.filesystemFull = true;
            return AFATFS_OPERATION_FAILURE;
            break;
    }

    return AFATFS_OPERATION_IN_PROGRESS;
}

static void afatfs_appendRegularFreeClusterInitOperationState(
    afatfsAppendFreeCluster_t *state, uint32_t previousCluster)
{
    state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL;
    state->previousCluster = previousCluster;
    state->searchCluster = afatfs.lastClusterAllocated;
    state->wrapped = false;
    state->zeroSector = 0;
}

/**
 * Queue up an operation to append a free cluster to the file and update the file's cursorCluster to point to it.
 *
 * You must seek to the end of the file first, so file.cursorCluster will be 0 for the first call, and
 * `file.cursorPreviousCluster` will be the cluster to append after.
 *
 * Note that the cursorCluster will be updated before this operation is completely finished (i.e. before the FAT is
 * updated) but you can go ahead and write to it before the operation succeeds.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The append completed successfully
 *     AFATFS_OPERATION_IN_PROGRESS - The operation was queued on the file and will complete later
 *     AFATFS_OPERATION_FAILURE     - Operation could not be queued or append failed, check afatfs.fileSystemFull
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeCluster(afatfsFilePtr_t file)
{
    if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    if (afatfs.filesystemFull || afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (file->physicalSize > UINT32_MAX - afatfs_clusterSize()) {
        return AFATFS_OPERATION_FAILURE;
    }

    file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;

    afatfs_appendRegularFreeClusterInitOperationState(
        &file->operation.state.appendFreeCluster, file->cursorPreviousCluster);

    return afatfs_appendRegularFreeClusterContinue(file);
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Size of a AFATFS supercluster in bytes
 */
ONLY_EXPOSE_FOR_TESTING
LBA_t afatfs_superClusterSize()
{
    return afatfs_fatEntriesPerSector() * afatfs_clusterSize();
}

/**
 * Continue to attempt to add a supercluster to the end of the given file.
 *
 * If the file operation was set to AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER and the operation completes, the file's
 * operation is cleared.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On completion
 *     AFATFS_OPERATION_IN_PROGRESS - Operation still in progress
 */
static afatfsOperationStatus_e afatfs_appendSuperclusterContinue(afatfsFile_t *file)
{
    afatfsAppendSupercluster_t *opState = &file->operation.state.appendSupercluster;

    afatfsOperationStatus_e status;

doMore:
    switch (opState->phase) {
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT:
            // Our file steals the first cluster of the freefile

            // We can go ahead and write to that space before the FAT and directory are updated
            file->cursorCluster = afatfs.freeFile.firstCluster;
            file->physicalSize += afatfs_superClusterSize();

            /* Remove the first supercluster from the freefile
             *
             * Even if the freefile becomes empty, we still don't set its first cluster to zero. This is so that
             * afatfs_fileGetNextCluster() can tell where a contiguous file ends (at the start of the freefile).
             *
             * Note that normally the freefile can't become empty because it is allocated as a non-integer number
             * of superclusters to avoid precisely this situation.
             */
            afatfs.freeFile.firstCluster += afatfs_fatEntriesPerSector();
            afatfs.freeFile.logicalSize -= afatfs_superClusterSize();
            afatfs.freeFile.physicalSize -= afatfs_superClusterSize();

            // The new supercluster needs to have its clusters chained contiguously and marked with a terminator at the end
            opState->fatRewriteStartCluster = file->cursorCluster;
            opState->fatRewriteEndCluster = opState->fatRewriteStartCluster + afatfs_fatEntriesPerSector();

            if (opState->previousCluster == 0) {
                // This is the new first cluster in the file so we need to update the directory entry
                file->firstCluster = file->cursorCluster;
            } else {
                /*
                 * We also need to update the FAT of the supercluster that used to end the file so that it no longer
                 * terminates there
                 */
                opState->fatRewriteStartCluster -= afatfs_fatEntriesPerSector();
            }

            opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY;
            goto doMore;
            break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY:
            // First update the freefile's directory entry to remove the first supercluster so we don't risk cross-linking the file
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT;
                goto doMore;
            }
            break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT:
            status = afatfs_FATFillWithPattern(
                AFATFS_FAT_PATTERN_TERMINATED_CHAIN, &opState->fatRewriteStartCluster, opState->fatRewriteEndCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                goto doMore;
            }
            break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            // Update the fileSize/firstCluster in the directory entry for the file
            status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_NORMAL);
            break;
    }

    if ((status == AFATFS_OPERATION_FAILURE || status == AFATFS_OPERATION_SUCCESS) &&
        file->operation.active == AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER) {
        file->operation.active = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

/**
 * Attempt to queue up an operation to append the first supercluster of the freefile to the given `file` (file's cursor
 * must be at end-of-file).
 *
 * The new cluster number will be set into the file's cursorCluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The append completed successfully and the file's cursorCluster has been updated
 *     AFATFS_OPERATION_IN_PROGRESS - The operation was queued on the file and will complete later, or there is already an
 *                                    append in progress.
 *     AFATFS_OPERATION_FAILURE     - Operation could not be queued (file was busy) or append failed (filesystem is full).
 *                                    Check afatfs.fileSystemFull
 */
static afatfsOperationStatus_e afatfs_appendSupercluster(afatfsFilePtr_t file)
{
    uint32_t superClusterSize = afatfs_superClusterSize();

    if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    if (afatfs.freeFile.logicalSize < superClusterSize) {
        afatfs.filesystemFull = true;
    }

    if (afatfs.filesystemFull || afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_FAILURE;
    }

    afatfsAppendSupercluster_t *opState = &file->operation.state.appendSupercluster;

    file->operation.active = AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER;
    opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT;
    opState->previousCluster = file->cursorPreviousCluster;

    return afatfs_appendSuperclusterContinue(file);
}

#endif

/**
 * Queue an operation to add a cluster of free space to the end of the file. Must be called when the file's cursor
 * is beyond the last allocated cluster.
 */
static afatfsOperationStatus_e afatfs_appendFreeCluster(afatfsFilePtr_t file)
{
    afatfsOperationStatus_e status;

#ifdef AFATFS_USE_FREEFILE
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        // Steal the first cluster from the beginning of the freefile if we can
        status = afatfs_appendSupercluster(file);
    } else
#endif
    {
        status = afatfs_appendRegularFreeCluster(file);
    }

    return status;
}

/**
 * Returns true if the file's cursor is sitting beyond the end of the last allocated cluster (i.e. the logical fileSize
 * is not checked).
 */
static bool afatfs_isEndOfAllocatedFile(afatfsFilePtr_t file)
{
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        return file->cursorOffset >= AFATFS_SECTOR_SIZE * afatfs.rootDirectorySectors;
    } else {
        return file->cursorCluster == 0 || afatfs_FATIsEndOfChainMarker(file->cursorCluster);
    }
}

// Best-effort read-ahead within the current cluster. Does not move the cursor,
// retain the prefetched sector, or queue a file operation.
void afatfs_fprefetch(afatfsFilePtr_t file)
{
    if (!afatfs_mediaSessionActive() || !file) {
        return;
    }
    if (((file->mode & AFATFS_FILE_MODE_READ) == 0) || afatfs_fileIsBusy(file) || afatfs_isEndOfAllocatedFile(file)) {
        return;
    }
    uint32_t inSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    uint32_t wanted = (inSector == 0) ? file->cursorOffset : file->cursorOffset - inSector + AFATFS_SECTOR_SIZE;
    if (wanted >= file->logicalSize) {
        return; // The file ends in the cursor's sector.
    }
    uint32_t cursorSectorInCluster = (file->cursorOffset % afatfs_clusterSize()) / AFATFS_SECTOR_SIZE;
    uint32_t ahead = (inSector == 0) ? 0 : 1;
    if (cursorSectorInCluster + ahead >= afatfs.sectorsPerCluster) {
        return; // The next sector is in another cluster.
    }
    uint8_t *buffer;
    afatfs_cacheSector(afatfs_fileGetCursorPhysicalSector(file) + ahead, &buffer, AFATFS_CACHE_READ, 0);
}

/**
 * Take a lock on the sector at the current file cursor position.
 *
 * Returns a pointer to the sector buffer if successful, or NULL if at the end of file (check afatfs_isEndOfAllocatedFile())
 * or the sector has not yet been read in from disk.
 */
static uint8_t *afatfs_fileRetainCursorSectorForRead(afatfsFilePtr_t file)
{
    uint8_t *result;

    LBA_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);

    /* If we've already got a locked sector then we can assume that was the same one that's at the cursor (because this
     * cache is invalidated when crossing a sector boundary)
     */
    if (file->readRetainCacheIndex != -1) {
        if (!afatfs_assert(physicalSector == afatfs.cacheDescriptor[file->readRetainCacheIndex].sectorIndex)) {
            return NULL;
        }

        result = afatfs_cacheSectorGetMemory(file->readRetainCacheIndex);
    } else {
        if (afatfs_isEndOfAllocatedFile(file)) {
            return NULL;
        }

        afatfs_assert(physicalSector > 0); // We never read the root sector using files

        afatfsOperationStatus_e status =
            afatfs_cacheSector(physicalSector, &result, AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN, 0);

        if (status != AFATFS_OPERATION_SUCCESS) {
            // Sector not ready for read
            return NULL;
        }

        file->readRetainCacheIndex = afatfs_getCacheDescriptorIndexForBuffer(result);
    }

    return result;
}

/**
 * Lock the sector at the file's cursor position for write, and return a reference to the memory for that sector.
 *
 * Returns NULL if the cache was too busy, try again later.
 */
static uint8_t *afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file, bool isFullSector)
{
    afatfsOperationStatus_e status;
    uint8_t *result;
    uint32_t eraseBlockCount = 0;

    // Do we already have a sector locked in our cache at the cursor position?
    if (file->writeLockedCacheIndex != -1) {
        LBA_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);

        if (!afatfs_assert(physicalSector == afatfs.cacheDescriptor[file->writeLockedCacheIndex].sectorIndex)) {
            return NULL;
        }

        result = afatfs_cacheSectorGetMemory(file->writeLockedCacheIndex);
    } else {
        // Find / allocate a sector and lock it in the cache so we can rely on it sticking around

        // Are we at the start of an empty file or the end of a non-empty file? If so we need to add a cluster
        if (afatfs_isEndOfAllocatedFile(file) && afatfs_appendFreeCluster(file) != AFATFS_OPERATION_SUCCESS) {
            // The extension of the file is in progress so please call us again later to try again
            return NULL;
        }

        LBA_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);
        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_LOCK;
        uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
        uint32_t offsetOfStartOfSector = file->cursorOffset & ~((uint32_t)AFATFS_SECTOR_SIZE - 1);
        uint32_t offsetOfEndOfSector = offsetOfStartOfSector + AFATFS_SECTOR_SIZE;

        /*
         * If there is data before the write point in this sector, or there could be data after the write-point
         * then we need to have the original contents of the sector in the cache for us to merge into.
         * If we're writing a full sector, even mid-file, we can skip the read.
         */
        if (!isFullSector && (cursorOffsetInSector > 0 || offsetOfEndOfSector < file->logicalSize)) {
            cacheFlags |= AFATFS_CACHE_READ;
        }

#ifdef AFATFS_USE_FREEFILE
        // In contiguous append mode, we'll pre-erase the whole supercluster
        if ((file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CONTIGUOUS)) ==
            (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CONTIGUOUS)) {
            uint32_t cursorOffsetInSupercluster = file->cursorOffset & (afatfs_superClusterSize() - 1);

            eraseBlockCount = afatfs_fatEntriesPerSector() * afatfs.sectorsPerCluster -
                cursorOffsetInSupercluster / AFATFS_SECTOR_SIZE;
        }
#endif

        status = afatfs_cacheSector(physicalSector, &result, cacheFlags, eraseBlockCount);

        if (status != AFATFS_OPERATION_SUCCESS) {
            // Not enough cache available to accept this write / sector not ready for read
            return NULL;
        }

        file->writeLockedCacheIndex = afatfs_getCacheDescriptorIndexForBuffer(result);
    }

    return result;
}

/**
 * Attempt to seek the file pointer by the offset, relative to the current position.
 *
 * Returns true if the seek was completed, or false if you should try again later by calling this routine again (the
 * cursor is not moved and no seek operation is queued on the file for you).
 *
 * You can only seek forwards by the size of a cluster or less, or backwards to stay within the same cluster. Otherwise
 * false will always be returned (calling this routine again will never make progress on the seek).
 *
 * This amount of seek is special because we will have to wait on at most one read operation, so it's easy to make
 * the seek atomic.
 */
static bool afatfs_fseekAtomic(afatfsFilePtr_t file, int32_t offset)
{
    // Seeks within a sector
    uint32_t newSectorOffset = offset + file->cursorOffset % AFATFS_SECTOR_SIZE;

    // i.e. newSectorOffset is non-negative and smaller than AFATFS_SECTOR_SIZE, we're staying within the same sector
    if (newSectorOffset < AFATFS_SECTOR_SIZE) {
        file->cursorOffset += offset;
        return true;
    }

    // We're seeking outside the sector so unlock it if we were holding it
    afatfs_fileUnlockCacheSector(file);

    // FAT16 root directories are made up of contiguous sectors rather than clusters
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        file->cursorOffset += offset;

        return true;
    }

    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);
    uint32_t newOffsetInCluster = offsetInCluster + offset;

    if (offset > (int32_t)clusterSizeBytes || offset < -(int32_t)offsetInCluster) {
        return false;
    }

    // Are we seeking outside the cluster? If so we'll need to find out the next cluster number
    if (newOffsetInCluster >= clusterSizeBytes) {
        uint32_t nextCluster;
        afatfsOperationStatus_e status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;
            file->cursorOffset += bytesToSeek;

            offset -= bytesToSeek;
        } else {
            // Try again later
            return false;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (!afatfs_isEndOfAllocatedFile(file)) {
        file->cursorOffset += offset;
    }

    return true;
}

/**
 * Returns true if the seek was completed, or false if it is still in progress.
 */
static bool afatfs_fseekInternalContinue(afatfsFile_t *file)
{
    afatfsSeek_t *opState = &file->operation.state.seek;
    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);

    afatfsOperationStatus_e status;

    // Keep advancing the cursor cluster forwards to consume seekOffset
    while (offsetInCluster + opState->seekOffset >= clusterSizeBytes && !afatfs_isEndOfAllocatedFile(file)) {
        uint32_t nextCluster;

        status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;

            file->cursorOffset += bytesToSeek;
            opState->seekOffset -= bytesToSeek;
            offsetInCluster = 0;
        } else {
            // Try again later
            return false;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (!afatfs_isEndOfAllocatedFile(file)) {
        file->cursorOffset += opState->seekOffset;
    }

    file->operation.active = AFATFS_FILE_OPERATION_NONE;

    if (opState->callback) {
        opState->callback(file, opState->user);
    }

    return true;
}

/**
 * Seek the file pointer forwards by offset bytes. Calls the callback when the seek is complete.
 *
 * Will happily seek beyond the logical end of the file.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The seek was completed immediately
 *     AFATFS_OPERATION_IN_PROGRESS - The seek was queued and will complete later
 *     AFATFS_OPERATION_FAILURE     - The seek could not be queued because the file was busy with another operation,
 *                                    try again later.
 */
static afatfsOperationStatus_e afatfs_fseekInternal(
    afatfsFilePtr_t file, uint32_t offset, afatfsFileCallback_t callback, void *user)
{
    // See if we can seek without queuing an operation
    if (afatfs_fseekAtomic(file, offset)) {
        if (callback) {
            callback(file, user);
        }

        return AFATFS_OPERATION_SUCCESS;
    } else {
        // Our operation must queue
        if (afatfs_fileIsBusy(file)) {
            return AFATFS_OPERATION_FAILURE;
        }

        afatfsSeek_t *opState = &file->operation.state.seek;

        file->operation.active = AFATFS_FILE_OPERATION_SEEK;
        opState->callback = callback;
        opState->user = user;
        opState->seekOffset = offset;

        return AFATFS_OPERATION_IN_PROGRESS;
    }
}

/**
 * Attempt to seek the file cursor from the given point (`whence`) by the given offset, just like C's fseek().
 *
 * AFATFS_SEEK_SET with offset 0 will always return AFATFS_OPERATION_SUCCESS.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The seek was completed immediately
 *     AFATFS_OPERATION_IN_PROGRESS - The seek was queued and will complete later. Feel free to attempt read/write
 *                                    operations on the file, they'll fail until the seek is complete.
 *     AFATFS_OPERATION_FAILURE     - The seek could not be queued because the file was busy with another operation,
 *                                    try again later.
 */
static void afatfs_fileRewind(afatfsFilePtr_t file)
{
    afatfs_fileUnlockCacheSector(file);
    file->cursorPreviousCluster = 0;
    file->cursorCluster = file->firstCluster;
    file->cursorOffset = 0;
}

afatfsOperationStatus_e afatfs_fseek(
    afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence, afatfsFileCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive() || !file || file->type == AFATFS_FILE_TYPE_NONE) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_FAILURE;
    }
    afatfs_fileUpdateFilesize(file);
    int64_t target;
    switch (whence) {
        case AFATFS_SEEK_CUR:
            target = (int64_t)file->cursorOffset + offset;
            break;
        case AFATFS_SEEK_END:
            target = (int64_t)file->logicalSize + offset;
            break;
        case AFATFS_SEEK_SET:
            target = offset;
            break;
        default:
            return AFATFS_OPERATION_FAILURE;
    }
    if (target < 0) {
        target = 0;
    }
    if ((uint64_t)target > file->logicalSize) {
        target = file->logicalSize;
    }
    if ((whence == AFATFS_SEEK_CUR && target >= file->cursorOffset) || (target == file->cursorOffset && target != 0)) {
        return afatfs_fseekInternal(file, (uint32_t)target - file->cursorOffset, callback, user);
    }
    afatfs_fileRewind(file);
    return afatfs_fseekInternal(file, (uint32_t)target, callback, user);
}

/**
 * Get the byte-offset of the file's cursor from the start of the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 */
bool afatfs_ftell(afatfsFilePtr_t file, uint32_t *position)
{
    if (!file || file->type == AFATFS_FILE_TYPE_NONE || !position) {
        return afatfs_fileError(file, AFATFS_ERROR_INVALID_OBJECT);
    }
    if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        *position = file->cursorOffset;
        return true;
    }
}

/**
 * Get the logical size of the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 */
bool afatfs_fGetSize(afatfsFilePtr_t file, uint32_t *size)
{
    if (!file || file->type == AFATFS_FILE_TYPE_NONE || !size) {
        return afatfs_fileError(file, AFATFS_ERROR_INVALID_OBJECT);
    }
    if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        // The logical size can get out of date, so update it first.
        afatfs_fileUpdateFilesize(file);
        *size = file->logicalSize;
        return true;
    }
}

/**
 * Get the information about the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 */
bool afatfs_fGetFileInfo(afatfsFilePtr_t file, fatDirectoryEntry_t **entry)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        return false;
    }
    if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        uint8_t *sector;
        afatfsOperationStatus_e result;

        if (file->directoryEntryPos.sectorNumberPhysical == 0) {
            *entry = NULL;
            return true; // Root directories don't have a directory entry
        }

        result = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &sector, AFATFS_CACHE_READ, 0);

#ifdef AFATFS_DEBUG_VERBOSE
        fprintf(stderr, "Reading directory entry to sector %u...\n", file->directoryEntryPos.sectorNumberPhysical);
#endif

        if (result != AFATFS_OPERATION_SUCCESS) {
            return false;
        }

        if (afatfs_assert(file->directoryEntryPos.entryIndex >= 0)) {
            *entry = (fatDirectoryEntry_t *)sector + file->directoryEntryPos.entryIndex;
        }

        return true;
    }
}

bool afatfs_fGetExFatFileInfo(afatfsFilePtr_t file, const exFatFileEntry_t **entry, const exFatStreamEntry_t **stream)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT || afatfs_fileIsBusy(file)) {
        return false;
    }
    *entry = file->exfatEntryCount ? (const exFatFileEntry_t *)file->exfatEntries : NULL;
    *stream =
        file->exfatEntryCount ? (const exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE) : NULL;
    return true;
#else
    (void)file;
    (void)entry;
    (void)stream;
    return false;
#endif
}

/**
 * Attempt to advance the directory pointer `finder` to the next entry in the directory.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS -     A pointer to the next directory entry has been loaded into *dirEntry. If the
 *                                    directory was exhausted then *dirEntry will be set to NULL.
 *     AFATFS_OPERATION_IN_PROGRESS - The disk is busy. The pointer is not advanced, call again later to retry.
 */
afatfsOperationStatus_e afatfs_findNext(
    afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry)
{
    uint8_t *sector;

    if (finder->entryIndex == AFATFS_FILES_PER_DIRECTORY_SECTOR - 1) {
        if (afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE)) {
            finder->entryIndex = -1;
            // Fall through to read the first entry of that new sector
        } else {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
    }

    sector = afatfs_fileRetainCursorSectorForRead(directory);

    if (sector) {
        finder->entryIndex++;

        *dirEntry = (fatDirectoryEntry_t *)sector + finder->entryIndex;

        finder->sectorNumberPhysical = afatfs_fileGetCursorPhysicalSector(directory);

        return AFATFS_OPERATION_SUCCESS;
    } else {
        if (afatfs_isEndOfAllocatedFile(directory)) {
            *dirEntry = NULL;

            return AFATFS_OPERATION_SUCCESS;
        }

        return AFATFS_OPERATION_IN_PROGRESS;
    }
}

/**
 * Release resources associated with a find operation. Calling this more than once is harmless.
 */
void afatfs_findLast(afatfsFilePtr_t directory)
{
    afatfs_fileUnlockCacheSector(directory);
}

// Special internal findLast that frees the CWD mutex. Calling this more than once is actually dangerous.
static void afatfs_findLastCWD(afatfsFile_t *directory)
{
    afatfs.cwdWalkMutex = NULL;
    afatfs_findLast(directory);
}

/**
 * Initialise the finder so that the first call with the directory to findNext() will return the first file in the
 * directory.
 */
void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder)
{
    afatfs_fileRewind(directory);
    finder->entryIndex = -1;
}

// Special, mutex-guarded findFirst variant for internal use. Returns AFATFS_OPERATION_FAILURE if
// the mutex was not free, returns AFATFS_OPERATION_SUCCESS if it was free and has now been taken.
// Must be paired with findLastCWD to free the mutex, otherwise the filesystem will deadlock.
static afatfsOperationStatus_e afatfs_findFirstCWD(
    afatfsFile_t *directory, afatfsFinder_t *finder, afatfsFile_t *caller)
{
    if (afatfs.cwdWalkMutex == NULL) {
        afatfs.cwdWalkMutex = caller;
        afatfs_findFirst(directory, finder);
        return AFATFS_OPERATION_SUCCESS;
    } else {
        return AFATFS_OPERATION_FAILURE;
    }
}

static afatfsOperationStatus_e afatfs_extendSubdirectoryContinue(afatfsFile_t *directory)
{
    afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;
    afatfsOperationStatus_e status;
    uint8_t *sectorBuffer;
    LBA_t clusterNumber;
    LBA_t physicalSector;
    uint16_t sectorInCluster;

doMore:
    switch (opState->phase) {
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER:
            status = afatfs_appendRegularFreeClusterContinue(directory);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                    ? AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS
                    : AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE;
                goto doMore;
            }
            break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS:
            // Now, zero out that cluster
            afatfs_fileGetCursorClusterAndSector(directory, &clusterNumber, &sectorInCluster);
            physicalSector = afatfs_fileGetCursorPhysicalSector(directory);

            while (1) {
                status = afatfs_cacheSector(physicalSector, &sectorBuffer, AFATFS_CACHE_WRITE, 0);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                memset(sectorBuffer, 0, AFATFS_SECTOR_SIZE);

                // If this is the first sector of a non-root directory, create the "." and ".." entries
                if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT &&
                    directory->directoryEntryPos.sectorNumberPhysical != 0 && directory->cursorOffset == 0) {
                    fatDirectoryEntry_t *dirEntries = (fatDirectoryEntry_t *)sectorBuffer;

                    memset(dirEntries[0].fileName, ' ', sizeof(dirEntries[0].fileName));
                    dirEntries[0].fileName[0] = '.';
                    dirEntries[0].firstClusterHigh = directory->firstCluster >> 16;
                    dirEntries[0].firstClusterLow = directory->firstCluster & 0xFFFF;
                    dirEntries[0].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;

                    memset(dirEntries[1].fileName, ' ', sizeof(dirEntries[1].fileName));
                    dirEntries[1].fileName[0] = '.';
                    dirEntries[1].fileName[1] = '.';
                    dirEntries[1].firstClusterHigh = opState->parentDirectoryCluster >> 16;
                    dirEntries[1].firstClusterLow = opState->parentDirectoryCluster & 0xFFFF;
                    dirEntries[1].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
                }

                if (sectorInCluster < afatfs.sectorsPerCluster - 1) {
                    // Move to next sector
                    afatfs_assert(afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE));
                    sectorInCluster++;
                    physicalSector++;
                } else {
                    break;
                }
            }

            // Seek back to the beginning of the cluster
            afatfs_assert(afatfs_fseekAtomic(directory, -AFATFS_SECTOR_SIZE * (afatfs.sectorsPerCluster - 1)));
            opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS;
            goto doMore;
            break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS:
            directory->operation.active = AFATFS_FILE_OPERATION_NONE;
            if (afatfs.cwdWalkMutex == directory) {
                afatfs_findLastCWD(&afatfs.currentDirectory);
            }

            if (opState->callback) {
                opState->callback(directory, opState->user);
            }

            return AFATFS_OPERATION_SUCCESS;
            break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE:
            directory->operation.active = AFATFS_FILE_OPERATION_NONE;
            if (afatfs.cwdWalkMutex == directory) {
                afatfs_findLastCWD(&afatfs.currentDirectory);
            }

            if (opState->callback) {
                opState->callback(NULL, opState->user);
            }
            return AFATFS_OPERATION_FAILURE;
            break;
    }

    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * Queue an operation to add a cluster to a sub-directory.
 *
 * Tthe new cluster is zero-filled. "." and ".." entries are added if it is the first cluster of a new subdirectory.
 *
 * The directory must not be busy, otherwise AFATFS_OPERATION_FAILURE is returned immediately.
 *
 * The directory's cursor must lie at the end of the directory file (i.e. isEndOfAllocatedFile() would return true).
 *
 * You must provide parentDirectory if this is the first extension to the subdirectory, otherwise pass NULL for that argument.
 */
static afatfsOperationStatus_e afatfs_extendSubdirectory(
    afatfsFile_t *directory, afatfsFilePtr_t parentDirectory, afatfsFileCallback_t callback, void *user)
{
    // FAT16 root directories cannot be extended
    if (directory->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY || afatfs_fileIsBusy(directory)) {
        return AFATFS_OPERATION_FAILURE;
    }

    /*
     * We'll assume that we're never asked to append the first cluster of a root directory, since any
     * reasonably-formatted volume should have a root!
     */
    afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;

    directory->operation.active = AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY;

    opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL;
    // FAT encodes the root parent as zero, including a FAT32 root stored in cluster 2 or later.
    opState->parentDirectoryCluster =
        parentDirectory && parentDirectory->directoryEntryPos.sectorNumberPhysical ? parentDirectory->firstCluster : 0;
    opState->callback = callback;
    opState->user = user;

    afatfs_appendRegularFreeClusterInitOperationState(&opState->appendFreeCluster, directory->cursorPreviousCluster);

    return afatfs_extendSubdirectoryContinue(directory);
}

/**
 * Allocate space for a new directory entry to be written, store the position of that entry in the finder, and set
 * the *dirEntry pointer to point to the entry within the cached FAT sector. This pointer's lifetime is only as good
 * as the life of the cache, so don't dawdle.
 *
 * Before the first call to this function, call afatfs_findFirst() on the directory.
 *
 * The directory sector in the cache is marked as dirty, so any changes written through to the entry will be flushed out
 * in a subsequent poll cycle.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - Call again later to continue
 *     AFATFS_OPERATION_SUCCESS     - Entry has been inserted and *dirEntry and *finder have been updated
 *     AFATFS_OPERATION_FAILURE     - When the directory is full.
 */
static afatfsOperationStatus_e afatfs_allocateDirectoryEntry(
    afatfsFilePtr_t directory, fatDirectoryEntry_t **dirEntry, afatfsFinder_t *finder)
{
    afatfsOperationStatus_e result;

    if (afatfs_fileIsBusy(directory)) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    while ((result = afatfs_findNext(directory, finder, dirEntry)) == AFATFS_OPERATION_SUCCESS) {
        if (*dirEntry) {
            if (fat_isAvailableEntry(*dirEntry, afatfs.fileSystemType)) {
                afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer((uint8_t *)*dirEntry));

                afatfs_findLastCWD(directory);
                return AFATFS_OPERATION_SUCCESS;
            }
        } else {
            // Need to extend directory size by adding a cluster
            result = afatfs_extendSubdirectory(directory, NULL, NULL, NULL);

            if (result == AFATFS_OPERATION_SUCCESS) {
                // Continue the search in the newly-extended directory
                continue;
            } else {
                // The status (in progress or failure) of extending the directory becomes our status
                break;
            }
        }
    }

    return result;
}

/**
 * Return a pointer to a free entry in the open files table (a file whose type is "NONE"). You should initialize
 * the file afterwards with afatfs_initFileHandle().
 */
static afatfsFilePtr_t afatfs_allocateFileHandle()
{
    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        if (afatfs.openFiles[i].type == AFATFS_FILE_TYPE_NONE) {
            return &afatfs.openFiles[i];
        }
    }

    return NULL;
}

/**
 * Continue the file truncation.
 *
 * When truncation finally succeeds or fails, the current operation is cleared on the file (if the current operation
 * was a truncate), then the truncate operation's callback is called. This allows the truncation to be called as a
 * sub-operation without it clearing the parent file operation.
 */
static afatfsOperationStatus_e afatfs_ftruncateContinue(afatfsFilePtr_t file, bool markDeleted)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    afatfsOperationStatus_e status = AFATFS_OPERATION_FAILURE;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (opState->phase == AFATFS_TRUNCATE_FILE_INITIAL && !afatfs_journalCommitted()) {
            afatfs.journal.blockAllocation = true;
            if (afatfs.allocationOwner == file) {
                afatfs.allocationOwner = NULL;
            }
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (afatfs.journal.blockAllocation && opState->phase != AFATFS_TRUNCATE_FILE_SUCCESS) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (afatfs.allocationOwner && afatfs.allocationOwner != file) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        afatfs.allocationOwner = file;
    }
#endif

#ifdef AFATFS_USE_FREEFILE
    uint32_t oldFreeFileStart;
    uint32_t freeFileGrow;
#endif

doMore:

    switch (opState->phase) {
        case AFATFS_TRUNCATE_FILE_FIND_TAIL:
            if (opState->targetSize && (opState->keepCluster < 2 || opState->keepCluster - 2 >= afatfs.numClusters)) {
                return AFATFS_OPERATION_FAILURE;
            }
            while (opState->clustersToKeep > 1) {
                uint32_t next;
                status = opState->linear ? AFATFS_OPERATION_SUCCESS
                                         : afatfs_FATGetNextCluster(0, opState->keepCluster, &next);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (opState->linear) {
                    next = opState->keepCluster + 1;
                }
                if (afatfs_FATIsEndOfChainMarker(next) || next < 2 || next - 2 >= afatfs.numClusters) {
                    return AFATFS_OPERATION_FAILURE;
                }
                opState->previousCluster = opState->keepCluster;
                opState->keepCluster = next;
                opState->clustersToKeep--;
            }
            if (opState->keepCluster) {
                status = opState->linear ? AFATFS_OPERATION_SUCCESS
                                         : afatfs_FATGetNextCluster(0, opState->keepCluster, &opState->currentCluster);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (opState->linear) {
                    opState->currentCluster = opState->keepCluster + 1;
                }
                if (opState->endCluster && opState->currentCluster == opState->endCluster) {
                    opState->currentCluster = 0xFFFFFFFF;
                }
            }
            opState->startCluster = opState->currentCluster;
            afatfs_fileUnlockCacheSector(file);
            opState->phase = AFATFS_TRUNCATE_FILE_FLUSH_DATA;
            // Fall through
        case AFATFS_TRUNCATE_FILE_FLUSH_DATA:
            if (!afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            file->logicalSize = opState->targetSize;
            file->modified = true;
            file->physicalSize = roundUpTo(opState->targetSize, afatfs_clusterSize());
            if (!opState->targetSize) {
                file->firstCluster = 0;
            }
            file->cursorOffset = opState->targetSize;
            file->cursorCluster = opState->targetSize % afatfs_clusterSize() ? opState->keepCluster : 0;
            file->cursorPreviousCluster = file->cursorCluster ? opState->previousCluster : opState->keepCluster;
            opState->previousCluster = 0; // Bounded tail traversal; the cursor predecessor is now stored on the file.
#ifdef ASYNCFS_ENABLE_EXFAT
            file->validDataLength = MIN(file->validDataLength, opState->targetSize);
            file->committedDataLength = file->validDataLength;
            file->dataDirty = false;
            if (!opState->targetSize) {
                file->exfatNoFatChain = false;
            }
#endif
            opState->phase = AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY;
            // Fall through
        case AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY:
            status = afatfs_saveDirectoryEntry(
                file, markDeleted ? AFATFS_SAVE_DIRECTORY_DELETED : AFATFS_SAVE_DIRECTORY_FOR_CLOSE);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_DIRECTORY_DURABLE;
                goto doMore;
            }
            break;
        case AFATFS_TRUNCATE_FILE_DIRECTORY_DURABLE:
            if (!afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            opState->phase = AFATFS_TRUNCATE_FILE_TERMINATE_CHAIN;
            // Fall through
        case AFATFS_TRUNCATE_FILE_TERMINATE_CHAIN:
            if (opState->keepCluster
#ifdef ASYNCFS_ENABLE_EXFAT
                && !(afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatNoFatChain)
#endif
            ) {
                status = afatfs_FATSetNextCluster(opState->keepCluster, 0xFFFFFFFF);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
            opState->phase = AFATFS_TRUNCATE_FILE_TAIL_DURABLE;
            // Fall through
        case AFATFS_TRUNCATE_FILE_TAIL_DURABLE:
            if (!afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            {
                if (opState->currentCluster == 0 || afatfs_FATIsEndOfChainMarker(opState->currentCluster)) {
                    opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
                    goto doMore;
                }
#ifdef AFATFS_USE_FREEFILE
                if (opState->endCluster && !opState->targetSize && afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT) {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS;
                } else
#endif
                {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL;
                }
                goto doMore;
            }
            break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS:
            // Prepare the clusters to be added back on to the beginning of the freefile
            status = afatfs_FATFillWithPattern(
                AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN, &opState->currentCluster, opState->endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE;
                goto doMore;
            }
            break;
        case AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE:
            // Note, it's okay to run this code several times:
            oldFreeFileStart = afatfs.freeFile.firstCluster;

            afatfs.freeFile.firstCluster = opState->startCluster;

            freeFileGrow = (oldFreeFileStart - opState->startCluster) * afatfs_clusterSize();

            afatfs.freeFile.logicalSize += freeFileGrow;
            afatfs.freeFile.physicalSize += freeFileGrow;

            status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);
            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
                goto doMore;
            }
            break;
#endif
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL:
            while (!afatfs_FATIsEndOfChainMarker(opState->currentCluster)) {
                if (opState->currentCluster < 2 || opState->currentCluster - 2 >= afatfs.numClusters ||
                    opState->previousCluster >= afatfs.numClusters) {
                    return AFATFS_OPERATION_FAILURE;
                }
                if (!opState->nextClusterValid) {
                    if (opState->linear) {
                        opState->nextCluster = opState->currentCluster + 1 == opState->endCluster
                            ? 0xFFFFFFFF
                            : opState->currentCluster + 1;
                        status = AFATFS_OPERATION_SUCCESS;
                    } else {
                        status = afatfs_FATGetNextCluster(0, opState->currentCluster, &opState->nextCluster);
                    }
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    if (!afatfs_FATIsEndOfChainMarker(opState->nextCluster) &&
                        (opState->nextCluster < 2 || opState->nextCluster - 2 >= afatfs.numClusters)) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    opState->nextClusterValid = true;
                }
                if (!opState->fatCleared) {
                    status = afatfs_FATSetNextCluster(opState->currentCluster, 0);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    opState->fatCleared = true;
                }
#ifdef ASYNCFS_ENABLE_EXFAT
                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                    if (!afatfs_flushCache()) {
                        return AFATFS_OPERATION_IN_PROGRESS;
                    }
                    status = afatfs_exfatBitmap(opState->currentCluster, true, false, NULL);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                }
#endif

                // Searches for unallocated regular clusters should be told about this free cluster now
                afatfs.lastClusterAllocated = MIN(afatfs.lastClusterAllocated, opState->currentCluster);
                opState->currentCluster = opState->nextCluster;
                opState->previousCluster++;
                opState->nextClusterValid = false;
                opState->fatCleared = false;
                afatfs.filesystemFull = false;
            }

            opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
            goto doMore;
            break;
        case AFATFS_TRUNCATE_FILE_SUCCESS:
            if (!afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#ifdef AFATFS_USE_FREEFILE
            if (opState->targetSize && (file->mode & AFATFS_FILE_MODE_CONTIGUOUS)) {
                file->mode &= (uint8_t)~AFATFS_FILE_MODE_CONTIGUOUS;
                afatfs.freeFile.operation.active = AFATFS_FILE_OPERATION_NONE;
            }
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && !afatfs_flushCache()) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (afatfs.allocationOwner == file) {
                afatfs.allocationOwner = NULL;
            }
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.journal.ready &&
                !afatfs_journalCommitted()) {
                afatfs.journal.blockAllocation = true;
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#endif
            if (file->operation.active == AFATFS_FILE_OPERATION_TRUNCATE) {
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
            }

            if (opState->callback) {
                opState->callback(file, opState->user);
            }

            return AFATFS_OPERATION_SUCCESS;
            break;
    }

    if (status == AFATFS_OPERATION_FAILURE && file->operation.active == AFATFS_FILE_OPERATION_TRUNCATE) {
        file->operation.active = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

/**
 * Queue an operation to truncate the file at its current cursor, like FatFs f_truncate.
 *
 * Returns true if the operation was successfully queued or false if the file is busy (try again later).
 *
 * The callback is called once the file has been truncated (some time after this routine returns).
 */
bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (!file || file->type == AFATFS_FILE_TYPE_NONE) {
        return afatfs_fileError(file, AFATFS_ERROR_INVALID_OBJECT);
    }
    if (file->type != AFATFS_FILE_TYPE_NORMAL) {
        return afatfs_fileError(file, AFATFS_ERROR_DENIED);
    }
    if (afatfs_fileIsBusy(file)) {
        return afatfs_fileError(file, AFATFS_ERROR_BUSY);
    }
    afatfs_fileUpdateFilesize(file);
    if (!(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) || afatfs.readOnly ||
        (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
        return afatfs_fileError(file, AFATFS_ERROR_DENIED);
    }
    file->error = AFATFS_ERROR_NONE;
    if (file->cursorOffset == file->logicalSize && (file->logicalSize || !file->physicalSize)) {
        if (callback) {
            callback(file, user);
        }
        return true;
    }
    return afatfs_truncateTo(file, file->cursorOffset, callback, user);
}

static bool afatfs_truncateTo(afatfsFilePtr_t file, uint32_t size, afatfsFileCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive() || !file || file->type == AFATFS_FILE_TYPE_NONE) {
        return false;
    }
    afatfsTruncateFile_t *opState;

    if (afatfs.readOnly || afatfs_fileIsBusy(file) ||
        !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) ||
        (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
        return false;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.journal.ready && file->firstCluster == afatfs.journal.firstCluster) {
        return false;
    }
#endif

    file->operation.active = AFATFS_FILE_OPERATION_TRUNCATE;

    opState = &file->operation.state.truncateFile;
    opState->callback = callback;
    opState->user = user;
    opState->phase = AFATFS_TRUNCATE_FILE_INITIAL;
    opState->startCluster = file->firstCluster;
    opState->currentCluster = opState->startCluster;
    opState->nextClusterValid = false;
    opState->fatCleared = false;
    opState->targetSize = size;
    opState->keepCluster = size ? file->firstCluster : 0;
    opState->previousCluster = 0;
    opState->clustersToKeep = size ? (size - 1) / afatfs_clusterSize() + 1 : 0;
    opState->linear = false;
    file->error = AFATFS_ERROR_NONE;

#ifdef AFATFS_USE_FREEFILE
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        // The file is contiguous and ends where the freefile begins
        opState->endCluster = afatfs.freeFile.firstCluster;
        opState->linear = true;
    } else
#endif
    {
        // The range of clusters to delete is not contiguous, so follow it as a linked-list instead
        opState->endCluster = 0;
    }

#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        opState->endCluster =
            file->exfatNoFatChain ? file->firstCluster + file->physicalSize / afatfs_clusterSize() : 0;
        opState->linear = file->exfatNoFatChain;
    }
#endif

    return true;
}

/**
 * Load details from the given FAT directory entry into the file.
 */
static void afatfs_fileLoadDirectoryEntry(afatfsFile_t *file, fatDirectoryEntry_t *entry)
{
    file->firstCluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
    file->logicalSize = entry->fileSize;
    file->physicalSize = roundUpTo(entry->fileSize, afatfs_clusterSize());
    file->attrib = entry->attrib;
    file->type = entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY ? AFATFS_FILE_TYPE_DIRECTORY : AFATFS_FILE_TYPE_NORMAL;
}

#ifdef ASYNCFS_ENABLE_EXFAT
// 1 = match, 0 = another/invalid entry set, -1 = matching but unsupported metadata.
static int afatfs_exfatLoadEntrySet(afatfsFilePtr_t file, const char *name)
{
    exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
    exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE);
    unsigned nameLength = (unsigned)strlen(name);
    if (primary->checksum !=
            exFat_entrySetChecksum(file->exfatEntries, file->exfatEntryCount * FAT_DIRECTORY_ENTRY_SIZE) ||
        stream->type != EXFAT_DIR_STREAM || stream->nameLength != nameLength ||
        file->exfatEntryCount < 2 + (nameLength + 14) / 15) {
        return 0;
    }
    uint16_t hash = 0;
    for (unsigned n = 0; n < nameLength; n++) {
        uint16_t upper = afatfs.upcaseAscii[(uint8_t)name[n]];
        hash = (uint16_t)(((hash >> 1) | ((uint32_t)hash << 15)) + (upper & 0xFF));
        hash = (uint16_t)(((hash >> 1) | ((uint32_t)hash << 15)) + (upper >> 8));
    }
    if (hash != stream->nameHash) {
        return 0;
    }
    for (unsigned n = 0; n < nameLength; n++) {
        uint8_t *entry = file->exfatEntries + (2 + n / 15) * FAT_DIRECTORY_ENTRY_SIZE;
        uint16_t ch = entry[2 + (n % 15) * 2] | ((uint16_t)entry[3 + (n % 15) * 2] << 8);
        if (entry[0] != EXFAT_DIR_FILENAME) {
            return 0;
        }
        if (ch >= 128) {
            return -1; // Avoid creating an ASCII alias of an unsupported Unicode name.
        }
        if (afatfs.upcaseAscii[ch] != afatfs.upcaseAscii[(uint8_t)name[n]]) {
            return 0;
        }
    }
    for (unsigned n = 2 + (nameLength + 14) / 15; n < file->exfatEntryCount; n++) {
        if ((file->exfatEntries[n * FAT_DIRECTORY_ENTRY_SIZE] & 0xE0) != 0xE0) {
            return -1;
        }
    }
    if ((stream->flags & 1) == 0 || (stream->flags & ~3) != 0 || stream->dataSize > UINT32_MAX ||
        stream->fileSize > stream->dataSize || stream->dataSize > UINT32_MAX - (afatfs_clusterSize() - 1) ||
        (stream->dataSize && !afatfs_exfatValidCluster(stream->firstCluster)) ||
        (!stream->dataSize && stream->firstCluster)) {
        return -1;
    }
    if ((primary->fileAttirb & FAT_FILE_ATTRIBUTE_DIRECTORY) &&
        (!stream->dataSize || stream->dataSize % afatfs_clusterSize() || stream->fileSize != stream->dataSize)) {
        return -1;
    }
    file->attrib = (uint8_t)primary->fileAttirb;
    file->type = (file->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) ? AFATFS_FILE_TYPE_DIRECTORY : AFATFS_FILE_TYPE_NORMAL;
    file->firstCluster = stream->firstCluster;
    file->logicalSize = (uint32_t)stream->dataSize;
    file->validDataLength = (uint32_t)stream->fileSize;
    file->committedDataLength = file->validDataLength;
    file->physicalSize = roundUpTo(file->logicalSize, afatfs_clusterSize());
    file->exfatNoFatChain = (stream->flags & 2) != 0;
    if (file->exfatNoFatChain &&
        (!file->physicalSize ||
            (uint64_t)file->firstCluster + file->physicalSize / afatfs_clusterSize() >
                (uint64_t)afatfs.numClusters + 2)) {
        return -1;
    }
    file->directoryEntryPos = file->exfatPositions[0];
    return 1;
}

// This sub-operation owns the CWD mutex until the complete set has been read or written.
// Every cursor and counter survives a cache miss; no pointer into the cache survives a yield.
static afatfsOperationStatus_e afatfs_createExFatContinue(afatfsFilePtr_t file)
{
    afatfsCreateFile_t *createState = &file->operation.state.createFile;
    afatfsOperationStatus_e status;
    fatDirectoryEntry_t *entry;
    for (;;) {
        switch (createState->phase) {
            case AFATFS_CREATEFILE_PHASE_INITIAL:
                if (afatfs_findFirstCWD(&afatfs.currentDirectory, &file->directoryEntryPos, file) !=
                    AFATFS_OPERATION_SUCCESS) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                createState->entriesRead = 0;
                createState->phase = AFATFS_CREATEFILE_PHASE_FIND_FILE;
                break;
            case AFATFS_CREATEFILE_PHASE_FIND_FILE:
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!entry || entry->fileName[0] == 0) {
                    if (!(file->mode & AFATFS_FILE_MODE_CREATE)) {
                        goto failure;
                    }
                    afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
                    createState->entriesRead = 0;
                    createState->directoryEnd = false;
                    createState->entryCount =
                        (uint8_t)(2 + (strlen((char *)createState->fileName) + 14) / 15 + createState->extraEntryCount);
                    if (createState->entryCount > AFATFS_EXFAT_MAX_ENTRIES) {
                        file->error = AFATFS_ERROR_UNSUPPORTED;
                        goto failure;
                    }
                    createState->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
                    break;
                }
                if ((uint8_t)entry->fileName[0] == EXFAT_DIR_FILEDIR) {
                    unsigned count = ((exFatFileEntry_t *)entry)->numOfEntries + 1U;
                    if (count > AFATFS_EXFAT_MAX_ENTRIES) {
                        goto failure;
                    }
                    createState->entriesRead = 0;
                    file->exfatEntryCount = count <= AFATFS_EXFAT_MAX_ENTRIES && count >= 3 ? (uint8_t)count : 0;
                }
                if (file->exfatEntryCount) {
                    unsigned n = createState->entriesRead++;
                    memcpy(file->exfatEntries + n * FAT_DIRECTORY_ENTRY_SIZE, entry, FAT_DIRECTORY_ENTRY_SIZE);
                    file->exfatPositions[n] = file->directoryEntryPos;
                    if (createState->entriesRead == file->exfatEntryCount) {
                        int match = afatfs_exfatLoadEntrySet(file, (char *)createState->fileName);
                        if (match < 0) {
                            goto failure;
                        }
                        if (match > 0) {
                            if (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) {
                                file->error = AFATFS_ERROR_EXISTS;
                                goto failure;
                            }
                            afatfs_findLastCWD(&afatfs.currentDirectory);
                            createState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                            return AFATFS_OPERATION_SUCCESS;
                        }
                        file->exfatEntryCount = 0;
                        createState->entriesRead = 0;
                    }
                }
                break;
            case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE:
                if (afatfs_fileIsBusy(&afatfs.currentDirectory)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!entry) {
                    status = afatfs_extendSubdirectory(&afatfs.currentDirectory, NULL, NULL, NULL);
                    file->directoryEntryPos.entryIndex = -1;
                    if (status == AFATFS_OPERATION_FAILURE) {
                        goto failure;
                    }
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                if (entry->fileName[0] == 0) {
                    createState->directoryEnd = true;
                }
                if (!createState->directoryEnd && !fat_isAvailableEntry(entry, FAT_FILESYSTEM_TYPE_EXFAT)) {
                    createState->entriesRead = 0;
                    break;
                }
                file->exfatPositions[createState->entriesRead++] = file->directoryEntryPos;
                if (createState->entriesRead != createState->entryCount) {
                    break;
                }
                createState->phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB;
                break;
            case AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB:
                if (createState->directoryEnd) {
                    afatfsDirEntryPointer_t previous = file->directoryEntryPos;
                    status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    if (entry) {
                        status = afatfs_journalBeforeWrite(afatfs_getCacheDescriptorIndexForBuffer((uint8_t *)entry));
                        if (status != AFATFS_OPERATION_SUCCESS) {
                            file->directoryEntryPos = previous;
                            return status;
                        }
                        afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer((uint8_t *)entry));
                        memset(entry, 0, FAT_DIRECTORY_ENTRY_SIZE);
                    }
                }
                file->exfatEntryCount = createState->entryCount;
                file->exfatWriteIndex = 0;
                memset(file->exfatEntries, 0, sizeof(file->exfatEntries));
                {
                    exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
                    exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE);
                    primary->type = EXFAT_DIR_FILEDIR;
                    primary->numOfEntries = createState->entryCount - 1;
                    primary->fileAttirb = file->attrib;
                    uint32_t timeStamp = afatfs_fatTime();
                    primary->creationDate = timeStamp >> 16;
                    primary->creationTime = timeStamp & 0xFFFF;
                    stream->type = EXFAT_DIR_STREAM;
                    stream->flags = 1; // New files use FAT chains; existing NoFatChain files are handled on open.
                    stream->nameLength = (uint8_t)strlen((char *)createState->fileName);
                    uint16_t hash = 0;
                    for (unsigned n = 0; n < stream->nameLength; n++) {
                        uint8_t *nameEntry = file->exfatEntries + (2 + n / 15) * FAT_DIRECTORY_ENTRY_SIZE;
                        nameEntry[0] = EXFAT_DIR_FILENAME;
                        nameEntry[2 + (n % 15) * 2] = createState->fileName[n];
                        uint16_t upper = afatfs.upcaseAscii[createState->fileName[n]];
                        hash = (uint16_t)(((hash >> 1) | ((uint32_t)hash << 15)) + (upper & 0xFF));
                        hash = (uint16_t)(((hash >> 1) | ((uint32_t)hash << 15)) + (upper >> 8));
                    }
                    stream->nameHash = hash;
                    if (createState->extraEntryCount) {
                        afatfsFilePtr_t source = afatfs_request->file;
                        memcpy(file->exfatEntries + (createState->entryCount - createState->extraEntryCount) * 32,
                            source->exfatEntries + (source->exfatEntryCount - createState->extraEntryCount) * 32,
                            createState->extraEntryCount * 32);
                    }
                }
                file->directoryEntryPos = file->exfatPositions[0];
                createState->phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY;
                break;
            case AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY:
                if (file->type == AFATFS_FILE_TYPE_DIRECTORY && file->firstCluster == 0) {
                    // Keep the CWD reservation until the initialized first cluster can be published.
                    createState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                    return AFATFS_OPERATION_SUCCESS;
                }
                status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_NORMAL);
                if (status == AFATFS_OPERATION_FAILURE) {
                    goto failure;
                }
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                afatfs_findLastCWD(&afatfs.currentDirectory);
                createState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                return AFATFS_OPERATION_SUCCESS;
            default:
                return AFATFS_OPERATION_SUCCESS;
        }
    }
failure:
    if (afatfs.cwdWalkMutex == file) {
        afatfs_findLastCWD(&afatfs.currentDirectory);
    }
    createState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
    return AFATFS_OPERATION_SUCCESS;
}
#endif

// Reuse the open-file table: readers may share a file, but a writer must be exclusive.
// Entry identity distinguishes empty files and equal names in different directories.
static bool afatfs_fileOpenAllowed(afatfsFilePtr_t file)
{
    if (file->firstCluster >= 2 &&
        (file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA))) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.journal.ready && file != &afatfs.journalFile && file->firstCluster == afatfs.journal.firstCluster) {
            return false;
        }
#endif
#ifdef AFATFS_USE_FREEFILE
        if (file != &afatfs.freeFile && afatfs.freeFile.type != AFATFS_FILE_TYPE_NONE &&
            file->firstCluster == afatfs.freeFile.firstCluster) {
            return false;
        }
#endif
    }
    if (file->type == AFATFS_FILE_TYPE_DIRECTORY) {
        for (unsigned i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
            afatfsFilePtr_t other = &afatfs.openFiles[i];
            if (other != file && other->type == AFATFS_FILE_TYPE_DIRECTORY &&
                other->firstCluster == file->firstCluster &&
                (other->operation.active == AFATFS_FILE_OPERATION_UNLINK ||
                    ((other->mode | file->mode) & AFATFS_FILE_MODE_METADATA))) {
                return false;
            }
        }
    }
    if (file->type != AFATFS_FILE_TYPE_NORMAL) {
        return true;
    }
    const uint8_t writeModes = AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA;
    if ((file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) &&
        (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
        return false;
    }
    for (unsigned i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        afatfsFilePtr_t other = &afatfs.openFiles[i];
        if (other == file || other->type != AFATFS_FILE_TYPE_NORMAL) {
            continue;
        }
        // A pending directory search has not established this handle's identity yet.
        if (other->operation.active == AFATFS_FILE_OPERATION_CREATE_FILE &&
            other->operation.state.createFile.phase != AFATFS_CREATEFILE_PHASE_SUCCESS) {
            continue;
        }
        if (other->directoryEntryPos.sectorNumberPhysical == file->directoryEntryPos.sectorNumberPhysical &&
            other->directoryEntryPos.entryIndex == file->directoryEntryPos.entryIndex &&
            ((other->mode | file->mode) & writeModes)) {
            return false;
        }
    }
    return true;
}

#ifdef ASYNCFS_ENABLE_LFN
#include "asyncfatfs_lfn.inc"
#endif

static void afatfs_createFileContinue(afatfsFile_t *file)
{
    afatfsCreateFile_t *opState = &file->operation.state.createFile;
    static fatDirectoryEntry_t *entry;
    afatfsOperationStatus_e status;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && opState->phase < AFATFS_CREATEFILE_PHASE_SUCCESS) {
        status = afatfs_createExFatContinue(file);
        if (status == AFATFS_OPERATION_FAILURE) {
            if (afatfs.cwdWalkMutex == file) {
                afatfs_findLastCWD(&afatfs.currentDirectory);
            }
            opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
        } else if (status != AFATFS_OPERATION_SUCCESS) {
            return;
        }
    }
#endif

#ifdef ASYNCFS_ENABLE_LFN
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT && opState->phase < AFATFS_CREATEFILE_PHASE_SUCCESS) {
        status = afatfs_createLfnContinue(file);
        if (status == AFATFS_OPERATION_FAILURE) {
            if (afatfs.cwdWalkMutex == file) {
                afatfs_findLastCWD(&afatfs.currentDirectory);
            }
            opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
        } else if (status != AFATFS_OPERATION_SUCCESS) {
            return;
        }
    }
#endif

doMore:
    switch (opState->phase) {
        case AFATFS_CREATEFILE_PHASE_INITIAL:
            if (afatfs_findFirstCWD(&afatfs.currentDirectory, &file->directoryEntryPos, file) ==
                AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_CREATEFILE_PHASE_FIND_FILE;
                goto doMore;
            }
            break;
        case AFATFS_CREATEFILE_PHASE_FIND_FILE:
            do {
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);

                switch (status) {
                    case AFATFS_OPERATION_SUCCESS:
                        // Is this the last entry in the directory?
                        if (entry == NULL || fat_isDirectoryEntryTerminator(entry)) {
                            afatfs_findLastCWD(&afatfs.currentDirectory);

                            if ((file->mode & AFATFS_FILE_MODE_CREATE) != 0) {
                                // The file didn't already exist, so we can create it. Allocate a new directory entry
                                status = afatfs_findFirstCWD(&afatfs.currentDirectory, &file->directoryEntryPos, file);

                                if (status == AFATFS_OPERATION_SUCCESS) {
                                    opState->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
                                    goto doMore;
                                }
                            } else {
                                // File not found.
                                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                                goto doMore;
                            }
                        } else if (!fat_isDirectoryEntryEmpty(entry) && !fat_isVolumeLabel(entry) &&
                            !fat_isLfnEntry(entry) &&
                            memcmp(entry->fileName, (char *)opState->fileName, FAT_FILENAME_LENGTH) == 0) {
                            // We found a file with this name!
                            uint32_t cluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
                            if (entry->fileSize > UINT32_MAX - afatfs_clusterSize() + 1 ||
                                ((entry->fileSize || (entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) &&
                                    (cluster < 2 || cluster - 2 >= afatfs.numClusters))) {
                                afatfs_findLastCWD(&afatfs.currentDirectory);
                                file->error = AFATFS_ERROR_UNSUPPORTED;
                                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                                goto doMore;
                            }
                            afatfs_fileLoadDirectoryEntry(file, entry);

                            afatfs_findLastCWD(&afatfs.currentDirectory);

                            if (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) {
                                file->error = AFATFS_ERROR_EXISTS;
                                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                                goto doMore;
                            }
                            opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                            goto doMore;
                        } // Else this entry doesn't match, fall through and continue the search

                        break;
                    case AFATFS_OPERATION_FAILURE:
                        afatfs_findLastCWD(&afatfs.currentDirectory);
                        opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                        goto doMore;
                        break;
                    case AFATFS_OPERATION_IN_PROGRESS:;
                }
            } while (status == AFATFS_OPERATION_SUCCESS);
            break;
        case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE:
            status = afatfs_allocateDirectoryEntry(&afatfs.currentDirectory, &entry, &file->directoryEntryPos);
            if (status == AFATFS_OPERATION_FAILURE) {
                afatfs_findLastCWD(&afatfs.currentDirectory);
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
            if (status != AFATFS_OPERATION_SUCCESS) {
                break;
            }
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->fileName, opState->fileName, FAT_FILENAME_LENGTH);
            entry->attrib = file->attrib;
            {
                uint32_t timeStamp = afatfs_fatTime();
                entry->creationDate = timeStamp >> 16;
                entry->creationTime = timeStamp & 0xFFFF;
                entry->lastWriteDate = timeStamp >> 16;
                entry->lastWriteTime = timeStamp & 0xFFFF;
            }
            opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
            goto doMore;
        case AFATFS_CREATEFILE_PHASE_SUCCESS:
            if (opState->directory && file->type != AFATFS_FILE_TYPE_DIRECTORY &&
                file->type != AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
                file->error = AFATFS_ERROR_EXISTS;
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
            if (!afatfs_fileOpenAllowed(file)) {
                file->error = AFATFS_ERROR_DENIED;
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
            if (file->type == AFATFS_FILE_TYPE_DIRECTORY && file->firstCluster == 0) {
                afatfsFileCallback_t callback = opState->callback;
                void *user = opState->user;
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
                afatfs_extendSubdirectory(file, &afatfs.currentDirectory, callback, user);
                break;
            }
            if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
                /*
                 * For this high performance file type, we require the directory entry for the file to be retained
                 * in the cache at all times.
                 */
                uint8_t *directorySector;

                status = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &directorySector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN, 0);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    // Retry next time
                    break;
                }
            }

            afatfs_fileRewind(file);

            // Is file empty?
            if (file->cursorCluster == 0) {
#ifdef AFATFS_USE_FREEFILE
                if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
                    if (afatfs_fileIsBusy(&afatfs.freeFile)) {
                        // Someone else's using the freefile, come back later.
                        break;
                    } else {
                        // Lock the freefile for our exclusive access
                        afatfs.freeFile.operation.active = AFATFS_FILE_OPERATION_LOCKED;
                    }
                }
#endif
            } else {
                // We can't guarantee that the existing file contents are contiguous
                file->mode &= ~AFATFS_FILE_MODE_CONTIGUOUS;

                // Seek to the end of the file if it is in append mode
                if ((file->mode & AFATFS_FILE_MODE_APPEND) != 0) {
                    // This replaces our open file operation
                    file->operation.active = AFATFS_FILE_OPERATION_NONE;
                    afatfs_fseekInternal(file, file->logicalSize, opState->callback, opState->user);
                    break;
                }

                // If we're only writing (not reading) the file must be truncated
                if ((file->mode & (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_READ)) ==
                    (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE)) {
                    // This replaces our open file operation
                    file->operation.active = AFATFS_FILE_OPERATION_NONE;
                    afatfs_ftruncate(file, opState->callback, opState->user);
                    break;
                }
            }

            file->operation.active = AFATFS_FILE_OPERATION_NONE;
            if (opState->callback) {
                opState->callback(file, opState->user);
            }
            break;
        case AFATFS_CREATEFILE_PHASE_FAILURE:
            if (!file->error) {
                file->error = AFATFS_ERROR_NOT_FOUND;
            }
            afatfs.lastError = (afatfsError_e)file->error;
            file->type = AFATFS_FILE_TYPE_NONE;
            file->operation.active = AFATFS_FILE_OPERATION_NONE;
            if (opState->callback) {
                opState->callback(NULL, opState->user);
            }
            break;
    }
}

/**
 * Reset the in-memory data for the given handle back to the zeroed initial state
 */
static void afatfs_initFileHandle(afatfsFilePtr_t file)
{
    memset(file, 0, sizeof(*file));
    file->writeLockedCacheIndex = -1;
    file->readRetainCacheIndex = -1;
}

static void afatfs_funlinkContinue(afatfsFilePtr_t file)
{
    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;
    afatfsOperationStatus_e status;

    if (opState->truncateFile.phase == AFATFS_TRUNCATE_FILE_CHECK_DIRECTORY) {
        afatfsTruncateFile_t *scan = &opState->truncateFile;
        bool empty = afatfs_FATIsEndOfChainMarker(scan->currentCluster);
        if (!empty && scan->clustersToKeep == afatfs.sectorsPerCluster) {
            uint32_t next;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatNoFatChain) {
                next = scan->currentCluster + 1;
                if (next - file->firstCluster >= file->physicalSize / afatfs_clusterSize()) {
                    next = 0xFFFFFFFF;
                }
            } else
#endif
            {
                if (afatfs_FATGetNextCluster(0, scan->currentCluster, &next) != AFATFS_OPERATION_SUCCESS) {
                    return;
                }
            }
            scan->currentCluster = next;
            scan->clustersToKeep = 0;
            if (++scan->previousCluster > afatfs.numClusters) {
                afatfs_fileError(file, AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                goto rejectedDirectory;
            }
            empty = afatfs_FATIsEndOfChainMarker(next);
        }
        if (!empty) {
            uint8_t *sector;
            if (afatfs_cacheSector(afatfs_fileClusterToPhysical(scan->currentCluster, scan->clustersToKeep), &sector,
                    AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS) {
                return;
            }
            for (unsigned n = 0; n < AFATFS_SECTOR_SIZE; n += FAT_DIRECTORY_ENTRY_SIZE) {
                fatDirectoryEntry_t *entry = (fatDirectoryEntry_t *)(sector + n);
                if (!sector[n]) {
                    empty = true;
                    break;
                }
#ifdef ASYNCFS_ENABLE_EXFAT
                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                    if (!(sector[n] & EXFAT_DIR_MASK)) {
                        continue;
                    }
                } else
#endif
                {
                    if (fat_isDirectoryEntryEmpty(entry) || fat_isLfnEntry(entry) || fat_isVolumeLabel(entry) ||
                        !memcmp(entry->fileName, ".          ", 11) || !memcmp(entry->fileName, "..         ", 11)) {
                        continue;
                    }
                }
                afatfs_fileError(file, AFATFS_ERROR_NOT_EMPTY);
                goto rejectedDirectory;
            }
            if (!empty) {
                scan->clustersToKeep++;
                return;
            }
        }
        {
            afatfsCallback_t callback = opState->callback;
            void *user = opState->user;
            file->operation.active = AFATFS_FILE_OPERATION_NONE;
            if (!afatfs_truncateTo(file, 0, NULL, NULL)) {
                if (callback) {
                    callback(user);
                }
                return;
            }
            opState->callback = callback;
            opState->user = user;
            file->operation.active = AFATFS_FILE_OPERATION_UNLINK;
        }
    }

    status = afatfs_ftruncateContinue(file, true);

    if (status == AFATFS_OPERATION_SUCCESS) {
        // Once the truncation is completed, we can close the file handle
        file->operation.active = AFATFS_FILE_OPERATION_NONE;
        afatfs_fclose(file, opState->callback, opState->user);
    } else if (status == AFATFS_OPERATION_FAILURE) {
        afatfs_fileError(file, AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.allocationOwner == file) {
            afatfs.allocationOwner = NULL;
        }
#endif
        goto rejectedDirectory;
    }
    return;
rejectedDirectory:
    file->operation.active = AFATFS_FILE_OPERATION_NONE;
    if (opState->callback) {
        opState->callback(opState->user);
    }
}

/**
 * Delete and close the file.
 *
 * Returns true if the operation was successfully queued (callback will be called some time after this routine returns)
 * or false if the file is busy and you should try again later.
 */
bool afatfs_funlink(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (afatfs_request && !afatfs_request->driving) {
        return afatfs_fileError(file, AFATFS_ERROR_BUSY);
    }

    if (!file || file->type == AFATFS_FILE_TYPE_NONE) {
        return true;
    }
    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;

    if (file->type != AFATFS_FILE_TYPE_NORMAL) {
        if (afatfs_fileIsBusy(file)) {
            return afatfs_fileError(file, AFATFS_ERROR_BUSY);
        }
        if (file->type != AFATFS_FILE_TYPE_DIRECTORY || !file->directoryEntryPos.sectorNumberPhysical ||
            file->firstCluster == afatfs.currentDirectory.firstCluster || afatfs.readOnly ||
            (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY) ||
            !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))) {
            return afatfs_fileError(file, AFATFS_ERROR_DENIED);
        }
        for (unsigned i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
            afatfsFilePtr_t other = &afatfs.openFiles[i];
            if (other != file && other->type == AFATFS_FILE_TYPE_DIRECTORY &&
                other->firstCluster == file->firstCluster) {
                return afatfs_fileError(file, AFATFS_ERROR_DENIED);
            }
        }
        for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next) {
            if (dir->cursor.firstCluster == file->firstCluster) {
                return afatfs_fileError(file, AFATFS_ERROR_DENIED);
            }
        }
        memset(opState, 0, sizeof(*opState));
        opState->truncateFile.phase = AFATFS_TRUNCATE_FILE_CHECK_DIRECTORY;
        opState->truncateFile.currentCluster = file->firstCluster;
        opState->callback = callback;
        opState->user = user;
        file->error = AFATFS_ERROR_NONE;
        file->operation.active = AFATFS_FILE_OPERATION_UNLINK;
        return true;
    }

    /*
     * Internally an unlink is implemented by first doing a ftruncate(), marking the directory entry as deleted,
     * then doing a fclose() operation.
     */

    // Start the sub-operation of truncating the file
    if (!afatfs_truncateTo(file, 0, NULL, NULL)) {
        return false;
    }

    /*
     * The unlink operation has its own private callback field so that the truncate suboperation doesn't end up
     * calling back early when it completes:
     */
    opState->callback = callback;
    opState->user = user;

    file->operation.active = AFATFS_FILE_OPERATION_UNLINK;

    return true;
}

/**
 * Open (or create) a file in the CWD with the given fileName.
 *
 * file             - Memory location to store the newly opened file details
 * name             - Filename in "name.ext" format. No path.
 * attrib           - FAT file attributes to give the file (if created)
 * fileMode         - Bitset of AFATFS_FILE_MODE_* constants. Include AFATFS_FILE_MODE_CREATE to create the file if
 *                    it does not exist.
 * callback         - Called when the operation is complete
 */
static afatfsFilePtr_t afatfs_createFile(
    afatfsFilePtr_t file, const char *name, uint8_t attrib, uint8_t fileMode, afatfsFileCallback_t callback, void *user)
{
    afatfsCreateFile_t *opState = &file->operation.state.createFile;

    afatfs_initFileHandle(file);
    opState->directory = (attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs_request &&
        afatfs_request->operation == AFATFS_API_RENAME && name == afatfs_request->otherName &&
        (fileMode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE)) {
        afatfsFilePtr_t source = afatfs_request->file;
        unsigned names = 2 + (((exFatStreamEntry_t *)(source->exfatEntries + 32))->nameLength + 14) / 15;
        opState->extraEntryCount = (uint8_t)(source->exfatEntryCount - names);
    }
#endif
    if (!name || !name[0]) {
        afatfs_fileError(file, AFATFS_ERROR_INVALID_NAME);
        if (callback) {
            callback(NULL, user);
        }
        return file;
    }
#ifdef ASYNCFS_ENABLE_LFN
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT && strcmp(name, ".") && !afatfs_lfnValidName(name)) {
        afatfs_fileError(file, AFATFS_ERROR_INVALID_NAME);
        if (callback) {
            callback(NULL, user);
        }
        return file;
    }
#else
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT && strcmp(name, ".")) {
        unsigned base = 0;
        unsigned extension = 0;
        bool dot = false;
        bool valid = true;
        for (unsigned n = 0; name[n]; n++) {
            uint8_t c = (uint8_t)name[n];
            if (c == '.') {
                if (dot || !base) {
                    valid = false;
                }
                dot = true;
                continue;
            }
            if (c <= 32 || c >= 127 || strchr("\"*+,/:;<=>?[\\]|", c)) {
                valid = false;
            }
            if (dot) {
                extension++;
            } else {
                base++;
            }
            if (base > 8 || extension > 3) {
                valid = false;
                break;
            }
        }
        if (!valid || !base || (dot && !extension)) {
            afatfs_fileError(file, AFATFS_ERROR_INVALID_NAME);
            if (callback) {
                callback(NULL, user);
            }
            return file;
        }
    }
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file != &afatfs.journalFile &&
        (fileMode &
            (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_METADATA))) {
        const char *reserved = AFATFS_JOURNAL_NAME;
        unsigned n = 0;
        for (; name[n] && reserved[n]; n++) {
            unsigned ch = (unsigned char)name[n];
            if (ch >= 'a' && ch <= 'z') {
                ch -= 'a' - 'A';
            }
            if (ch != (unsigned char)reserved[n]) {
                break;
            }
        }
        if (!name[n] && !reserved[n]) {
            afatfs_fileError(file, AFATFS_ERROR_DENIED);
            if (callback) {
                callback(NULL, user);
            }
            return file;
        }
    }
#endif
#ifdef AFATFS_USE_FREEFILE
    if (file != &afatfs.freeFile &&
        (fileMode &
            (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA))) {
        const char *reserved = AFATFS_FREESPACE_FILENAME;
        unsigned n = 0;
        for (; name[n] && reserved[n]; n++) {
            unsigned a = (uint8_t)name[n];
            unsigned b = (uint8_t)reserved[n];
            if (a >= 'a' && a <= 'z') {
                a -= 32;
            }
            if (b >= 'a' && b <= 'z') {
                b -= 32;
            }
            if (a != b) {
                break;
            }
        }
        if (!name[n] && !reserved[n]) {
            afatfs_fileError(file, AFATFS_ERROR_DENIED);
            if (callback) {
                callback(NULL, user);
            }
            return file;
        }
    }
#endif
    if (afatfs.readOnly &&
        (fileMode &
            (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA))) {
        afatfs.lastError = AFATFS_ERROR_RECOVERY_REQUIRED;
        if (callback) {
            callback(NULL, user);
        }
        return file;
    }

    // Queue the operation to finish the file creation
    file->operation.active = AFATFS_FILE_OPERATION_CREATE_FILE;

    file->mode = fileMode;

    if (strcmp(name, ".") == 0) {
        file->firstCluster = afatfs.currentDirectory.firstCluster;
        file->physicalSize = afatfs.currentDirectory.physicalSize;
        file->logicalSize = afatfs.currentDirectory.logicalSize;
        file->attrib = afatfs.currentDirectory.attrib;
        file->type = afatfs.currentDirectory.type;
        file->directoryEntryPos = afatfs.currentDirectory.directoryEntryPos;
#ifdef ASYNCFS_ENABLE_EXFAT
        file->exfatNoFatChain = afatfs.currentDirectory.exfatNoFatChain;
        file->validDataLength = afatfs.currentDirectory.validDataLength;
        file->committedDataLength = afatfs.currentDirectory.committedDataLength;
        file->exfatEntryCount = afatfs.currentDirectory.exfatEntryCount;
        memcpy(file->exfatEntries, afatfs.currentDirectory.exfatEntries, sizeof(file->exfatEntries));
        memcpy(file->exfatPositions, afatfs.currentDirectory.exfatPositions, sizeof(file->exfatPositions));
        file->directoryEntryPos = afatfs.currentDirectory.directoryEntryPos;
#endif
    } else {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            size_t length = strlen(name);
            bool valid = length > 0 && length < FAT_LONG_FILENAME_LENGTH && strcmp(name, "..") != 0;
            for (size_t n = 0; n < length; n++) {
                unsigned char ch = (unsigned char)name[n];
                if (ch < 32 || ch >= 127 || strchr("\"*/:<>?\\|", ch)) {
                    valid = false;
                }
            }
            if (!valid) {
                afatfs_fileError(file, AFATFS_ERROR_INVALID_NAME);
                file->type = AFATFS_FILE_TYPE_NONE;
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
                if (callback) {
                    callback(NULL, user);
                }
                return file;
            }
            memcpy(opState->fileName, name, length + 1);
        } else
#endif
        {
#ifdef ASYNCFS_ENABLE_LFN
            memcpy(opState->fileName, name, strlen(name) + 1);
            if (!afatfs_lfnShortName(name, opState->shortName)) {
                opState->location.aliasNumber = 1;
                afatfs_lfnAlias(opState);
            }
#else
            fat_convertFileNameToFATStyle(name, opState->fileName);
#endif
        }
        file->attrib = attrib;

        if ((attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0) {
            file->type = AFATFS_FILE_TYPE_DIRECTORY;
        } else {
            file->type = AFATFS_FILE_TYPE_NORMAL;
        }
    }

    opState->callback = callback;
    opState->user = user;

    if (strcmp(name, ".") == 0) {
        // Since we already have the directory entry details, we can skip straight to the final operations requried
        opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
    } else {
        opState->phase = AFATFS_CREATEFILE_PHASE_INITIAL;
    }

    afatfs_createFileContinue(file);

    return file;
}

// A durability point releases this handle's data buffers before publishing ValidDataLength.
// Each phase yields through the existing poll/cache machinery; no synchronous I/O is added.
static afatfsOperationStatus_e afatfs_syncFileContinue(afatfsFilePtr_t file, uint8_t *phase)
{
    if (*phase == 0) {
        afatfs_fileUnlockCacheSector(file);
        *phase = 1;
    }
    if (*phase == 1) {
        if (!afatfs_flushCache()) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;
    }
    if (*phase == 2) {
        bool needsCommit = true;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatEntryCount >= 3) {
            const exFatStreamEntry_t *stream =
                (const exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE);
            needsCommit = file->dataDirty || stream->dataSize != file->logicalSize ||
                stream->fileSize != file->validDataLength || stream->firstCluster != file->firstCluster;
        }
#endif
        if (!afatfs.readOnly && file->type != AFATFS_FILE_TYPE_DIRECTORY &&
            file->type != AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY && needsCommit &&
            (file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE))) {
            afatfsOperationStatus_e status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
        }
        *phase = 3;
    }
    if (!afatfs_flushCache()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (!afatfs_journalCommitted()) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        file->committedDataLength = MIN(file->logicalSize, file->validDataLength);
        file->dataDirty = false;
    }
#endif
    if (!sdcard_poll()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    sdcardOperationStatus_e status = sdcard_endWriteBlocks();
    if (status != SDCARD_OPERATION_SUCCESS) {
        if (status == SDCARD_OPERATION_FAILURE) {
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
        }
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#endif
    if (!sdcard_poll()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    return AFATFS_OPERATION_SUCCESS;
}

static void afatfs_fsyncContinue(afatfsFilePtr_t file)
{
    afatfsSyncFile_t *opState = &file->operation.state.syncFile;
    if (afatfs_syncFileContinue(file, &opState->phase) != AFATFS_OPERATION_SUCCESS) {
        return;
    }
    file->operation.active = AFATFS_FILE_OPERATION_NONE;
    if (opState->callback) {
        opState->callback(file, opState->user);
    }
}

bool afatfs_fsync(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (!file || file->type == AFATFS_FILE_TYPE_NONE || afatfs_fileIsBusy(file)) {
        return false;
    }
    afatfs_fileUpdateFilesize(file);
#ifdef ASYNCFS_ENABLE_EXFAT
    if (!afatfs.readOnly && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
        file->type == AFATFS_FILE_TYPE_NORMAL && file->logicalSize == 0 && file->physicalSize != 0) {
        return afatfs_truncateTo(file, 0, callback, user);
    }
#endif
    file->operation.active = AFATFS_FILE_OPERATION_SYNC;
    file->operation.state.syncFile.callback = callback;
    file->operation.state.syncFile.user = user;
    file->operation.state.syncFile.phase = 0;
    afatfs_fsyncContinue(file);
    return true;
}

static void afatfs_fcloseContinue(afatfsFilePtr_t file)
{
    afatfsCacheBlockDescriptor_t *descriptor;
    afatfsCloseFile_t *opState = &file->operation.state.closeFile;
    if (afatfs_syncFileContinue(file, &opState->phase) != AFATFS_OPERATION_SUCCESS) {
        return;
    }

    // Release our reservation on the directory cache if needed
    if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
        descriptor = afatfs_findCacheSector(file->directoryEntryPos.sectorNumberPhysical);

        if (descriptor) {
            descriptor->retainCount = MAX((int)descriptor->retainCount - 1, 0);
        }
    }

    // Release locks on the sector at the file cursor position
    afatfs_fileUnlockCacheSector(file);

#ifdef AFATFS_USE_FREEFILE
    // Release our exclusive lock on the freefile if needed
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        afatfs_assert(afatfs.freeFile.operation.active == AFATFS_FILE_OPERATION_LOCKED);
        afatfs.freeFile.operation.active = AFATFS_FILE_OPERATION_NONE;
    }
#endif

    file->type = AFATFS_FILE_TYPE_NONE;
    file->operation.active = AFATFS_FILE_OPERATION_NONE;

    if (opState->callback) {
        opState->callback(opState->user);
    }
}

/**
 * Returns true if an operation was successfully queued to close the file and destroy the file handle. If the file is
 * currently busy, false is returned and you should retry later.
 *
 * If provided, the callback will be called after the operation completes (pass NULL for no callback).
 *
 * If this function returns true, you should not make any further calls to the file (as the handle might be reused for a
 * new file).
 */
#ifdef ASYNCFS_ENABLE_EXFAT
static void afatfs_closeAfterEmptyTruncate(afatfsFilePtr_t file, void *user)
{
    (void)user;
    afatfsCallback_t callback = file->closeAfterTruncateCallback;
    void *callbackUser = file->closeAfterTruncateUser;
    file->closeAfterTruncateCallback = NULL;
    file->closeAfterTruncateUser = NULL;
    afatfs_fclose(file, callback, callbackUser);
}
#endif

bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (!file || file->type == AFATFS_FILE_TYPE_NONE) {
        return true;
    } else if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        afatfs_fileUpdateFilesize(file);
#ifdef ASYNCFS_ENABLE_EXFAT
        if (!afatfs.readOnly && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
            file->type == AFATFS_FILE_TYPE_NORMAL && file->logicalSize == 0 && file->physicalSize != 0) {
            file->closeAfterTruncateCallback = callback;
            file->closeAfterTruncateUser = user;
            return afatfs_truncateTo(file, 0, afatfs_closeAfterEmptyTruncate, NULL);
        }
#endif

        file->operation.active = AFATFS_FILE_OPERATION_CLOSE;
        file->operation.state.closeFile.callback = callback;
        file->operation.state.closeFile.user = user;
        file->operation.state.closeFile.phase = 0;
        afatfs_fcloseContinue(file);
        return true;
    }
}

/**
 * Create a new directory with the given name, or open the directory if it already exists.
 *
 * The directory will be passed to the callback, or NULL if the creation failed.
 *
 * Returns true if the directory creation was begun, or false if there are too many open files.
 */
bool afatfs_mkdir(const char *fileName, afatfsFileCallback_t callback, void *user)
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        return false;
    }
    if (afatfs_request && !afatfs_request->driving) {
        return false;
    }
    afatfsFilePtr_t file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFile(file, fileName, FAT_FILE_ATTRIBUTE_DIRECTORY,
            AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE, callback, user);
    } else if (callback) {
        callback(NULL, user);
    }

    return file != NULL;
}

/**
 * Change the working directory to the directory with the given handle (use fopen). Pass NULL for `directory` in order to
 * change to the root directory.
 *
 * Returns true on success, false if you should call again later to retry. After changing into a directory, your handle
 * to that directory may be closed by fclose().
 */
bool afatfs_chdir(afatfsFilePtr_t directory)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (afatfs.cwdWalkMutex) {
        return false;
    }
    if (afatfs_request && !afatfs_request->driving) {
        return false;
    }
    if (afatfs_fileIsBusy(&afatfs.currentDirectory)) {
        return false;
    }

    if (directory) {
        if (afatfs_fileIsBusy(directory)) {
            return false;
        }

        memcpy(&afatfs.currentDirectory, directory, sizeof(*directory));
        return true;
    } else {
        afatfs_initFileHandle(&afatfs.currentDirectory);

        afatfs.currentDirectory.mode = AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE;

        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            afatfs.currentDirectory.type = AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY;
        } else {
            afatfs.currentDirectory.type = AFATFS_FILE_TYPE_DIRECTORY;
        }

        afatfs.currentDirectory.firstCluster = afatfs.rootDirectoryCluster;
        afatfs.currentDirectory.attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;

        // Root directories don't have a directory entry to represent themselves:
        afatfs.currentDirectory.directoryEntryPos.sectorNumberPhysical = 0;

        afatfs_fileRewind(&afatfs.currentDirectory);

        return true;
    }
}

/**
 * Begin the process of opening a file with the given name in the current working directory (paths in the fileName are
 * not supported) using the given mode.
 *
 * To open the current working directory, pass "." for fileName.
 *
 * The complete() callback is called when finished with either a file handle (file was opened) or NULL upon failure.
 *
 * Supported file mode strings:
 *
 * r - Read from an existing file
 * w - Create a file for write access, if the file already exists then truncate it
 * a - Create a file for write access to the end of the file only, if the file already exists then append to it
 *
 * r+ - Read and write from an existing file
 * w+ - Read and write from an existing file, if the file doesn't already exist it is created
 * a+ - Read from or append to an existing file, if the file doesn't already exist it is created TODO
 *
 * as - Create a new file which is stored contiguously on disk (high performance mode/freefile) for append or write
 * ws   If the file is already non-empty or freefile support is not compiled in then it will fall back to non-contiguous
 *      operation.
 *
 * All other mode strings are illegal. In particular, don't add "b" to the end of the mode string.
 *
 * Returns false if the the open failed really early (out of file handles).
 */
bool afatfs_fopen(const char *fileName, const char *mode, afatfsFileCallback_t complete, void *user)
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        return false;
    }
    if (afatfs_request && !afatfs_request->driving && mode && mode[0] != 'r') {
        return false;
    }
    if (!fileName || !mode ||
        (strcmp(mode, "r") && strcmp(mode, "r+") && strcmp(mode, "w") && strcmp(mode, "w+") && strcmp(mode, "a") &&
            strcmp(mode, "a+") && strcmp(mode, "as") && strcmp(mode, "ws") && strcmp(mode, "x") &&
            strcmp(mode, "x+"))) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        if (complete) {
            complete(NULL, user);
        }
        return false;
    }
    uint8_t fileMode = 0;
    afatfsFilePtr_t file;

    switch (mode[0]) {
        case 'r':
            fileMode = AFATFS_FILE_MODE_READ;
            break;
        case 'w':
            fileMode = AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_CREATE;
            break;
        case 'x':
            fileMode = AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_EXCLUSIVE_CREATE;
            break;
        case 'a':
            fileMode = AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CREATE;
            break;
    }

    switch (mode[1]) {
        case '+':
            fileMode |= AFATFS_FILE_MODE_READ;

            if (fileMode == AFATFS_FILE_MODE_READ) {
                fileMode |= AFATFS_FILE_MODE_WRITE;
            }
            break;
        case 's':
#ifdef AFATFS_USE_FREEFILE
            if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT) {
                fileMode |= AFATFS_FILE_MODE_CONTIGUOUS | AFATFS_FILE_MODE_RETAIN_DIRECTORY;
            }
#endif
            break;
    }

    file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFile(file, fileName, FAT_FILE_ATTRIBUTE_ARCHIVE, fileMode, complete, user);
    } else if (complete) {
        complete(NULL, user);
    }

    return file != NULL;
}

/**
 * Write one byte. Return 1 if accepted, -1 if rejected (including a busy cache).
 */
int afatfs_fputc(afatfsFilePtr_t file, uint8_t character)
{
    if (!afatfs_mediaSessionActive()) {
        return -1;
    }
    if (afatfs.readOnly || !file || file->type == AFATFS_FILE_TYPE_NONE ||
        (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY) ||
        !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))) {
        afatfs_fileError(file, AFATFS_ERROR_DENIED);
        return -1;
    }
    if (afatfs_fileIsBusy(file)) {
        afatfs_fileError(file, AFATFS_ERROR_BUSY);
        return -1;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        return afatfs_fwrite(file, &character, 1) == 1 ? 1 : -1;
    }
#endif
    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;

    int cacheIndex = file->writeLockedCacheIndex;

    /* If we've already locked the current sector in the cache, and we won't be completing the sector, we won't
     * be caching/uncaching/seeking, so we can just run this simpler fast case.
     */
    if (cacheIndex != -1 && cursorOffsetInSector != AFATFS_SECTOR_SIZE - 1) {
        afatfs_cacheSectorGetMemory(cacheIndex)[cursorOffsetInSector] = character;
        file->cursorOffset++;
        file->modified = true;
    } else {
        // Slow path
        return afatfs_fwrite(file, &character, sizeof(character)) == 1 ? 1 : -1;
    }
    file->error = AFATFS_ERROR_NONE;
    return 1;
}

/**
 * Attempt to write `len` bytes from `buffer` into the `file`.
 *
 * Returns the number of bytes actually written.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *
 * Fewer bytes will be written than requested when:
 *     The write spanned a sector boundary and the next sector's contents/location was not yet available in the cache.
 *     Or you tried to extend the length of the file but the filesystem is full (check afatfs_isFull()).
 */
uint32_t afatfs_fwrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t len)
{
    if (!afatfs_mediaSessionActive()) {
        return 0;
    }
    if (!file || file->type == AFATFS_FILE_TYPE_NONE || (!buffer && len)) {
        afatfs_fileError(file, AFATFS_ERROR_INVALID_OBJECT);
        return 0;
    }
    if (afatfs.readOnly || (file->attrib & FAT_FILE_ATTRIBUTE_READ_ONLY)) {
        afatfs_fileError(file, AFATFS_ERROR_DENIED);
        return 0;
    }
    if ((file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE)) == 0) {
        afatfs_fileError(file, AFATFS_ERROR_DENIED);
        return 0;
    }

    if (afatfs_fileIsBusy(file)) {
        // There might be a seek pending
        afatfs_fileError(file, AFATFS_ERROR_BUSY);
        return 0;
    }
    file->error = AFATFS_ERROR_NONE;

#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        // Zero a gap before extending ValidDataLength, so old disk contents stay inaccessible.
        while (len && file->validDataLength < file->cursorOffset) {
            uint8_t *zeroSector;
            afatfsOperationStatus_e status;
            uint32_t offset = file->validDataLength;
            if (file->exfatNoFatChain) {
                status =
                    afatfs_cacheSector(afatfs_fileClusterToPhysical(file->firstCluster + offset / afatfs_clusterSize(),
                                           (offset % afatfs_clusterSize()) / AFATFS_SECTOR_SIZE),
                        &zeroSector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            } else {
                status = afatfs_exfatStreamSector(file->firstCluster, offset, &file->zeroCursorCluster,
                    &file->zeroCursorIndex, &zeroSector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE);
            }
            if (status != AFATFS_OPERATION_SUCCESS) {
                return 0;
            }
            uint32_t count = MIN(AFATFS_SECTOR_SIZE - offset % AFATFS_SECTOR_SIZE, file->cursorOffset - offset);
            memset(zeroSector + offset % AFATFS_SECTOR_SIZE, 0, count);
            file->validDataLength += count;
        }
        len = MIN(len, UINT32_MAX - file->cursorOffset);
    }
#endif

    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    uint32_t writtenBytes = 0;

    while (len > 0) {
        uint32_t bytesToWriteThisSector = MIN(AFATFS_SECTOR_SIZE - cursorOffsetInSector, len);
        bool writingFullSector = (bytesToWriteThisSector == AFATFS_SECTOR_SIZE);
        uint8_t *sectorBuffer;

        sectorBuffer = afatfs_fileLockCursorSectorForWrite(file, writingFullSector);
        if (!sectorBuffer) {
            // Cache is currently busy
            break;
        }

        memcpy(sectorBuffer + cursorOffsetInSector, buffer, bytesToWriteThisSector);
        file->modified = true;
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
            file->validDataLength = MAX(file->validDataLength, file->cursorOffset + bytesToWriteThisSector);
            file->dataDirty = true;
        }
#endif

        writtenBytes += bytesToWriteThisSector;

        /*
         * If the seek doesn't complete immediately then we'll break and wait for that seek to complete by waiting for
         * the file to be non-busy on entry again.
         *
         * A seek operation should always be able to queue on the file since we have checked that the file wasn't busy
         * on entry (fseek will never return AFATFS_OPERATION_FAILURE).
         *
         * If the seek has to queue, when the seek completes, it'll update the fileSize for us to contain the cursor.
         */
        if (afatfs_fseekInternal(file, bytesToWriteThisSector, NULL, NULL) == AFATFS_OPERATION_IN_PROGRESS) {
            break;
        }

#ifdef AFATFS_USE_FREEFILE
        if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
            afatfs_assert(file->cursorCluster < afatfs.freeFile.firstCluster);
        }
#endif

        len -= bytesToWriteThisSector;
        buffer += bytesToWriteThisSector;
        cursorOffsetInSector = 0;
    }

    if (!writtenBytes && len) {
        file->error = (uint8_t)(afatfs.filesystemFull ? AFATFS_ERROR_NO_SPACE : AFATFS_ERROR_BUSY);
    }
    return writtenBytes;
}

/**
 * Attempt to read `len` bytes from `file` into the `buffer`.
 *
 * Returns the number of bytes actually read.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *     EOF was reached (check afatfs_feof())
 *
 * Fewer bytes than requested will be read when:
 *     The read spans a AFATFS_SECTOR_SIZE boundary and the following sector was not available in the cache yet.
 */
uint32_t afatfs_fread(afatfsFilePtr_t file, uint8_t *buffer, uint32_t len)
{
    if (!afatfs_mediaSessionActive()) {
        return 0;
    }
    if (!file || file->type == AFATFS_FILE_TYPE_NONE || (!buffer && len)) {
        afatfs_fileError(file, AFATFS_ERROR_INVALID_OBJECT);
        return 0;
    }
    if ((file->mode & AFATFS_FILE_MODE_READ) == 0) {
        afatfs_fileError(file, AFATFS_ERROR_DENIED);
        return 0;
    }

    if (afatfs_fileIsBusy(file)) {
        // There might be a seek pending
        file->error = AFATFS_ERROR_BUSY;
        return 0;
    }
    file->error = AFATFS_ERROR_NONE;

    /*
     * If we've just previously fwritten() to extend the file, the logical filesize will be out of date and the cursor
     * will appear to be beyond the end of the file (but actually it's precisely at the end of the file, because if
     * we had seeked backwards to where we could read something with fseek(), we would have updated the filesize).
     */
    if (file->cursorOffset >= file->logicalSize) {
        return 0;
    }

    len = MIN(file->logicalSize - file->cursorOffset, len);

    uint32_t readBytes = 0;
    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;

    while (len > 0) {
        uint32_t bytesToReadThisSector = MIN(AFATFS_SECTOR_SIZE - cursorOffsetInSector, len);
        uint8_t *sectorBuffer;

        sectorBuffer = afatfs_fileRetainCursorSectorForRead(file);
        if (!sectorBuffer) {
            // Cache is currently busy
            if (!readBytes) {
                file->error = AFATFS_ERROR_BUSY;
            }
            return readBytes;
        }

        memcpy(buffer, sectorBuffer + cursorOffsetInSector, bytesToReadThisSector);
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
            file->validDataLength < file->cursorOffset + bytesToReadThisSector) {
            uint32_t valid =
                file->validDataLength > file->cursorOffset ? file->validDataLength - file->cursorOffset : 0;
            memset(buffer + valid, 0, bytesToReadThisSector - valid);
        }
#endif

        readBytes += bytesToReadThisSector;

        /*
         * If the seek doesn't complete immediately then we'll break and wait for that seek to complete by waiting for
         * the file to be non-busy on entry again.
         *
         * A seek operation should always be able to queue on the file since we have checked that the file wasn't busy
         * on entry (fseek will never return AFATFS_OPERATION_FAILURE).
         */
        afatfsOperationStatus_e seekStatus = afatfs_fseekInternal(file, bytesToReadThisSector, NULL, NULL);
        if (cursorOffsetInSector + bytesToReadThisSector == AFATFS_SECTOR_SIZE) {
            afatfs_fprefetch(file);
        }
        if (seekStatus == AFATFS_OPERATION_IN_PROGRESS) {
            break;
        }

        len -= bytesToReadThisSector;
        buffer += bytesToReadThisSector;
        cursorOffsetInSector = 0;
    }

    return readBytes;
}

/**
 * Returns true if the file's pointer position currently lies at the end-of-file point (i.e. one byte beyond the last
 * byte in the file).
 */
bool afatfs_feof(afatfsFilePtr_t file)
{
    return file->cursorOffset >= file->logicalSize;
}

/**
 * Continue any queued operations on the given file.
 */
static void afatfs_fileOperationContinue(afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE) {
        return;
    }

    switch (file->operation.active) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            afatfs_createFileContinue(file);
            break;
        case AFATFS_FILE_OPERATION_SEEK:
            afatfs_fseekInternalContinue(file);
            break;
        case AFATFS_FILE_OPERATION_CLOSE:
            afatfs_fcloseContinue(file);
            break;
        case AFATFS_FILE_OPERATION_SYNC:
            afatfs_fsyncContinue(file);
            break;
        case AFATFS_FILE_OPERATION_UNLINK:
            afatfs_funlinkContinue(file);
            break;
        case AFATFS_FILE_OPERATION_TRUNCATE:
            if (afatfs_ftruncateContinue(file, false) == AFATFS_OPERATION_FAILURE) {
                afatfsFileCallback_t callback = file->operation.state.truncateFile.callback;
                void *user = file->operation.state.truncateFile.user;
                afatfs_fileError(file, AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                file->operation.active = AFATFS_FILE_OPERATION_NONE;
#ifdef ASYNCFS_ENABLE_EXFAT
                if (afatfs.allocationOwner == file) {
                    afatfs.allocationOwner = NULL;
                }
#endif
                if (callback) {
                    callback(NULL, user);
                }
            }
            break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER:
            afatfs_appendSuperclusterContinue(file);
            break;
        case AFATFS_FILE_OPERATION_LOCKED:;
            break;
#endif
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            afatfs_appendRegularFreeClusterContinue(file);
            break;
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            afatfs_extendSubdirectoryContinue(file);
            break;
        case AFATFS_FILE_OPERATION_NONE:;
            break;
    }
}

/**
 * Check files for pending operations and execute them.
 */
static void afatfs_fileOperationsPoll()
{
    afatfs_fileOperationContinue(&afatfs.currentDirectory);

#ifdef AFATFS_USE_FREEFILE
    // Closing the internal freefile now yields while its writes are synchronized too.
    afatfs_fileOperationContinue(&afatfs.freeFile);
#endif

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    afatfs_fileOperationContinue(&afatfs.introSpecLog);
#endif

    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        afatfs_fileOperationContinue(&afatfs.openFiles[i]);
    }
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Return the available size of the freefile (used for files in contiguous append mode)
 */
uint32_t afatfs_getContiguousFreeSpace()
{
    return afatfs.freeFile.logicalSize;
}

/**
 * Call to set up the initial state for finding the largest block of free space on the device whose corresponding FAT
 * sectors are themselves entirely free space (so the free space has dedicated FAT sectors of its own).
 */
static void afatfs_findLargestContiguousFreeBlockBegin()
{
    // The first FAT sector has two reserved entries, so it isn't eligible for this search. Start at the next FAT sector.
    afatfs.initState.freeSpaceSearch.candidateStart = afatfs_fatEntriesPerSector();
    afatfs.initState.freeSpaceSearch.candidateEnd = afatfs.initState.freeSpaceSearch.candidateStart;
    afatfs.initState.freeSpaceSearch.bestGapStart = 0;
    afatfs.initState.freeSpaceSearch.bestGapLength = 0;
    afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
}

/**
 * Call to continue the search for the largest contiguous block of free space on the device.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - SD card is busy, call again later to resume
 *     AFATFS_OPERATION_SUCCESS - When the search has finished and afatfs.initState.freeSpaceSearch has been updated with the details of the best gap.
 *     AFATFS_OPERATION_FAILURE - When a read error occured
 */
static afatfsOperationStatus_e afatfs_findLargestContiguousFreeBlockContinue()
{
    afatfsFreeSpaceSearch_t *opState = &afatfs.initState.freeSpaceSearch;
    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    uint32_t candidateGapLength;
    uint32_t searchLimit;
    afatfsFindClusterStatus_e searchStatus;

    while (1) {
        switch (opState->phase) {
            case AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE:
                // Find the first free cluster
                switch (afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR,
                    &opState->candidateStart, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                    case AFATFS_FIND_CLUSTER_FOUND:
                        opState->candidateEnd = opState->candidateStart + 1;
                        opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE;
                        break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                        // We finished searching the volume (didn't find any more holes to examine)
                        return AFATFS_OPERATION_SUCCESS;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
                break;
            case AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE:
                // Find the first used cluster after the beginning of the hole (that signals the end of the hole)

                // Don't search beyond the end of the volume, or such that the freefile size would exceed the max filesize
                searchLimit = MIN((uint64_t)opState->candidateStart + FAT_MAXIMUM_FILESIZE / afatfs_clusterSize(),
                    afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                searchStatus =
                    afatfs_findClusterWithCondition(CLUSTER_SEARCH_OCCUPIED, &opState->candidateEnd, searchLimit);

                switch (searchStatus) {
                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                    case AFATFS_FIND_CLUSTER_FOUND:
                        // Either we found a used sector, or the search reached the end of the volume or exceeded the max filesize
                        candidateGapLength = opState->candidateEnd - opState->candidateStart;

                        if (candidateGapLength > opState->bestGapLength) {
                            opState->bestGapStart = opState->candidateStart;
                            opState->bestGapLength = candidateGapLength;
                        }

                        if (searchStatus == AFATFS_FIND_CLUSTER_NOT_FOUND) {
                            // This is the best hole there can be
                            return AFATFS_OPERATION_SUCCESS;
                        } else {
                            // Start a new search for a new hole
                            opState->candidateStart = roundUpTo(opState->candidateEnd + 1, fatEntriesPerSector);
                            opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
                        }
                        break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
                break;
        }
    }
}

static void afatfs_freeFileCreated(afatfsFile_t *file, void *user)
{
    (void)user;
    if (file) {
        // Did the freefile already have allocated space?
        if (file->logicalSize > 0) {
            // We've completed freefile init, move on to the next init phase
            afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_LAST + 1;
        } else {
            // Allocate clusters for the freefile
            afatfs_findLargestContiguousFreeBlockBegin();
            afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_FAT_SEARCH;
        }
    } else {
        // Failed to allocate an entry
        afatfs.lastError = AFATFS_ERROR_GENERIC;
        afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }
}

#endif

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING

static void afatfs_introspecLogCreated(afatfsFile_t *file, void *user)
{
    (void)user;
    if (file) {
        afatfs.initPhase++;
    } else {
        afatfs.lastError = AFATFS_ERROR_GENERIC;
        afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }
}

#endif

#ifdef ASYNCFS_ENABLE_EXFAT
#include "asyncfatfs_journal.inc"
static afatfsOperationStatus_e afatfs_exfatMountContinue(void)
{
    uint8_t *sector;
    afatfsOperationStatus_e status;
    switch (afatfs.exfatMountPhase) {
        case 0: // Validate all eleven boot sectors and the repeated checksum sector.
            while (afatfs.mountOffset < 12) {
                status = afatfs_cacheSector(afatfs.partitionStartSector + afatfs.mountOffset, &sector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (afatfs.mountOffset == 11) {
                    for (unsigned n = 0; n < AFATFS_SECTOR_SIZE; n += 4) {
                        uint32_t checksum = sector[n] | ((uint32_t)sector[n + 1] << 8) |
                            ((uint32_t)sector[n + 2] << 16) | ((uint32_t)sector[n + 3] << 24);
                        if (checksum != afatfs.bootChecksum) {
                            return AFATFS_OPERATION_FAILURE;
                        }
                    }
                } else {
                    if (afatfs.mountOffset >= 1 && afatfs.mountOffset <= 8 &&
                        (sector[508] != 0 || sector[509] != 0 || sector[510] != 0x55 || sector[511] != 0xAA)) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    for (unsigned n = 0; n < AFATFS_SECTOR_SIZE; n++) {
                        if (afatfs.mountOffset == 0 && (n == 106 || n == 107 || n == 112)) {
                            continue;
                        }
                        afatfs.bootChecksum = (afatfs.bootChecksum >> 1 | afatfs.bootChecksum << 31) + sector[n];
                    }
                }
                ++afatfs.mountOffset;
            }
            afatfs_chdir(NULL);
            afatfs_findFirst(&afatfs.currentDirectory, &afatfs.mountFinder);
            afatfs.exfatMountPhase = 1;
            return AFATFS_OPERATION_IN_PROGRESS;
        case 1: // Metadata entries may occur anywhere in a fragmented root directory.
            for (;;) {
                fatDirectoryEntry_t *entry;
                status = afatfs_findNext(&afatfs.currentDirectory, &afatfs.mountFinder, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!entry || entry->fileName[0] == 0) {
                    break;
                }
                if (++afatfs.rootEntriesRead >
                    (uint64_t)afatfs.numClusters * afatfs_clusterSize() / FAT_DIRECTORY_ENTRY_SIZE) {
                    return AFATFS_OPERATION_FAILURE;
                }
                uint8_t type = (uint8_t)entry->fileName[0];
                uint8_t *raw = (uint8_t *)entry;
                afatfsJournal_t *journal = &afatfs.journal;
                if (!journal->found) {
                    afatfsFile_t *fileHandle = &afatfs.journalFile;
                    if (type == EXFAT_DIR_FILEDIR) {
                        journal->foundEntries = raw[1] == 2 ? 1 : 0;
                    } else if (journal->foundEntries && journal->foundEntries < 3) {
                        journal->foundEntries++;
                    }
                    if (journal->foundEntries) {
                        unsigned n = journal->foundEntries - 1;
                        memcpy(fileHandle->exfatEntries + n * FAT_DIRECTORY_ENTRY_SIZE, raw, FAT_DIRECTORY_ENTRY_SIZE);
                        fileHandle->exfatPositions[n] = afatfs.mountFinder;
                        if (journal->foundEntries == 3) {
                            bool match = type == EXFAT_DIR_FILENAME;
                            for (unsigned k = 0; k < sizeof(AFATFS_JOURNAL_NAME) - 1; k++) {
                                unsigned ch = raw[2 + k * 2];
                                if (ch >= 'a' && ch <= 'z') {
                                    ch -= 'a' - 'A';
                                }
                                if (raw[3 + k * 2] || ch != AFATFS_JOURNAL_NAME[k]) {
                                    match = false;
                                }
                            }
                            journal->found = match;
                            journal->foundEntries = 0;
                        }
                    }
                }
                if (type == EXFAT_DIR_BITMAP || type == EXFAT_DIR_UPCASE) {
                    exFatUpTableEntry_t *metadata = (exFatUpTableEntry_t *)entry;
                    if (!afatfs_exfatValidCluster(metadata->firstCluster) || metadata->dataSize > UINT32_MAX) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    if (type == EXFAT_DIR_BITMAP) {
                        if (afatfs.bitmapCluster || raw[1] != 0 ||
                            metadata->dataSize < ((uint64_t)afatfs.numClusters + 7) / 8) {
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs.bitmapCluster = metadata->firstCluster;
                        afatfs.bitmapLength = (uint32_t)metadata->dataSize;
                    } else {
                        if (afatfs.upcaseCluster || metadata->dataSize == 0 || metadata->dataSize > 131072 ||
                            (metadata->dataSize & 1)) {
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs.upcaseCluster = metadata->firstCluster;
                        afatfs.upcaseLength = (uint32_t)metadata->dataSize;
                        afatfs.upcaseChecksum = metadata->tableChecksum;
                    }
                }
                if (journal->found && afatfs.bitmapCluster && afatfs.upcaseCluster) {
                    break;
                }
            }
            afatfs_findLast(&afatfs.currentDirectory);
            if (!afatfs.bitmapCluster || !afatfs.upcaseCluster) {
                return AFATFS_OPERATION_FAILURE;
            }
            afatfs.mountOffset = 0;
            afatfs.mountCluster = 0;
            afatfs.mountClusterIndex = 0;
            afatfs.mountChecksum = 0;
            afatfs.exfatMountPhase = 2;
            return AFATFS_OPERATION_IN_PROGRESS;
        case 2: // Verify and decompress the on-volume up-case table, retaining only the ASCII API range.
            while (afatfs.mountOffset < afatfs.upcaseLength) {
                status = afatfs_exfatStreamSector(afatfs.upcaseCluster, afatfs.mountOffset, &afatfs.mountCluster,
                    &afatfs.mountClusterIndex, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                unsigned n = afatfs.mountOffset % AFATFS_SECTOR_SIZE;
                uint16_t word = sector[n] | ((uint16_t)sector[n + 1] << 8);
                afatfs.mountChecksum = (afatfs.mountChecksum >> 1 | afatfs.mountChecksum << 31) + sector[n];
                afatfs.mountChecksum = (afatfs.mountChecksum >> 1 | afatfs.mountChecksum << 31) + sector[n + 1];
                afatfs.mountOffset += 2;
                if (afatfs.upcaseSkip) {
                    if (!word || afatfs.upcaseCodepoint + word > 65536) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    uint32_t end = afatfs.upcaseCodepoint + word;
                    for (; afatfs.upcaseCodepoint < end && afatfs.upcaseCodepoint < 128; afatfs.upcaseCodepoint++) {
                        afatfs.upcaseAscii[afatfs.upcaseCodepoint] = (uint16_t)afatfs.upcaseCodepoint;
                    }
                    afatfs.upcaseCodepoint = end;
                    afatfs.upcaseSkip = false;
                } else if (word == 0xFFFF && afatfs.upcaseCodepoint != 0xFFFF) {
                    afatfs.upcaseSkip = true;
                } else {
                    if (afatfs.upcaseCodepoint >= 65536) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    if (afatfs.upcaseCodepoint < 128) {
                        afatfs.upcaseAscii[afatfs.upcaseCodepoint] = word;
                    }
                    ++afatfs.upcaseCodepoint;
                }
            }
            if (afatfs.upcaseSkip || afatfs.upcaseCodepoint != 65536 || afatfs.mountChecksum != afatfs.upcaseChecksum) {
                return AFATFS_OPERATION_FAILURE;
            }
            afatfs.exfatMountPhase = 3;
            return AFATFS_OPERATION_IN_PROGRESS;
        case 3:
            return afatfs_journalMount();
    }
    return AFATFS_OPERATION_FAILURE;
}
#endif

static void afatfs_initContinue()
{
#ifdef AFATFS_USE_FREEFILE
    afatfsOperationStatus_e status;
#endif

    uint8_t *sector;

doMore:

    switch (afatfs.initPhase) {
        case AFATFS_INITIALIZATION_READ_MBR:
            if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0) ==
                AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(sector)) {
                    afatfs.initPhase = AFATFS_INITIALIZATION_READ_VOLUME_ID;
#ifdef ASYNCFS_ENABLE_EXFAT
                    if (afatfs.partitionType == MBR_PARTITION_TYPE_GPT) {
                        afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_HEADER;
                    }
#endif
                    goto doMore;
                } else {
                    afatfs.lastError = AFATFS_ERROR_BAD_MBR;
                    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
            break;
#ifdef ASYNCFS_ENABLE_EXFAT
        case AFATFS_INITIALIZATION_READ_GPT_HEADER:
        case AFATFS_INITIALIZATION_READ_GPT_ARRAY:
        case AFATFS_INITIALIZATION_READ_GPT_ENTRY:
        case AFATFS_INITIALIZATION_READ_GPT_VOLUME:
        case AFATFS_INITIALIZATION_CHECK_GPT_OVERLAP:
            if (afatfs_gptContinue() == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BAD_MBR;
                afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
            break;
#endif
        case AFATFS_INITIALIZATION_READ_VOLUME_ID:
            if (afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE,
                    0) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseVolumeID(sector)) {
                    afatfs.initPhase++;
                } else {
                    afatfs.lastError = AFATFS_ERROR_BAD_FILESYSTEM_HEADER;
                    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
            break;
        case AFATFS_INITIALIZATION_READ_ROOT:
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                afatfsOperationStatus_e mountStatus = afatfs_exfatMountContinue();
                if (mountStatus == AFATFS_OPERATION_FAILURE) {
                    afatfs.lastError = AFATFS_ERROR_BAD_FILESYSTEM_HEADER;
                    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                } else if (mountStatus == AFATFS_OPERATION_SUCCESS) {
                    afatfs.initPhase++;
#ifdef AFATFS_USE_FREEFILE
                    // The reservation-file optimisation is specific to FAT16/FAT32.
                    afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_LAST + 1;
#endif
                    if (afatfs.readOnly) {
                        afatfs.initPhase = AFATFS_INITIALIZATION_DONE;
                    }
                }
                break;
            }
#endif
            afatfs_chdir(NULL);
            afatfs.initPhase++;
            break;

#ifdef AFATFS_USE_FREEFILE
        case AFATFS_INITIALIZATION_FREEFILE_CREATE:
            afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_CREATING;

            afatfs_createFile(&afatfs.freeFile, AFATFS_FREESPACE_FILENAME,
                FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY | FAT_FILE_ATTRIBUTE_HIDDEN,
                AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_RETAIN_DIRECTORY, afatfs_freeFileCreated, NULL);
            break;
        case AFATFS_INITIALIZATION_FREEFILE_CREATING:
            afatfs_fileOperationContinue(&afatfs.freeFile);
            break;
        case AFATFS_INITIALIZATION_FREEFILE_FAT_SEARCH:
            if (afatfs_findLargestContiguousFreeBlockContinue() == AFATFS_OPERATION_SUCCESS) {
                // If the freefile ends up being empty then we only have to save its directory entry:
                afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY;

                if (afatfs.initState.freeSpaceSearch.bestGapLength > AFATFS_FREEFILE_LEAVE_CLUSTERS + 1) {
                    afatfs.initState.freeSpaceSearch.bestGapLength -= AFATFS_FREEFILE_LEAVE_CLUSTERS;

                    /* So that the freefile never becomes empty, we want it to occupy a non-integer number of
                     * superclusters. So its size mod the number of clusters in a supercluster should be 1.
                     */
                    afatfs.initState.freeSpaceSearch.bestGapLength =
                        ((afatfs.initState.freeSpaceSearch.bestGapLength - 1) & ~(afatfs_fatEntriesPerSector() - 1)) +
                        1;

                    // Anything useful left over?
                    if (afatfs.initState.freeSpaceSearch.bestGapLength > afatfs_fatEntriesPerSector()) {
                        uint32_t startCluster = afatfs.initState.freeSpaceSearch.bestGapStart;
                        // Points 1-beyond the final cluster of the freefile:
                        uint32_t endCluster = afatfs.initState.freeSpaceSearch.bestGapStart +
                            afatfs.initState.freeSpaceSearch.bestGapLength;

                        afatfs_assert(endCluster < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                        afatfs.initState.freeSpaceFAT.startCluster = startCluster;
                        afatfs.initState.freeSpaceFAT.endCluster = endCluster;

                        afatfs.freeFile.firstCluster = startCluster;

                        afatfs.freeFile.logicalSize =
                            afatfs.initState.freeSpaceSearch.bestGapLength * afatfs_clusterSize();
                        afatfs.freeFile.physicalSize = afatfs.freeFile.logicalSize;

                        // We can write the FAT table for the freefile now
                        afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_UPDATE_FAT;
                    } // Else the freefile's FAT chain and filesize remains the default (empty)
                }

                goto doMore;
            }
            break;
        case AFATFS_INITIALIZATION_FREEFILE_UPDATE_FAT:
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_TERMINATED_CHAIN,
                &afatfs.initState.freeSpaceFAT.startCluster, afatfs.initState.freeSpaceFAT.endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase = AFATFS_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY;

                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_GENERIC;
                afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
            break;
        case AFATFS_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY:
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile, AFATFS_SAVE_DIRECTORY_NORMAL);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase++;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_GENERIC;
                afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
            break;
#endif

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
        case AFATFS_INITIALIZATION_INTROSPEC_LOG_CREATE:
            afatfs.initPhase = AFATFS_INITIALIZATION_INTROSPEC_LOG_CREATING;

            afatfs_createFile(&afatfs.introSpecLog, AFATFS_INTROSPEC_LOG_FILENAME, FAT_FILE_ATTRIBUTE_ARCHIVE,
                AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_APPEND, afatfs_introspecLogCreated, NULL);
            break;
        case AFATFS_INITIALIZATION_INTROSPEC_LOG_CREATING:
            afatfs_fileOperationContinue(&afatfs.introSpecLog);
            break;
#endif

        case AFATFS_INITIALIZATION_DONE:
            afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_READY;
            break;
    }
}

/**
 * Check to see if there are any pending operations on the filesystem and perform a little work (without waiting on the
 * sdcard). You must call this periodically.
 */
void afatfs_poll()
{
    if (afatfs_request && afatfs_request->operation >= AFATFS_API_MKFS) {
        if (sdcard_poll()) {
            afatfs_apiPoll();
        }
        return;
    }
    if (!afatfs_mediaSessionActive()) {
        if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPING) {
            sdcard_poll();
        }
        return;
    }
    // Only attempt to continue FS operations if the card is present & ready, otherwise we would just be wasting time
    if (sdcard_poll()) {
#ifdef ASYNCFS_ENABLE_EXFAT
        afatfs_journalPoll();
#endif
        afatfs_flushCache();

        switch (afatfs.fileSystemState) {
            case AFATFS_FILESYSTEM_STATE_INITIALIZATION:
                afatfs_initContinue();
                break;
            case AFATFS_FILESYSTEM_STATE_READY:
            case AFATFS_FILESYSTEM_STATE_CLOSING:
#ifdef ASYNCFS_ENABLE_EXFAT
                if (afatfs_journalUnresolvedError()) {
                    break;
                }
#endif
                afatfs_fileOperationsPoll();
                afatfs_apiPoll();
                break;
            default:;
        }
    }
}

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING

void afatfs_sdcardProfilerCallback(sdcardBlockOperation_e operation, uint32_t blockIndex, uint32_t duration)
{
    // Make sure the log file has actually been opened before we try to log to it:
    if (afatfs.introSpecLog.type == AFATFS_FILE_TYPE_NONE) {
        return;
    }

    enum {
        LOG_ENTRY_SIZE = 16 // Log entry size should be a power of two to avoid partial fwrites()
    };

    uint8_t buffer[LOG_ENTRY_SIZE];

    buffer[0] = operation;

    // Padding/reserved:
    buffer[1] = 0;
    buffer[2] = 0;
    buffer[3] = 0;

    buffer[4] = blockIndex & 0xFF;
    buffer[5] = (blockIndex >> 8) & 0xFF;
    buffer[6] = (blockIndex >> 16) & 0xFF;
    buffer[7] = (blockIndex >> 24) & 0xFF;

    buffer[8] = duration & 0xFF;
    buffer[9] = (duration >> 8) & 0xFF;
    buffer[10] = (duration >> 16) & 0xFF;
    buffer[11] = (duration >> 24) & 0xFF;

    // Padding/reserved:
    buffer[12] = 0;
    buffer[13] = 0;
    buffer[14] = 0;
    buffer[15] = 0;

    // Ignore write failures
    afatfs_fwrite(&afatfs.introSpecLog, buffer, LOG_ENTRY_SIZE);
}

#endif

afatfsFilesystemState_e afatfs_getFilesystemState()
{
    return afatfs.fileSystemState;
}

afatfsError_e afatfs_getLastError()
{
    return afatfs.lastError;
}

static bool afatfs_fileHasPendingWork(afatfsFilePtr_t file)
{
#ifdef ASYNCFS_ENABLE_LFN
    if (file->type != AFATFS_FILE_TYPE_NONE && file->lfnOutput) {
        return true;
    }
#endif
    if (afatfs_fileIsBusy(file)) {
        return true;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (file->type != AFATFS_FILE_TYPE_NONE && file->dataDirty) {
        return true;
    }
#endif
    return false;
}

bool afatfs_isIdle(void)
{
    if (afatfs_request) {
        return false;
    }
    for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next) {
        if (dir->reading) {
            return false;
        }
    }
    if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED) {
        return true;
    }
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY || afatfs.cacheDirtyEntries ||
        afatfs.cacheFlushInProgress || afatfs.cwdWalkMutex) {
        return false;
    }

    if (afatfs_fileHasPendingWork(&afatfs.currentDirectory)) {
        return false;
    }
    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        if (afatfs_fileHasPendingWork(&afatfs.openFiles[i])) {
            return false;
        }
    }
#ifdef AFATFS_USE_FREEFILE
    if (afatfs_fileHasPendingWork(&afatfs.freeFile)) {
        return false;
    }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (afatfs_fileHasPendingWork(&afatfs.introSpecLog)) {
        return false;
    }
#endif
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        const afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[i];
        if (descriptor->locked ||
            (descriptor->state != AFATFS_CACHE_STATE_EMPTY && descriptor->state != AFATFS_CACHE_STATE_IN_SYNC)) {
            return false;
        }
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        const afatfsJournal_t *journal = &afatfs.journal;
        if (afatfs.allocationOwner || afatfs_fileHasPendingWork(&afatfs.journalFile) || journal->ioPending ||
            journal->ioDone || journal->ioFailed || journal->active || journal->provision || journal->headerPrepared ||
            journal->blockAllocation || journal->phase != AFATFS_JOURNAL_IDLE) {
            return false;
        }
    }
#endif
    return true;
}

bool afatfs_canPowerOff(void)
{
    return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED;
}

void afatfs_init()
{
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_INITIALIZATION);
    afatfs.initPhase = AFATFS_INITIALIZATION_READ_MBR;
    afatfs.lastClusterAllocated = FAT_SMALLEST_LEGAL_CLUSTER_NUMBER;
    afatfs.cwdWalkMutex = NULL;

    // Make sure the cache descriptors are invalidated
    memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
    memset(afatfs.openFiles, 0, sizeof(afatfs.openFiles));

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    sdcard_setProfilerCallback(afatfs_sdcardProfilerCallback);
#endif
}

void afatfs_notifyMediaRemoved(void)
{
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_REMOVED);
    afatfs.readOnly = true;
    afatfs.lastError = AFATFS_ERROR_MEDIA_REMOVED;
}

// Allocation changes invalidate FAT32 FSInfo hints. Write "unknown", rather than
// leaving a believable stale count for the next driver. The closed first handle
// supplies the two-sector scratch state; no counter or sector buffer is allocated.
static bool afatfs_syncFsInfo(void)
{
    if (!afatfs.fsInfoDirty || afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 || afatfs.readOnly) {
        return true;
    }
    afatfsTruncateFile_t *scratch = &afatfs.openFiles[0].operation.state.truncateFile;
    if (afatfs.initPhase == AFATFS_INITIALIZATION_DONE) {
        uint8_t *buffer;
        if (afatfs_cacheSector(afatfs.partitionStartSector, &buffer, AFATFS_CACHE_READ, 0) !=
            AFATFS_OPERATION_SUCCESS) {
            return false;
        }
        const fatVolumeID_t *volume = (const fatVolumeID_t *)buffer;
        uint32_t primary = volume->fat.fatDescriptor.fat32.fsInfo;
        uint32_t backup = volume->fat.fatDescriptor.fat32.backupBootSector;
        scratch->startCluster = scratch->endCluster = 0;
        if (primary && primary < volume->fat.reservedSectorCount && primary < afatfs.partitionSectors) {
            scratch->startCluster = primary;
            if (backup && backup != 0xFFFF && backup + primary < volume->fat.reservedSectorCount &&
                backup + primary < afatfs.partitionSectors) {
                scratch->endCluster = backup + primary;
            }
        }
        afatfs.initPhase = AFATFS_SHUTDOWN_FSINFO_PRIMARY;
    }
    while (afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_PRIMARY || afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_BACKUP) {
        uint32_t relative =
            afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_PRIMARY ? scratch->startCluster : scratch->endCluster;
        if (relative && (uint64_t)afatfs.partitionStartSector + relative <= UINT32_MAX) {
            uint8_t *page;
            if (afatfs_cacheSector(afatfs.partitionStartSector + relative, &page, AFATFS_CACHE_READ, 0) !=
                AFATFS_OPERATION_SUCCESS) {
                return false;
            }
            if (!memcmp(page, "RRaA", 4) && !memcmp(page + 484, "rrAa", 4) && !memcmp(page + 508, "\0\0\x55\xAA", 4)) {
                if (afatfs_cacheSector(afatfs.partitionStartSector + relative, &page,
                        AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0) != AFATFS_OPERATION_SUCCESS) {
                    return false;
                }
                memset(page + 488, 0xFF, 8);
            }
        }
        afatfs.initPhase++;
    }
    if (!afatfs_flushCache()) {
        return false;
    }
    afatfs.fsInfoDirty = false;
    afatfs.initPhase = AFATFS_INITIALIZATION_DONE;
    return true;
}

/**
 * Shut down the filesystem, flushing all data to the disk. Keep calling until it returns true.
 *
 * dirty - Set to true to skip the flush operation and terminate immediately (buffered data will be lost!)
 */
bool afatfs_destroy(bool dirty)
{
    if (!dirty && afatfs_request) {
        afatfs_poll();
        return false;
    }
    bool cleanShutdown = !dirty &&
        (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_READY ||
            afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED ||
            afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_CLOSING ||
            afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPING);
    // Only a known mounted/closing/stopped instance may complete clean shutdown.
    // Preserve buffers after initialization/fatal errors; forced discard is explicit.
    if (!dirty && !cleanShutdown) {
        if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION) {
            afatfs_poll();
        }
        return false;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (!dirty && afatfs_journalUnresolvedError()) {
        return false;
    }
#endif
    // Only attempt detailed cleanup if the filesystem is in reasonable looking state
    if (!dirty &&
        (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_READY ||
            afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_CLOSING)) {
        afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_CLOSING;
        int openFileCount = 0;

        for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
            if (afatfs.openFiles[i].type != AFATFS_FILE_TYPE_NONE) {
                afatfs_fclose(&afatfs.openFiles[i], NULL, NULL);
                // The close operation might not finish right away, so count this file as still open for now
                openFileCount++;
            }
        }

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
        if (afatfs.introSpecLog.type != AFATFS_FILE_TYPE_NONE) {
            afatfs_fclose(&afatfs.introSpecLog, NULL, NULL);
            openFileCount++;
        }
#endif

#ifdef AFATFS_USE_FREEFILE
        if (afatfs.freeFile.type != AFATFS_FILE_TYPE_NONE) {
            afatfs_fclose(&afatfs.freeFile, NULL, NULL);
            openFileCount++;
        }
#endif

        if (afatfs.currentDirectory.type != AFATFS_FILE_TYPE_NONE) {
            afatfs_fclose(&afatfs.currentDirectory, NULL, NULL);
            openFileCount++;
        }

        afatfs_poll();

        if (!afatfs_flushCache()) {
            return false;
        }

        if (afatfs.cacheFlushInProgress) {
            return false;
        }

        // A speculative read owns its cache buffer until the backend callback.
        // Do not reset that buffer or permit card power-off while it is in flight.
        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING ||
                afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_WRITING) {
                return false;
            }
        }

        if (openFileCount > 0) {
            return false;
        }
        if (!afatfs_syncFsInfo()) {
            return false;
        }

#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && !afatfs.readOnly) {
            if (!afatfs_journalCommitted()) {
                return false;
            }
            if (!afatfs.exfatClean) {
                // Retain journal ownership until the clean boot marker is durable.
                // A cut here can recover from a valid, owned, empty journal. An
                // unowned dirty volume remains ambiguous with an external writer.
                uint8_t *sector;
                if (afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE,
                        0) != AFATFS_OPERATION_SUCCESS) {
                    return false;
                }
                sector[106] &= (uint8_t)~2;
                afatfs.exfatClean = true;
                return false; // The cache barrier on the next call waits for the marker's ACK.
            }
            if (afatfs.journal.ready && afatfs.journal.owned) {
                afatfs.journal.owned = false;
                afatfs.journal.phase = AFATFS_JOURNAL_COMMIT;
                return false;
            }
            if (afatfs.journal.phase != AFATFS_JOURNAL_IDLE) {
                return false;
            }
        }
#endif

#ifdef AFATFS_DEBUG
        /* All sector locks should have been released by closing the files, so the subsequent flush should have written
         * all dirty pages to disk. If not, something's wrong:
         */
        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            afatfs_assert(afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_DIRTY);
        }
#endif
        afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_STOPPING;
    }

    if (!dirty && afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPING) {
        if (!sdcard_poll()) {
            return false;
        }
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
        sdcardOperationStatus_e status = sdcard_endWriteBlocks();
        if (status != SDCARD_OPERATION_SUCCESS) {
            if (status == SDCARD_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            }
            return false;
        }
#endif
        if (!sdcard_poll()) {
            return false;
        }
    }

    // Clear the afatfs so it's as if we never ran
    afatfs_resetMediaSession(cleanShutdown ? AFATFS_FILESYSTEM_STATE_STOPPED : AFATFS_FILESYSTEM_STATE_UNKNOWN);

    return true;
}

static bool afatfs_bufferWritableFile(afatfsFilePtr_t file)
{
    return file && file->type == AFATFS_FILE_TYPE_NORMAL && !afatfs_fileIsBusy(file) &&
        (file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE));
}

static bool afatfs_bufferReusableSector(const afatfsCacheBlockDescriptor_t *descriptor)
{
    return descriptor->state == AFATFS_CACHE_STATE_EMPTY ||
        (descriptor->state == AFATFS_CACHE_STATE_IN_SYNC && !descriptor->locked && !descriptor->retainCount);
}

static uint32_t afatfs_bufferFileTail(afatfsFilePtr_t file, int cacheIndex)
{
    return afatfs_bufferWritableFile(file) && file->writeLockedCacheIndex == cacheIndex
        ? AFATFS_SECTOR_SIZE - file->cursorOffset % AFATFS_SECTOR_SIZE
        : 0;
}

/**
 * Shared cache capacity plus this file's own writable sector tail. No I/O or allocation.
 */
uint32_t afatfs_getFileFreeBufferSpace(afatfsFilePtr_t file)
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY || afatfs.readOnly ||
        !afatfs_bufferWritableFile(file)) {
        return 0;
    }

    uint32_t result = 0;
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        const afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[i];
        if (afatfs_bufferReusableSector(descriptor)) {
            result += AFATFS_SECTOR_SIZE;
        } else if (descriptor->state == AFATFS_CACHE_STATE_DIRTY && descriptor->locked) {
            result += afatfs_bufferFileTail(file, i);
        }
    }
    return result;
}

/**
 * Count reusable cache sectors and writable tails already owned by open files.
 * Each cache sector is counted once; no additional bookkeeping storage is needed.
 */
uint32_t afatfs_getFreeBufferSpace()
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY || afatfs.readOnly) {
        return 0;
    }

    uint32_t result = 0;
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        const afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[i];
        if (afatfs_bufferReusableSector(descriptor)) {
            result += AFATFS_SECTOR_SIZE;
        } else if (descriptor->state == AFATFS_CACHE_STATE_DIRTY && descriptor->locked) {
            uint32_t available = 0;
            for (int n = 0; n < AFATFS_MAX_OPEN_FILES; n++) {
                available = MAX(available, afatfs_bufferFileTail(&afatfs.openFiles[n], i));
            }
            result += available;
        }
    }
    return result;
}

#include "asyncfatfs_api.inc"
