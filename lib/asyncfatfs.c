/**
 * @file asyncfatfs.c
 * @brief Asynchronous FAT filesystem, cache and optional feature implementations.
 *
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

#include "asyncfatfs_config.h"

#if !ASYNCFS_ENABLE_FATFS_API && !defined(ASYNCFS_ENABLE_LFN) && !defined(ASYNCFS_ENABLE_EXFAT) &&                     \
    !defined(AFATFS_USE_FREEFILE) && !defined(AFATFS_USE_INTROSPECTIVE_LOGGING) &&                                     \
    !defined(AFATFS_ENABLE_FAT_JOURNAL) && !defined(AFATFS_ENABLE_JOURNAL) &&                                          \
    !defined(AFATFS_DISABLE_ALLOCATION_PIPELINE)
// FAT32 cluster numbers use 28 bits. The hint's high bit tracks one owned,
// unlinked look-ahead EOC; no on-disk field or additional runtime storage is used.
#define AFATFS_PIPELINE_ALLOCATION
#define AFATFS_RESERVED_CLUSTER 0x80000000U
#define AFATFS_CLUSTER_HINT_MASK 0x0FFFFFFFU
#endif

// Entry-sized mirroring benefits the multiblock read profiles. Keep the original
// acquisition ABI and full-page copy in the single-block profile: passing a
// separate image-state output regresses its measured read/write hot paths.
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
#define AFATFS_FAT_MIRROR_STATE(parameter) parameter,
#else
#define AFATFS_FAT_MIRROR_STATE(parameter)
#endif

#if defined(ASYNCFS_ENABLE_LFN) && !defined(AFATFS_DISABLE_FAT_JOURNAL)
#define AFATFS_ENABLE_FAT_JOURNAL
#endif

#if defined(ASYNCFS_ENABLE_EXFAT) || defined(AFATFS_ENABLE_FAT_JOURNAL)
#define AFATFS_ENABLE_JOURNAL
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
#define AFATFS_INLINE __forceinline
#else
#define AFATFS_WEAK __attribute__((weak))
#define AFATFS_ALIGNED __attribute__((aligned(4)))
#define AFATFS_INLINE __attribute__((always_inline)) inline
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

// Allocate the cache in DMA-safe memory outside the struct
#if !defined(ENABLE_AFATFS_DMA_CACHE)
#define ENABLE_AFATFS_DMA_CACHE 0
#endif

#define MIN(firstValue, secondValue) ((firstValue) < (secondValue) ? (firstValue) : (secondValue))
#define MAX(firstValue, secondValue) ((firstValue) > (secondValue) ? (firstValue) : (secondValue))

#ifdef ASYNCFS_ENABLE_LBA64
typedef uint64_t LBA_t;
#else
typedef uint32_t LBA_t;
#endif

#ifdef AFATFS_ENABLE_JOURNAL
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
#ifdef AFATFS_ENABLE_JOURNAL
    uint32_t journalEpoch;
#endif

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
#if defined(ASYNCFS_ENABLE_LFN) || defined(ASYNCFS_ENABLE_EXFAT)
    // FAT alias/reservation state and exFAT entry counters belong to different filesystems.
    union {
#ifdef ASYNCFS_ENABLE_LFN
        uint32_t aliasNumber;
        uint32_t orphanSector;
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
        struct {
            uint8_t extraEntryCount;
            uint8_t entryCount;
            uint8_t entriesRead;
        } exfat;
#endif
    } location;
#ifdef ASYNCFS_ENABLE_LFN
    // Parsing finishes before reserving and publishing the new entry set.
    union {
        afatfsLongNameState_t lfn;
        struct {
            uint8_t freeEntries;
            uint8_t writeIndex;
        } counters;
    } scan;
#endif

    uint8_t fileName[FAT_LONG_FILENAME_LENGTH];
#else
    uint8_t fileName[FAT_FILENAME_LENGTH];
#endif
    uint8_t phase;
#ifdef ASYNCFS_ENABLE_LFN
    uint8_t shortName[FAT_FILENAME_LENGTH];
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
    AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE,
#ifdef AFATFS_PIPELINE_ALLOCATION
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_MARK_DURABLE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT,
#endif
} afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeCluster_t {
    LBA_t previousCluster;
    LBA_t searchCluster;
    // Chain conversion finishes before directory zeroing begins.
    union {
        uint32_t convertCluster;
        uint32_t zeroSector;
    } progress;
    afatfsAppendFreeClusterPhase_e phase;
    bool wrapped : 1;
#ifdef AFATFS_PIPELINE_ALLOCATION
    bool collecting : 1;
    unsigned char collectIdle : 6;
#endif
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

    // Tail search, ordinary chain erasure and contiguous return run in separate phases.
    union {
        LBA_t startCluster;
        uint32_t nextCluster;
        uint32_t clustersToKeep;
    } traversal;
    LBA_t currentCluster; // Used to mark progress
    LBA_t endCluster; // Optional, for contiguous files set to 1 past the end cluster of the file, otherwise set to 0
    uint32_t targetSize;
    uint32_t keepCluster;
    uint32_t previousCluster;
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

#ifdef AFATFS_PIPELINE_ALLOCATION
typedef struct afatfsCollectSync_t {
    afatfsAppendFreeCluster_t appendFreeCluster;
    union {
        afatfsCallback_t close;
        afatfsFileCallback_t sync;
    } callback;
    void *user;
} afatfsCollectSync_t;
typedef char afatfsCollectSyncFits[(sizeof(afatfsCollectSync_t) <= sizeof(afatfsUnlinkFile_t)) ? 1 : -1];
#endif

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
#ifdef AFATFS_PIPELINE_ALLOCATION
    AFATFS_FILE_OPERATION_COLLECT_SYNC,
    AFATFS_FILE_OPERATION_COLLECT_CLOSE,
#endif
} afatfsFileOperation_e;

#ifdef AFATFS_PIPELINE_ALLOCATION
typedef struct afatfsBatchRecovery_t {
    uint32_t windowStart;
    uint32_t tableSector;
    uint32_t candidate;
    uint32_t resumeChild;
    uint32_t hops;
    uint32_t budget;
    uint8_t phase;
    uint8_t bitmapIndex;
    uint8_t tableCopy;
    uint8_t directoryEntry;
    uint8_t nextPhase;
    bool windowAllocated : 1;
    bool windowChanged : 1;
    bool passChanged : 1;
} afatfsBatchRecovery_t;
typedef char afatfsBatchRecoveryFits[(sizeof(afatfsBatchRecovery_t) <= sizeof(afatfsTruncateFile_t)) ? 1 : -1];
#endif

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
#ifdef AFATFS_PIPELINE_ALLOCATION
        afatfsBatchRecovery_t batchRecovery;
        afatfsCollectSync_t collectSync;
#endif
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
#elif defined(ASYNCFS_ENABLE_LFN)
    bool modified : 1;
    bool lfnAscii : 1;
#else
#ifdef AFATFS_PIPELINE_ALLOCATION
    bool modified : 1;
    bool directoryExact : 1;
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
#ifdef AFATFS_PIPELINE_ALLOCATION
    AFATFS_INITIALIZATION_BATCH_RECOVERY,
#endif
#ifdef AFATFS_ENABLE_FAT_JOURNAL
    AFATFS_INITIALIZATION_FAT_JOURNAL,
#endif

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

typedef struct afatfsExfatMount_t {
    uint32_t upcaseCluster;
    uint32_t upcaseLength;
    uint32_t upcaseChecksum;
    // Boot validation, root scanning and up-case decoding run sequentially.
    union {
        struct {
            uint32_t offset;
            uint32_t checksum;
        } boot;
        struct {
            afatfsFinder_t finder;
            uint32_t entriesRead;
        } directory;
        struct {
            uint32_t offset;
            uint32_t checksum;
            uint32_t cluster;
            uint32_t clusterIndex;
            uint32_t codepoint;
        } upcase;
    } progress;
} afatfsExfatMount_t;
// Internal partition marker; never interpreted as an on-disk MBR partition type.
#define AFATFS_PARTITION_EXFAT_READ_ONLY (MBR_PARTITION_TYPE_EXFAT | 0x80)
#endif

typedef struct afatfs_t {
#if ENABLE_AFATFS_DMA_CACHE
    uint8_t *cache;
#else
    uint8_t cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
#endif
#ifdef AFATFS_ENABLE_JOURNAL
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
    // The createFile operation has to walk the CWD, but there can only be one active walker at a time.
    // createFile must claim this mutex before beginning a directory walk (findFirst) on the global CWD.
    afatfsFile_t *cwdWalkMutex;
#ifdef ASYNCFS_ENABLE_EXFAT
    afatfsFile_t *allocationOwner;
#endif
    afatfsCacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_SECTORS];
#if defined(AFATFS_USE_FREEFILE) || defined(ASYNCFS_ENABLE_EXFAT)
    // Used during initialization; only one member is live at a time.
    union {
#ifdef AFATFS_USE_FREEFILE
        afatfsFreeSpaceSearch_t freeSpaceSearch;
        afatfsFreeSpaceFAT_t freeSpaceFAT;
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
        afatfsExfatMount_t exfat;
#endif
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
#ifdef AFATFS_PIPELINE_ALLOCATION
    bool batchDirtyMarked : 1;
    bool batchMarkingClean : 1;
    bool batchCollectedSession : 1;
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
    bool upcaseSkip : 1;
    bool exfatClean : 1;
#endif
} afatfs_t;

#if ENABLE_AFATFS_DMA_CACHE
static AFATFS_ALIGNED uint8_t afatfs_cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
#endif
static afatfs_t afatfs;
static uint32_t afatfs_mediaGeneration; // Survives mount-state resets; no per-handle fields.
#if ASYNCFS_ENABLE_FATFS_API
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
static void afatfs_apiFormat(afatfsRequest_t *request);
static AFATFS_INLINE void afatfs_apiResetSession(afatfsFilesystemState_e state);
static AFATFS_INLINE bool afatfs_apiIsIdle(void);
static AFATFS_INLINE bool afatfs_apiShutdownReady(bool dirty);
static AFATFS_INLINE bool afatfs_apiPollFormat(void);
#else
#define afatfs_apiResetSession(state) ((void)0)
#define afatfs_apiIsIdle() true
#define afatfs_apiShutdownReady(dirty) true
#define afatfs_apiPollFormat() false
#endif

/* Forward declarations for the core and feature sections below. */
#ifdef AFATFS_PIPELINE_ALLOCATION
static uint8_t *afatfs_cleanFatMirror(LBA_t physicalSectorIndex);
static afatfsOperationStatus_e afatfs_releaseReservedCluster(void);
static void afatfs_releaseIdleReservation(void);
static void afatfs_sealCollections(void);
static bool afatfs_collectWritable(const afatfsFile_t *file);
static bool afatfs_collectFatPage(LBA_t sector);
#endif

static void afatfs_fileOperationContinue(afatfsFile_t *file);
static void afatfs_fileUnlockCacheSector(afatfsFilePtr_t file);
static bool afatfs_truncateTo(afatfsFilePtr_t file, uint32_t size, afatfsFileCallback_t callback, void *user);
static uint8_t *afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file, bool isFullSector);
static uint8_t *afatfs_fileRetainCursorSectorForRead(afatfsFilePtr_t file);

#ifdef ASYNCFS_ENABLE_LFN
static afatfsOperationStatus_e afatfs_lfnDelete(afatfsFilePtr_t file);
static unsigned afatfs_lfnUpper(unsigned character);
static bool afatfs_lfnValidName(const char *name);
static void afatfs_lfnConsume(afatfsLongNameState_t *state, const uint8_t *entry, const char *wanted, char *output);
static bool afatfs_lfnAttached(const afatfsLongNameState_t *state, const uint8_t *entry);
static bool afatfs_lfnShortName(const char *name, uint8_t *output);
static void afatfs_lfnAlias(afatfsCreateFile_t *state);
#if ASYNCFS_ENABLE_FATFS_API
static afatfsOperationStatus_e afatfs_lfnFileName(afatfsFilePtr_t file, afatfsFileInfo_t *info);
#endif
static afatfsOperationStatus_e afatfs_createLfnContinue(afatfsFilePtr_t file);
static AFATFS_INLINE bool afatfs_lfnFileHasPendingWork(afatfsFilePtr_t file);
static AFATFS_INLINE bool afatfs_lfnDeletionPending(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode);
static AFATFS_INLINE bool afatfs_lfnBeginDeletion(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode);
#else
#define afatfs_lfnFileHasPendingWork(file) false
#define afatfs_lfnDeletionPending(file, mode) false
#define afatfs_lfnBeginDeletion(file, mode) false
#define afatfs_lfnDelete(file) AFATFS_OPERATION_SUCCESS
#endif

#ifdef ASYNCFS_ENABLE_EXFAT
static afatfsOperationStatus_e afatfs_gptContinue(void);
static bool afatfs_exfatValidCluster(uint32_t cluster);
static afatfsOperationStatus_e afatfs_exfatStreamSector(uint32_t firstCluster, uint32_t offset, uint32_t *cursorCluster,
    uint32_t *cursorIndex, uint8_t **sector, uint8_t flags);
static afatfsOperationStatus_e afatfs_exfatBitmap(uint32_t cluster, bool write, bool allocated, bool *value);
static afatfsOperationStatus_e afatfs_exfatWriteEntrySet(afatfsFilePtr_t file);
static int afatfs_exfatLoadEntrySet(afatfsFilePtr_t file, const char *name);
static afatfsOperationStatus_e afatfs_createExFatContinue(afatfsFilePtr_t file);
static void afatfs_closeAfterEmptyTruncate(afatfsFilePtr_t file, void *user);
static afatfsOperationStatus_e afatfs_exfatMountContinue(void);
static afatfsOperationStatus_e afatfs_journalMount(void);
static AFATFS_INLINE bool afatfs_exfatFileHasPendingWork(afatfsFilePtr_t file);
static AFATFS_INLINE bool afatfs_exfatIsIdle(void);
static AFATFS_INLINE bool afatfs_exfatPrepareTruncate(afatfsFilePtr_t file);
static AFATFS_INLINE bool afatfs_exfatActive(void);
static AFATFS_INLINE void afatfs_exfatFinishTruncate(afatfsFilePtr_t file, uint32_t targetSize);
static AFATFS_INLINE bool afatfs_exfatLinearAllocation(afatfsFilePtr_t file);
static AFATFS_INLINE void afatfs_exfatConfigureTruncate(afatfsFilePtr_t file);
static AFATFS_INLINE bool afatfs_exfatProtectivePartition(uint8_t partitionType);
static AFATFS_INLINE bool afatfs_exfatSelectGPT(const mbrPartitionEntry_t *entry);
static AFATFS_INLINE bool afatfs_exfatLegacyPartition(uint8_t partitionType);
static AFATFS_INLINE bool afatfs_exfatVolumeSelected(void);
static AFATFS_INLINE bool afatfs_exfatParseVolumeID(const uint8_t *sector);
static AFATFS_INLINE void afatfs_exfatSelectMountPhase(void);
static AFATFS_INLINE bool afatfs_exfatContinueGPT(void);
static AFATFS_INLINE void afatfs_exfatMountRoot(void);
static AFATFS_INLINE afatfsOperationStatus_e afatfs_exfatSaveDirectoryEntry(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode);
static AFATFS_INLINE void afatfs_exfatCompleteSync(afatfsFilePtr_t file);
static AFATFS_INLINE void afatfs_exfatAccountWrite(afatfsFilePtr_t file, uint32_t writtenBytes);
static AFATFS_INLINE void afatfs_exfatAdjustFileInfo(afatfsFilePtr_t file, afatfsFileInfo_t *info);
#else
#define afatfs_exfatFileHasPendingWork(file) false
#define afatfs_exfatIsIdle() true
#define afatfs_exfatPrepareTruncate(file) true
#define afatfs_exfatActive() false
#define afatfs_exfatFinishTruncate(file, targetSize) ((void)0)
#define afatfs_exfatLinearAllocation(file) false
#define afatfs_exfatConfigureTruncate(file) ((void)0)
#define afatfs_exfatProtectivePartition(partitionType) false
#define afatfs_exfatSelectGPT(entry) false
#define afatfs_exfatLegacyPartition(partitionType) false
#define afatfs_exfatVolumeSelected() (afatfs.partitionType == MBR_PARTITION_TYPE_EXFAT)
#define afatfs_exfatParseVolumeID(sector) false
#define afatfs_exfatSelectMountPhase() ((void)0)
#define afatfs_exfatContinueGPT() false
#define afatfs_exfatMountRoot() ((void)0)
#define afatfs_exfatSaveDirectoryEntry(file, mode) AFATFS_OPERATION_FAILURE
#define afatfs_exfatCompleteSync(file) ((void)0)
#define afatfs_exfatAccountWrite(file, writtenBytes) ((void)0)
#define afatfs_exfatAdjustFileInfo(file, info) ((void)0)
#endif

#if defined(ASYNCFS_ENABLE_EXFAT) || defined(AFATFS_ENABLE_FAT_JOURNAL)
static uint32_t afatfs_crc32Update(uint32_t crc, const uint8_t *bytes, unsigned length);
static AFATFS_INLINE void afatfs_journalResetCacheEpoch(afatfsCacheBlockDescriptor_t *descriptor);
#else
#define afatfs_journalResetCacheEpoch(descriptor) ((void)0)
#endif

#ifdef AFATFS_ENABLE_JOURNAL
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index);
static void afatfs_journalPoll(void);
static bool afatfs_journalCommitted(void);
static bool afatfs_journalUnresolvedError(void);
static bool afatfs_deferBackgroundFlush(void);
static bool afatfs_allocationMetadataBuffered(const afatfsFile_t *file);
static void afatfs_jfail(void);
static uint32_t afatfs_jsector(uint32_t offset);
static bool afatfs_jio(bool writing, uint32_t sector);
static bool afatfs_jwriteHeader(void);
static void afatfs_jselectHeader(uint32_t slot);
static afatfsOperationStatus_e afatfs_journalRecover(void);
static AFATFS_INLINE bool afatfs_journalIsIdle(void);
static AFATFS_INLINE void afatfs_journalFinishMount(void);
static AFATFS_INLINE bool afatfs_journalShutdown(void);
#else
// Without a journal there is no pending journal commit or journal error.
// Constant queries keep shared durability checks in place without calls or storage.
#define afatfs_allocationMetadataBuffered(file) false
#define afatfs_journalCommitted() true
#define afatfs_journalUnresolvedError() false
#define afatfs_journalIsIdle() true
#define afatfs_journalFinishMount() ((void)0)
#define afatfs_journalShutdown() true
#endif

#ifdef AFATFS_ENABLE_FAT_JOURNAL
static afatfsOperationStatus_e afatfs_fatJournalMount(void);
static bool afatfs_fatJournalShutdown(void);
static AFATFS_INLINE bool afatfs_fatJournalAllowsSector(LBA_t physicalSectorIndex, uint8_t sectorFlags);
#else
#define afatfs_fatJournalAllowsSector(physicalSectorIndex, sectorFlags) true
#endif

/* ==================== AsyncFS core ==================== */

/**
 * @brief Test whether the current media session may service filesystem operations.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_mediaSessionActive(void)
{
    return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION ||
        afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_READY ||
        afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_CLOSING;
}

/**
 * @brief Cancel backend ownership and reset all runtime state for a new media session.
 * @param state Lifecycle state to install after backend cancellation and runtime reset.
 */
static void afatfs_resetMediaSession(afatfsFilesystemState_e state)
{
    afatfs_apiResetSession(state);
    uint32_t generation = afatfs_mediaGeneration + 1;
    if (!generation) {
        generation = 1;
    }
    afatfs_mediaGeneration = generation;                      // Retire callbacks before stopping DMA/the backend.
    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_REMOVED; // Block reentrant I/O during cancellation.
    sdcard_reset(); // Must stop all accesses to old buffers/media before returning.
    memset(&afatfs, 0, sizeof(afatfs));
#if ENABLE_AFATFS_DMA_CACHE
    afatfs.cache = afatfs_cache;
#endif
    afatfs.fileSystemState = state;
}

/**
 * @brief Provide the weak, application-overridable FAT timestamp hook.
 * @return Packed FAT date in the high 16 bits and FAT time in the low 16 bits.
 */
AFATFS_WEAK uint32_t afatfs_fatTime(void)
{
    return ((uint32_t)AFATFS_DEFAULT_FILE_DATE << 16) | AFATFS_DEFAULT_FILE_TIME;
}

/**
 * @brief Round an unsigned value up to a multiple of the requested granularity.
 * @param value Integer value to encode, store or round.
 * @param rounding Nonzero granularity used to round the value upward.
 * @return The first multiple of rounding not smaller than value, subject to unsigned arithmetic limits.
 */
static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
    uint32_t remainder = value % rounding;

    if (remainder > 0) {
        value += rounding - remainder;
    }

    return value;
}

/**
 * @brief Test whether a nonzero integer contains exactly one set bit.
 * @param value Integer value to encode, store or round.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool isPowerOfTwo(unsigned int value)
{
    return ((value != 0) && ((value & (~value + 1)) == value));
}

/**
 * @brief Record a fatal filesystem error when an internal invariant fails.
 *
 * @details Check for conditions that should always be true (and if otherwise mean a bug or a corrupt filesystem).
 *
 * If the condition is false, the filesystem is marked as being in a fatal state.
 *
 * Returns the value of the condition.
 * @param condition Invariant or cluster-search condition to evaluate.
 * @return The supplied condition; false also latches the fatal filesystem state.
 */
static AFATFS_INLINE bool afatfs_assert(bool condition)
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

/**
 * @brief Test whether an internal operation or compatibility request owns a file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_fileIsBusy(afatfsFilePtr_t file)
{
    return file->operation.active != AFATFS_FILE_OPERATION_NONE
#if ASYNCFS_ENABLE_FATFS_API
        || (afatfs_request && !afatfs_request->driving && afatfs_request->file == file &&
               afatfs_request->operation >= AFATFS_API_CLOSE && afatfs_request->operation <= AFATFS_API_TRUNCATE) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->operation == AFATFS_API_FREE &&
            afatfs_request->phase && file->type == AFATFS_FILE_TYPE_NORMAL) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->operation == AFATFS_API_EXPAND &&
            afatfs_request->phase && file->type == AFATFS_FILE_TYPE_NORMAL) ||
        (afatfs_request && !afatfs_request->driving && afatfs_request->file == file &&
            afatfs_request->operation >= AFATFS_API_EXPAND && afatfs_request->operation <= AFATFS_API_FORWARD)
#endif
        ;
}

/**
 * @brief Record a per-file error and update the session diagnostic.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param error Filesystem diagnostic to record or return.
 * @return Always false, so callers can return the recorded failure directly.
 */
static bool afatfs_fileError(afatfsFilePtr_t file, afatfsError_e error)
{
    if (file) {
        file->error = (uint8_t)error;
    }
    afatfs.lastError = error;
    return false;
}

/**
 * @brief Read the last error recorded for a file handle.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return The per-file diagnostic, or AFATFS_ERROR_INVALID_OBJECT for a NULL handle.
 */
afatfsError_e afatfs_ferror(afatfsFilePtr_t file)
{
    return file && file->type != AFATFS_FILE_TYPE_NONE ? (afatfsError_e)file->error : AFATFS_ERROR_INVALID_OBJECT;
}

/**
 * @brief Clear the diagnostic stored in a file handle.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
void afatfs_clearerr(afatfsFilePtr_t file)
{
    if (file && file->type != AFATFS_FILE_TYPE_NONE) {
        file->error = AFATFS_ERROR_NONE;
    }
}

/**
 * @brief Return the number of FAT entries represented by one cached sector.
 *
 * @details The number of FAT table entries that fit within one AFATFS sector size.
 *
 * Note that this is the same as the number of clusters in an AFATFS supercluster.
 * @return 256 entries for FAT16, or 128 entries for FAT32/exFAT.
 */
static uint32_t afatfs_fatEntriesPerSector()
{
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16 ? AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR
                                                              : AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
}

/**
 * @brief Return the mounted volume's cluster size in bytes.
 *
 * @details Size of a FAT cluster in bytes
 * @return Mounted cluster size in bytes.
 */
ONLY_EXPOSE_FOR_TESTING
LBA_t afatfs_clusterSize()
{
    return afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;
}

/**
 * @brief Extract the byte offset within a cluster using the validated geometry mask.
 *
 * @details Given a byte offset within a file, return the byte offset of that position within the cluster it
 * belongs to.
 * @param byteOffset Byte position whose offset within the current cluster is needed.
 * @return Zero-based byte offset within the current cluster.
 */
static LBA_t afatfs_byteIndexInCluster(LBA_t byteOffset)
{
    return afatfs.byteInClusterMask & byteOffset;
}

/**
 * @brief Convert a file byte offset to its sector index within the current cluster.
 *
 * @details Given a byte offset within a file, return the index of the sector within the cluster it belongs to.
 * @param byteOffset Byte position whose offset within the current cluster is needed.
 * @return Zero-based sector index within the current cluster.
 */
static LBA_t afatfs_sectorIndexInCluster(LBA_t byteOffset)
{
    return afatfs_byteIndexInCluster(byteOffset) / AFATFS_SECTOR_SIZE;
}

// Get the buffer memory for the cache entry of the given index.
/**
 * @brief Locate the aligned memory belonging to a cache descriptor.
 * @param cacheEntryIndex Index of a slot in the statically allocated sector cache.
 * @return Pointer to the slot memory; ownership and readiness are determined by its descriptor.
 */
static uint8_t *afatfs_cacheSectorGetMemory(int cacheEntryIndex)
{
    return afatfs.cache + cacheEntryIndex * AFATFS_SECTOR_SIZE;
}

/**
 * @brief Validate a cache-buffer offset and obtain its descriptor index.
 * @param memory Pointer into the library cache; storage ownership remains with the cache.
 * @return Cache descriptor index, or -1 after a failed bounds invariant.
 */
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
AFATFS_INLINE
#endif
static int afatfs_getCacheDescriptorIndexForBuffer(uint8_t *memory)
{
    // Cache offsets are nonnegative. Validate before the unsigned division so
    // multiblock reads also avoid signed pointer-difference rounding code.
    size_t byteOffset = (size_t)(memory - afatfs.cache);

    if (afatfs_assert(byteOffset < (size_t)AFATFS_NUM_CACHE_SECTORS * AFATFS_SECTOR_SIZE)) {
        return (int)(byteOffset / AFATFS_SECTOR_SIZE);
    } else {
        return -1;
    }
}

/**
 * @brief Locate the descriptor owning a byte within the cache memory.
 * @param memory Pointer into the library cache; storage ownership remains with the cache.
 * @return Descriptor owning the supplied valid cache address.
 */
static afatfsCacheBlockDescriptor_t *afatfs_getCacheDescriptorForBuffer(uint8_t *memory)
{
    return afatfs.cacheDescriptor + afatfs_getCacheDescriptorIndexForBuffer(memory);
}

/**
 * @brief Mark a cache page dirty and assign its ordered write timestamp.
 * @param descriptor Cache descriptor whose state and ownership flags are inspected or updated.
 */
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

/**
 * @brief Initialize a descriptor when its cache slot is assigned to a sector.
 * @param descriptor Cache descriptor whose state and ownership flags are inspected or updated.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @param locked Initial write-lock state for the cache slot.
 */
static void afatfs_cacheSectorInit(afatfsCacheBlockDescriptor_t *descriptor, LBA_t sectorIndex, bool locked)
{
    afatfs_journalResetCacheEpoch(descriptor);
    descriptor->sectorIndex = sectorIndex;

    descriptor->accessTimestamp = descriptor->writeTimestamp = ++afatfs.cacheTimer;

    descriptor->consecutiveEraseBlockCount = 0;

    descriptor->state = AFATFS_CACHE_STATE_EMPTY;

    descriptor->locked = locked;
    descriptor->retainCount = 0;
    descriptor->discardable = 0;
}

/**
 * @brief Apply a read completion only to the matching live media generation.
 *
 * @details Called by the SD card driver when one of our read operations completes.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @param buffer Original 512-byte backend buffer, or NULL for a failed transfer.
 * @param callbackData Full media-generation identifier supplied when the backend operation started.
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
 * @brief Acknowledge a cached write without accepting completions from retired media.
 *
 * @details Called by the SD card driver when one of our write operations completes.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @param buffer Original backend buffer, or NULL for a failed transfer.
 * @param callbackData Full media-generation identifier supplied when the backend operation started.
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
 * @brief Submit one dirty cache page while preserving backend ownership until completion.
 *
 * @details Attempt to flush the dirty cache entry with the given index to the SDcard.
 * @param cacheIndex Index of the cache page being inspected or modified.
 */
static void afatfs_cacheFlushSector(int cacheIndex)
{
    afatfsCacheBlockDescriptor_t *cacheDescriptor = &afatfs.cacheDescriptor[cacheIndex];

#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
    // Discover a consecutive run in the existing write order. Never pre-erase
    // an uncached metadata sector whose undo image may not yet be durable.
    uint32_t blockCount = 1;
    uint32_t previousTime = cacheDescriptor->writeTimestamp;
    while (blockCount < AFATFS_NUM_CACHE_SECTORS && blockCount <= UINT32_MAX - cacheDescriptor->sectorIndex) {
        int nextIndex = -1;
        for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
            const afatfsCacheBlockDescriptor_t *candidate = &afatfs.cacheDescriptor[index];
            if (candidate->state == AFATFS_CACHE_STATE_DIRTY && !candidate->locked &&
                candidate->writeTimestamp > previousTime &&
                (nextIndex < 0 || candidate->writeTimestamp < afatfs.cacheDescriptor[nextIndex].writeTimestamp)) {
                nextIndex = index;
            }
        }
        if (nextIndex < 0 ||
            afatfs.cacheDescriptor[nextIndex].sectorIndex != cacheDescriptor->sectorIndex + blockCount) {
            break;
        }
        previousTime = afatfs.cacheDescriptor[nextIndex].writeTimestamp;
        blockCount++;
    }
    blockCount = MAX(blockCount, cacheDescriptor->consecutiveEraseBlockCount);
    if (blockCount >= AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT) {
        sdcardOperationStatus_e status = sdcard_beginWriteBlocks(cacheDescriptor->sectorIndex, blockCount);
        if (status != SDCARD_OPERATION_SUCCESS) {
            if (status == SDCARD_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            }
            return;
        }
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
 * @brief Find a cache descriptor by physical sector, regardless of its transfer state.
 *
 * @details Find a sector in the cache which corresponds to the given physical sector index, or NULL if the sector
 * isn't
 * cached. Note that the cached sector could be in any state including completely empty.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @return Matching descriptor, or NULL when the physical sector is not cached.
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
 * @brief Find a cached sector or select an eligible replacement without evicting owned pages.
 *
 * @details Find or allocate a cache sector for the given sector index on disk. Returns a block which matches one
 * of these
 * conditions (in descending order of preference):
 *
 * - The requested sector that already exists in the cache
 * - The index of an empty sector
 * - The index of a synced discardable sector
 * - The index of the oldest synced sector
 *
 * Otherwise it returns -1 to signal failure (cache is full!)
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @param mirrorBuffer Optional output for a clean FAT mirror, captured on a miss before replacing its descriptor.
 * @return Descriptor index, or -1 when no cache page is eligible.
 */
static AFATFS_INLINE int afatfs_allocateCacheSector(LBA_t sectorIndex, uint8_t **mirrorBuffer)
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

    // Most reads hit an existing page, especially the shared FAT page. Avoid
    // evaluating eviction candidates until a miss actually requires one.
    for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        if (afatfs.cacheDescriptor[index].sectorIndex == sectorIndex) {
            if (afatfs.cacheDescriptor[index].state == AFATFS_CACHE_STATE_EMPTY) {
                break;
            }
            afatfs.cacheDescriptor[index].accessTimestamp = ++afatfs.cacheTimer;
            return index;
        }
    }

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            // Preserve the original empty matching slot's allocation priority.
            emptyIndex = i;
            break;
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

#ifdef AFATFS_PIPELINE_ALLOCATION
    if (allocateIndex >= 0 && emptyIndex < 0 && afatfs.batchDirtyMarked &&
        afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
        afatfs.cacheDescriptor[allocateIndex].sectorIndex >= afatfs.fatStartSector &&
        afatfs.cacheDescriptor[allocateIndex].sectorIndex < afatfs.clusterStartSector) {
        // Keep an acknowledged FAT page while completed data can make room.
        // This is a soft preference: without flushable data the FAT may be evicted.
        for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
            const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
            if (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked &&
                entry->sectorIndex >= afatfs.clusterStartSector) {
                return -1;
            }
        }
    }
#endif
    if (allocateIndex > -1) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (mirrorBuffer) {
            // Capture before reinitializing a slot: the victim can be the mirror itself.
            *mirrorBuffer = afatfs_cleanFatMirror(sectorIndex);
        }
#else
        (void)mirrorBuffer;
#endif
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
    }

    return allocateIndex;
}

/**
 * @brief Test whether background flushing may defer an old directory image during allocation.
 *
 * @details Attempt to flush dirty cache pages out to the sdcard, returning true if all flushable data has been
 * flushed.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @return True when the condition or completion described above holds; false otherwise.
 */
#ifdef AFATFS_PIPELINE_ALLOCATION
static bool afatfs_deferDirectoryPage(const afatfsCacheBlockDescriptor_t *entry)
{
    // Bound the opportunistic delay even if the application stops writing.
    // Explicit flushes and durability barriers do not use this policy.
    if (afatfs.sectorsPerCluster != 1 || afatfs.cacheTimer - entry->accessTimestamp >= 64) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->type == AFATFS_FILE_TYPE_NORMAL && file->modified &&
            (file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) &&
            file->directoryEntryPos.sectorNumberPhysical == entry->sectorIndex &&
            (file->operation.active == AFATFS_FILE_OPERATION_NONE ||
                file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER ||
                (file->operation.active == AFATFS_FILE_OPERATION_CLOSE && file->operation.state.closeFile.phase < 2) ||
                (file->operation.active == AFATFS_FILE_OPERATION_SYNC && file->operation.state.syncFile.phase < 2))) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Start the oldest eligible write while excluding a specified directory sector.
 * @param excludedSector Physical sector to leave untouched during this filtered flush.
 * @param background True for opportunistic flushing that may defer directory publication.
 * @return True when no eligible page needs submission; false while a write is active or was started.
 */
static bool afatfs_flushCacheFiltered(LBA_t excludedSector, bool background)
#else
/**
 * @brief Attempt to flush eligible dirty pages without waiting for storage completion.
 * @return True when no flushable page needs submission; false while a write is active or was started.
 */
static bool afatfs_flushCache()
#endif
{
    if (!afatfs_mediaSessionActive()) {
        return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED;
    }
    if (afatfs_journalUnresolvedError()) {
        return false;
    }
    if (afatfs.cacheFlushInProgress) {
        return false;
    }
    if (afatfs.cacheDirtyEntries > 0) {
        // Flush the oldest flushable sector
        uint32_t earliestSectorTime = 0xFFFFFFFF;
        int earliestSectorIndex = -1;

        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_DIRTY || afatfs.cacheDescriptor[i].locked ||
                (earliestSectorIndex != -1 && afatfs.cacheDescriptor[i].writeTimestamp >= earliestSectorTime)) {
                continue;
            }
#ifdef AFATFS_PIPELINE_ALLOCATION
            if ((background && afatfs_collectFatPage(afatfs.cacheDescriptor[i].sectorIndex)) ||
                afatfs.cacheDescriptor[i].sectorIndex == excludedSector ||
                (background && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&
                    afatfs_deferDirectoryPage(&afatfs.cacheDescriptor[i]))) {
                continue;
            }
#endif
            earliestSectorIndex = i;
            earliestSectorTime = afatfs.cacheDescriptor[i].writeTimestamp;
        }

        if (earliestSectorIndex > -1) {
            afatfs_cacheFlushSector(earliestSectorIndex);

            // That flush will take time to complete so we may as well tell caller to come back later
            return false;
        }
    }

    return true;
}

#ifdef AFATFS_PIPELINE_ALLOCATION
/**
 * @brief Attempt to flush eligible dirty pages without waiting for storage completion.
 * @return True when no flushable page needs submission; false while a write is active or was started.
 */
static bool afatfs_flushCache(void)
{
    return afatfs_flushCacheFiltered(0, false);
}

/**
 * @brief Drain allocation data and FAT pages before publishing a file's directory update.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_flushAllocationCache(const afatfsFile_t *file)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
        file->type == AFATFS_FILE_TYPE_NORMAL) {
        // This page still advertises the previous, already durable chain.
        // Drain FAT/data first, then replace its old size with the new size.
        return afatfs_flushCacheFiltered(file->directoryEntryPos.sectorNumberPhysical, false);
    }
    return afatfs_flushCache();
}
#endif



/**
 * @brief Request a bounded consecutive backend read session without acquiring a data buffer.
 *
 * @details Flush cache pages and wait for the exFAT metadata checkpoint, if enabled.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @param blockCount Maximum number of consecutive sectors announced to the backend.
 * @return True when the backend accepted the bounded session; false when it must be retried.
 */
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
static bool afatfs_beginReadSession(LBA_t sectorIndex, uint32_t blockCount)
{
    sdcardOperationStatus_e status = sdcard_beginReadBlocks(sectorIndex, blockCount);
    if (status == SDCARD_OPERATION_FAILURE) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }
    return status == SDCARD_OPERATION_SUCCESS;
}

/**
 * @brief Attempt to stop the backend read session before a durability or shutdown boundary.
 * @return True once the session stop is acknowledged; false while the stop is still pending or failed.
 */
static bool afatfs_endReadSession(void)
{
    sdcardOperationStatus_e status = sdcard_endReadBlocks();
    if (status == SDCARD_OPERATION_FAILURE) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }
    return status == SDCARD_OPERATION_SUCCESS;
}
#endif

/**
 * @brief Advance an explicit cache flush and its required backend session boundaries.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @see afatfs_poll
 * @see afatfs_canPowerOff
 */
bool afatfs_flush(void)
{
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs_releaseReservedCluster() != AFATFS_OPERATION_SUCCESS) {
        return false;
    }
#endif
    if (!afatfs_flushCache()) {
        return false;
    }
    if (!afatfs_journalCommitted()) {
        return false;
    }
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    return afatfs_endReadSession();
#else
    return true;
#endif
}

/**
 * @brief Report the filesystem's current allocation-exhaustion indication.
 *
 * @details Returns true if either the freefile or the regular cluster pool has been exhausted during a previous
 * write operation.
 * @return True when the condition or completion described above holds; false otherwise.
 */
bool afatfs_isFull()
{
    return afatfs.filesystemFull;
}

/**
 * @brief Report whether the mounted session prohibits writes.
 * @return True when the condition or completion described above holds; false otherwise.
 */
bool afatfs_isReadOnly(void)
{
    return afatfs.readOnly;
}

/**
 * @brief Translate a FAT copy and sector offset into a physical sector number.
 *
 * @details Get the physical sector number that corresponds to the FAT sector of the given fatSectorIndex within
 * the given
 * FAT (fatIndex may be 0 or 1). (0, 0) gives the first sector of the first FAT.
 * @param fatIndex Zero-based FAT copy number; zero selects the primary FAT.
 * @param fatSectorIndex Sector offset within the selected FAT copy.
 * @return Physical sector number in the selected FAT copy.
 */
static LBA_t afatfs_fatSectorToPhysical(int fatIndex, LBA_t fatSectorIndex)
{
    return afatfs.fatStartSector + (fatIndex ? afatfs.fatSectors : 0) + fatSectorIndex;
}

/**
 * @brief Translate a data cluster and in-cluster sector into a physical sector.
 * @param clusterNumber Data-cluster number using the filesystem convention that the first cluster is two.
 * @param sectorIndex Physical sector number associated with the cache lookup or backend completion.
 * @return Physical data-sector number corresponding to the supplied coordinates.
 */
static LBA_t afatfs_fileClusterToPhysical(LBA_t clusterNumber, LBA_t sectorIndex)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
// Describe only a known contiguous run. No speculative pages, FAT IO, cursor
// changes or persistent state: the backend owns the bounded session counters.
/**
 * @brief Bound a possible read session by the eligible reader and its logical end of file.
 * @param physicalSector Physical data-sector number from which the bounded read run starts.
 * @return Maximum run length in sectors; one means that batching is not eligible.
 */
static uint16_t afatfs_readLimit(LBA_t physicalSector)
{
    afatfsFile_t *reader = NULL;
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 || afatfs.sectorsPerCluster != 1) {
        return 1;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->type == AFATFS_FILE_TYPE_NORMAL && (file->mode & AFATFS_FILE_MODE_READ)) {
            if (reader || (file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))) {
                return 1; // Alternating readers otherwise open/abort a session per sector.
            }
            reader = file;
        }
    }
    if (!reader || reader->cursorOffset >= reader->logicalSize || reader->cursorCluster < 2 ||
        reader->cursorCluster >= afatfs.numClusters + 2 ||
        physicalSector != afatfs.clusterStartSector + reader->cursorCluster - 2) {
        return 1;
    }
    return (uint16_t)MIN(AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR,
        (reader->logicalSize - 1) / AFATFS_SECTOR_SIZE - reader->cursorOffset / AFATFS_SECTOR_SIZE + 1);
}

/**
 * @brief Measure a contiguous FAT run using only entries already present in the cache.
 * @param physicalSector Physical data-sector number from which the bounded read run starts.
 * @param remaining Maximum number of sectors remaining in the eligible file range.
 * @return Number of verified contiguous sectors, limited by the cached FAT page and remaining range.
 */
static uint16_t afatfs_readExtent(LBA_t physicalSector, uint16_t remaining)
{
    uint32_t cluster = physicalSector - afatfs.clusterStartSector + 2;
    if (afatfs.numClusters + 2 - cluster < remaining) {
        remaining = (uint16_t)(afatfs.numClusters + 2 - cluster);
    }
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->sectorIndex != afatfs.fatStartSector + cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR ||
            (entry->state != AFATFS_CACHE_STATE_IN_SYNC && entry->state != AFATFS_CACHE_STATE_DIRTY)) {
            continue;
        }
        const uint32_t *table = (const uint32_t *)(afatfs.cache + index * AFATFS_SECTOR_SIZE);
        uint16_t first = (uint16_t)(cluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
        if (AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR - first < remaining) {
            remaining = (uint16_t)(AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR - first);
        }
        uint16_t count = 1;
        while (count < remaining && (table[first + count - 1] & 0x0FFFFFFFU) == cluster + count) {
            count++;
        }
        return count;
    }
    return 1;
}
#endif

#ifdef __XC16__
__attribute__((always_inline)) inline
#endif
/**
 * @brief Resolve the physical sector at a file cursor, including the FAT16 fixed root.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return Physical sector number at the current cursor.
 */
static LBA_t afatfs_fileGetCursorPhysicalSector(afatfsFilePtr_t file)
{
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + file->cursorOffset / AFATFS_SECTOR_SIZE;
    } else {
#if defined(AFATFS_PIPELINE_ALLOCATION) && !defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT)
        if (afatfs.sectorsPerCluster == 1) {
            // The cursor's sector within a one-sector cluster is always zero.
            return afatfs.clusterStartSector + file->cursorCluster - 2;
        }
#endif
        LBA_t cursorSectorInCluster = afatfs_sectorIndexInCluster(file->cursorOffset);
        return afatfs_fileClusterToPhysical(file->cursorCluster, cursorSectorInCluster);
    }
}

/**
 * @brief Split a cursor into its current cluster and in-cluster sector coordinates.
 *
 * @details Sector here is the sector index within the cluster.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param cluster Receives the cursor cluster number.
 * @param sector Receives the cursor sector index within its cluster.
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


#ifdef AFATFS_PIPELINE_ALLOCATION
// Keep this shallow lookup out of the cache routine's deep allocation/flush
// stack path. Its result uses the caller's existing output pointer as scratch.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
/**
 * @brief Find a clean acknowledged mirror image that can reconstruct a missing primary FAT page.
 * @param physicalSectorIndex Physical sector number to acquire or inspect.
 * @return Pointer to the selected library-owned data; NULL when no matching data is available.
 */
static uint8_t *afatfs_cleanFatMirror(LBA_t physicalSectorIndex)
{
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION &&
        afatfs.initPhase == AFATFS_INITIALIZATION_BATCH_RECOVERY) {
        return NULL; // Recovery must inspect each on-disk copy independently.
    }
#endif
    LBA_t mirrorSector = physicalSectorIndex + afatfs.fatSectors;
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->sectorIndex == mirrorSector && entry->state == AFATFS_CACHE_STATE_IN_SYNC && !entry->locked) {
            return afatfs_cacheSectorGetMemory(index);
        }
    }
    return NULL;
}
#endif

#if AFATFS_READ_SESSION_QUERY
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
#elif defined(_MSC_VER)
__forceinline
#else
__attribute__((always_inline)) inline
#endif
/**
 * @brief Find a cached page whose state permits immediate reads without allocation.
 * @param physicalSectorIndex Physical sector number to acquire or inspect.
 * @return Readable descriptor index, or -1 when the lookup cannot bypass allocation.
 */
static int afatfs_findReadableCacheSector(LBA_t physicalSectorIndex)
{
    // Keep the lookup's registers out of the read-session path when querying
    // the backend; other configurations retain the inline cache-hit path.
    for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[index];
        if (descriptor->sectorIndex != physicalSectorIndex) {
            continue;
        }
        if (descriptor->state == AFATFS_CACHE_STATE_IN_SYNC ||
            descriptor->state == AFATFS_CACHE_STATE_DIRTY || descriptor->state == AFATFS_CACHE_STATE_WRITING) {
            return index;
        }
        break;
    }
    return -1;
}

/**
 * @brief Acquire a cached sector with the requested read, write, lock and retention semantics.
 *
 * @details Get a cache entry for the given sector and store a pointer to the cached memory in *buffer.
 *
 * physicalSectorIndex - The index of the sector in the SD card to cache
 * sectorflags         - A union of AFATFS_CACHE_* constants that says which operations the sector will be cached
 * for.
 * buffer              - A pointer to the 512-byte memory buffer for the sector will be stored here upon success
 * eraseCount          - For write operations, set to a non-zero number to hint that we plan to write that many
 * sectors
 *                       consecutively (including this sector)
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 * @param physicalSectorIndex Physical sector number to acquire or inspect.
 * @param buffer Receives a cache-page pointer on success; do not free or use it before success.
 * @param sectorFlags AFATFS_CACHE_* access and ownership flags requested by the caller.
 * @param eraseCount Consecutive-write hint in sectors; zero disables pre-erasing future data sectors.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
    if (sectorFlags == AFATFS_CACHE_READ) {
        // Plain reads of an existing page need neither mirror recovery nor an
        // allocation decision. Keep the allocator's bounds and access accounting.
        int readableIndex = afatfs_findReadableCacheSector(physicalSectorIndex);
        if (readableIndex >= 0) {
            if (!afatfs_assert(afatfs.numClusters == 0 || physicalSectorIndex <
                    afatfs.clusterStartSector + afatfs.numClusters * afatfs.sectorsPerCluster)) {
                return AFATFS_OPERATION_FAILURE;
            }
            afatfs.cacheDescriptor[readableIndex].accessTimestamp = ++afatfs.cacheTimer;
            *buffer = afatfs_cacheSectorGetMemory(readableIndex);
            return AFATFS_OPERATION_SUCCESS;
        }
    }
    if (!afatfs_fatJournalAllowsSector(physicalSectorIndex, sectorFlags)) {
        return AFATFS_OPERATION_FAILURE;
    }

    uint8_t **mirrorBuffer = NULL;
#ifdef AFATFS_PIPELINE_ALLOCATION
    *buffer = NULL;
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && (sectorFlags & AFATFS_CACHE_READ) &&
        physicalSectorIndex >= afatfs.fatStartSector &&
        physicalSectorIndex - afatfs.fatStartSector < afatfs.fatSectors) {
        mirrorBuffer = buffer;
    }
#endif

    int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex, mirrorBuffer);

    if (cacheSectorIndex == -1) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            !afatfs.cacheFlushInProgress) {
            int dataIndex = -1;
            // Released writer data is discardable only after its ACK. Prefer
            // making that space reusable over evicting the FAT or publishing
            // an old directory image merely to satisfy cache pressure.
            for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
                const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
                if (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked && entry->discardable &&
                    (dataIndex < 0 || entry->writeTimestamp < afatfs.cacheDescriptor[dataIndex].writeTimestamp)) {
                    dataIndex = index;
                }
            }
            if (dataIndex >= 0) {
                afatfs_cacheFlushSector(dataIndex);
                return AFATFS_OPERATION_IN_PROGRESS;
            }
        }
#endif
        // Batching must yield to cache pressure, including fragmented FAT chains
        // and a reader/writer holding the other cache entries.
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && physicalSectorIndex >= afatfs.fatStartSector &&
            physicalSectorIndex - afatfs.fatStartSector < 2 * afatfs.fatSectors &&
            !afatfs_flushCacheFiltered(0, true)) {
            // Prefer releasing a completed data page to repeatedly publishing
            // an old directory size merely to acquire the second FAT page.
            return AFATFS_OPERATION_IN_PROGRESS;
        }
#endif
        afatfs_flushCache();
        // We don't have enough free cache to service this request right now, try again later
        return AFATFS_OPERATION_IN_PROGRESS;
    }

#ifdef AFATFS_PIPELINE_ALLOCATION
    if (*buffer && afatfs.cacheDescriptor[cacheSectorIndex].state == AFATFS_CACHE_STATE_EMPTY) {
        // A dirty/in-flight primary cannot have been evicted, and a clean mirror
        // has acknowledged the same image. Reconstruct the missing primary from
        // that image without I/O. The allocator may have reused the mirror's
        // own slot: cacheSectorInit changes its descriptor, never its bytes.
        if (*buffer != afatfs_cacheSectorGetMemory(cacheSectorIndex)) {
            memcpy(afatfs_cacheSectorGetMemory(cacheSectorIndex), *buffer, AFATFS_SECTOR_SIZE);
        }
        afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_IN_SYNC;
    }
#endif

#ifdef AFATFS_ENABLE_JOURNAL
    if ((sectorFlags & (AFATFS_CACHE_METADATA | AFATFS_CACHE_WRITE)) == (AFATFS_CACHE_METADATA | AFATFS_CACHE_WRITE) &&
        afatfs.journal.ready) {
        afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[cacheSectorIndex];
        if (descriptor->state == AFATFS_CACHE_STATE_EMPTY) {
            sectorFlags |= AFATFS_CACHE_READ;
        } else if (descriptor->state != AFATFS_CACHE_STATE_READING) {
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
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
                eraseCount = afatfs_readLimit(physicalSectorIndex);
                if (eraseCount >= AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) {
#if AFATFS_READ_SESSION_QUERY
                    if (!sdcard_isReadBlockInSequence(physicalSectorIndex)) {
#endif
                        eraseCount = afatfs_readExtent(physicalSectorIndex, (uint16_t)eraseCount);
                        if (eraseCount >= AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT &&
                            !afatfs_beginReadSession(physicalSectorIndex, eraseCount)) {
                            return AFATFS_OPERATION_IN_PROGRESS;
                        }
#if AFATFS_READ_SESSION_QUERY
                    }
#endif
                }
#endif
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
            // Only append-only data may announce future sectors. FAT and
            // directory sectors must be captured and ready in cache first.
            if (sectorFlags & AFATFS_CACHE_METADATA) {
                eraseCount = 0;
            }
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
#ifdef AFATFS_PIPELINE_ALLOCATION
            if ((sectorFlags & AFATFS_CACHE_WRITE) &&
                afatfs.cacheDescriptor[cacheSectorIndex].state == AFATFS_CACHE_STATE_WRITING) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#endif
            // Fall through
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


/**
 * @brief Inspect the MBR or superfloppy header and select a supported partition.
 *
 * @details Parse a legacy or protective MBR, preserving the legacy partition selection order.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_parseMBR(const uint8_t *sector)
{
    // Check MBR signature
    if (sector[AFATFS_SECTOR_SIZE - 2] != FAT_VOLUME_ID_SIGNATURE_1 ||
        sector[AFATFS_SECTOR_SIZE - 1] != FAT_VOLUME_ID_SIGNATURE_2) {
        return false;
    }

    mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *)(sector + 446);

    for (int i = 0; i < 4; i++) {
        // Preserve the existing protective/legacy entry selection order.
        if (afatfs_exfatProtectivePartition(partition[i].type)) {
            return afatfs_exfatSelectGPT(&partition[i]);
        }
        if (partition[i].lbaBegin > 0 &&
            (partition[i].type == MBR_PARTITION_TYPE_FAT32 || partition[i].type == MBR_PARTITION_TYPE_FAT32_LBA ||
                partition[i].type == MBR_PARTITION_TYPE_FAT16 || partition[i].type == MBR_PARTITION_TYPE_FAT16_LBA
                || afatfs_exfatLegacyPartition(partition[i].type)
                )) {
            afatfs.partitionStartSector = partition[i].lbaBegin;
            afatfs.partitionType = partition[i].type;
            afatfs.partitionSectors = partition[i].numSectors;

            return true;
        }
    }

    return false;
}

/**
 * @brief Validate the boot sector and install the supported filesystem geometry.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_parseVolumeID(const uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *)sector;

    afatfs.fileSystemType = FAT_FILESYSTEM_TYPE_INVALID;

    if (!afatfs_exfatVolumeSelected()) {
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
        return afatfs_exfatParseVolumeID(sector);
    }
    return true;
}

/**
 * @brief Resolve a cluster number to its FAT sector and entry offset.
 *
 * @details Get the position of the FAT entry for the cluster with the given number.
 * @param cluster Data-cluster number to inspect or update.
 * @param fatSectorIndex Receives the FAT-sector offset containing this cluster.
 * @param fatSectorEntryIndex Receives the entry index within the selected FAT sector.
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

/**
 * @brief Test a decoded cluster value against the active filesystem's end-of-chain range.
 * @param clusterNumber Data-cluster number using the filesystem convention that the first cluster is two.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_FATIsEndOfChainMarker(uint32_t clusterNumber)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        return fat32_isEndOfChainMarker(clusterNumber);
    } else if (afatfs_exfatActive()) {
        return exFat_isEndOfChainMarker(clusterNumber);
    } else {
        return fat16_isEndOfChainMarker(clusterNumber);
    }
}

/**
 * @brief Read the next cluster link from the selected FAT copy through the cache.
 *
 * @details Look up the FAT to find out which cluster follows the one with the given number and store it into
 * *nextCluster.
 *
 * Use fat_isFreeSpace() and fat_isEndOfChainMarker() on nextCluster to distinguish those special values from
 * regular
 * cluster numbers.
 *
 * Note that if you're trying to find the next cluster of a file, you should be calling afatfs_fileGetNextCluster()
 * instead, as that one supports contiguous freefile-based files (which needn't consult the FAT).
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - *nextCluster is set to the next cluster number
 * @param fatIndex Zero-based FAT copy number; zero selects the primary FAT.
 * @param cluster Data-cluster number to inspect or update.
 * @param nextCluster Successor cluster value or output receiving the decoded successor.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Acquire writable primary and mirror FAT pages under the required journal protection.
 *
 * @details Acquire writable FAT cache sectors. On FAT16/FAT32, callers update the primary
 * sector and copy it to the returned mirror only after both acquisitions succeed.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE     - When the filesystem encounters a fatal error
 * @param fatSectorIndex Zero-based sector offset within a FAT copy.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @param mirror Output receiving the writable mirror FAT page; cache ownership is retained by the library.
 * @param flags Flags controlling the requested cache or metadata operation.
 * @param mirrorDirty Optional output permitting entry-sized copies only for an already-dirty native paired image.
 * @param eraseCount Consecutive-write hint in sectors; zero disables pre-erasing future data sectors.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_FATWriteSector(
    LBA_t fatSectorIndex, afatfsFATSector_t *sector, uint8_t **mirror, uint8_t flags,
    AFATFS_FAT_MIRROR_STATE(bool *mirrorDirty) uint32_t eraseCount)
{
    *mirror = NULL;
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    if (mirrorDirty) {
        *mirrorDirty = false;
    }
#endif
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        if (afatfs.cacheFlushInProgress) {
            for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
                const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
                if (entry->state == AFATFS_CACHE_STATE_WRITING &&
                    (entry->sectorIndex == afatfs_fatSectorToPhysical(0, fatSectorIndex) ||
                        entry->sectorIndex == afatfs_fatSectorToPhysical(1, fatSectorIndex))) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
            }
        }
        afatfsOperationStatus_e status = afatfs_cacheSector(afatfs_fatSectorToPhysical(0, fatSectorIndex),
            &sector->bytes, (flags & ~AFATFS_CACHE_WRITE) | AFATFS_CACHE_RETAIN | AFATFS_CACHE_METADATA, eraseCount);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        // Retain the clean primary while acquiring the mirror, so the mirror
        // cannot evict it. Neither page becomes dirty on an unsuccessful retry.
        afatfsCacheBlockDescriptor_t *primary = afatfs_getCacheDescriptorForBuffer(sector->bytes);
        status = afatfs_cacheSector(afatfs_fatSectorToPhysical(1, fatSectorIndex), mirror,
            (flags & AFATFS_CACHE_DISCARDABLE) | AFATFS_CACHE_METADATA, eraseCount);
        primary->retainCount--;
        if (status == AFATFS_OPERATION_SUCCESS) {
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
            afatfsCacheBlockDescriptor_t *mirrorDescriptor = afatfs_getCacheDescriptorForBuffer(*mirror);
            // Capture before marking dirty: only an existing dirty native pair
            // permits an entry-sized copy. New, clean or in-flight pages still
            // need the caller's complete updated image, just as before.
            if (mirrorDirty) {
                *mirrorDirty = mirrorDescriptor->state == AFATFS_CACHE_STATE_DIRTY;
            }
            afatfs_cacheSectorMarkDirty(primary);
            afatfs_cacheSectorMarkDirty(mirrorDescriptor);
#else
            afatfs_cacheSectorMarkDirty(primary);
            afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer(*mirror));
#endif
        }
        return status;
    }
#endif
    afatfsOperationStatus_e status = afatfs_cacheSector(
        afatfs_fatSectorToPhysical(0, fatSectorIndex), &sector->bytes, flags | AFATFS_CACHE_METADATA, eraseCount);
    if (status == AFATFS_OPERATION_SUCCESS && afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT) {
        // Acquire both existing cache entries before changing either copy. A cache miss must not advance the caller.
        status = afatfs_cacheSector(afatfs_fatSectorToPhysical(1, fatSectorIndex), mirror,
            AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA | (flags & AFATFS_CACHE_DISCARDABLE), eraseCount);
    }
    return status;
}

/**
 * @brief Update a cluster link and its mirror without bypassing metadata ordering.
 * @param startCluster First cluster included in this traversal or update.
 * @param nextCluster Successor cluster value or output receiving the decoded successor.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_FATSetNextCluster(LBA_t startCluster, LBA_t nextCluster)
{
#ifdef AFATFS_ENABLE_FAT_JOURNAL
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.ready &&
        startCluster >= afatfs.journal.firstCluster &&
        startCluster - afatfs.journal.firstCluster < afatfs.journal.sectors / afatfs.sectorsPerCluster) {
        afatfs.lastError = AFATFS_ERROR_DENIED;
        return AFATFS_OPERATION_FAILURE;
    }
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.ready && !nextCluster) {
        afatfs.journal.blockAllocation = true;
    }
#endif
    afatfsFATSector_t sector;
    uint8_t *mirror;
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    bool mirrorDirty;
#endif
    LBA_t fatSectorIndex;
    LBA_t fatSectorEntryIndex;
    afatfsOperationStatus_e result;

    if (!afatfs_assert(startCluster >= FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
        return AFATFS_OPERATION_FAILURE; // startCluster is not valid;
    }

    afatfs_getFATPositionForCluster(startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    result = afatfs_FATWriteSector(
        fatSectorIndex, &sector, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA,
        AFATFS_FAT_MIRROR_STATE(&mirrorDirty) 0);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            sector.fat16[fatSectorEntryIndex] = nextCluster;
        } else if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
            sector.fat32[fatSectorEntryIndex] =
                (sector.fat32[fatSectorEntryIndex] & 0xF0000000) | (nextCluster & 0x0FFFFFFF);
        } else {
            sector.fat32[fatSectorEntryIndex] = nextCluster;
        }
        if (mirror) {
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
            if (mirrorDirty) {
                memcpy(mirror + fatSectorEntryIndex * sizeof(uint32_t),
                    &sector.fat32[fatSectorEntryIndex], sizeof(uint32_t));
            } else {
                memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
            }
#else
            memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
#endif
        }
    }

    return result;
}


/**
 * @brief Reconcile a file's logical size with completed cursor and allocation changes.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_fileUpdateFilesize(afatfsFile_t *file)
{
    file->logicalSize = MAX(file->logicalSize, file->cursorOffset);
}

/**
 * @brief Release the file's read retention and write lock at a cursor boundary.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_fileUnlockCacheSector(afatfsFilePtr_t file)
{
    if (file->writeLockedCacheIndex != -1) {
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL) {
            // Applies equally to fwrite, fputc, append and update handles.
            // Dirty/in-flight pages remain ineligible for cache replacement.
            afatfs.cacheDescriptor[file->writeLockedCacheIndex].discardable = true;
        }
        afatfs.cacheDescriptor[file->writeLockedCacheIndex].locked = 0;
        file->writeLockedCacheIndex = -1;
    }
    if (file->readRetainCacheIndex != -1) {
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL &&
            !(file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))) {
            // Reuse released data before another reader's untouched prefetch.
            // Dirty, retained and in-flight pages remain protected by the allocator.
            afatfs.cacheDescriptor[file->readRetainCacheIndex].discardable = true;
        }
        afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount =
            MAX((int)afatfs.cacheDescriptor[file->readRetainCacheIndex].retainCount - 1, 0);
        file->readRetainCacheIndex = -1;
    }
}

/**
 * @brief Test whether the pending file allocation owns the unique look-ahead reservation.
 *
 * @details Starting from and including the given cluster number, find the number of the first cluster which
 * matches the given
 * condition.
 *
 * searchLimit - Last cluster to examine (exclusive). To search the entire volume, pass:
 *                   afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER
 *
 * Condition:
 *     CLUSTER_SEARCH_FREE_AT_BEGINNING_OF_FAT_SECTOR - Find a cluster marked as free in the FAT which lies at the
 *         beginning of its FAT sector. The passed initial search 'cluster' must correspond to the first entry of a
 * FAT sector.
 *     CLUSTER_SEARCH_FREE            - Find a cluster marked as free in the FAT
 *     CLUSTER_SEARCH_OCCUPIED        - Find a cluster marked as occupied in the FAT.
 *
 * Returns:
 *     AFATFS_FIND_CLUSTER_FOUND       - A cluster matching the criteria was found and stored in *cluster
 *     AFATFS_FIND_CLUSTER_IN_PROGRESS - The search is not over, call this routine again later with the updated
 * *cluster value to resume
 *     AFATFS_FIND_CLUSTER_FATAL       - An unexpected read error occurred, the volume should be abandoned
 *     AFATFS_FIND_CLUSTER_NOT_FOUND   - The entire device was searched without finding a suitable cluster (the
 *                                       *cluster points to just beyond the final cluster).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param cluster Data-cluster number to inspect or update.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_fileReservesCluster(afatfsFilePtr_t file, uint32_t cluster)
{
    return (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER ||
               file->operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY) &&
        file->operation.state.appendFreeCluster.phase > AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE &&
        file->operation.state.appendFreeCluster.phase < AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE &&
        file->operation.state.appendFreeCluster.searchCluster == cluster;
}

/**
 * @brief Test a cluster against the current unlinked allocation reservation.
 * @param cluster Data-cluster number to inspect or update.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_clusterIsReserved(uint32_t cluster)
{
    if (afatfs_fileReservesCluster(&afatfs.currentDirectory, cluster)) {
        return true;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (afatfs_fileReservesCluster(&afatfs.openFiles[index], cluster)) {
            return true;
        }
    }
#ifdef AFATFS_USE_FREEFILE
    if (afatfs_fileReservesCluster(&afatfs.freeFile, cluster)) {
        return true;
    }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (afatfs_fileReservesCluster(&afatfs.introSpecLog, cluster)) {
        return true;
    }
#endif
    return false;
}

#ifdef AFATFS_PIPELINE_ALLOCATION
/**
 * @brief Return the unlinked look-ahead cluster to the FAT before abandoning its reservation.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_releaseReservedCluster(void)
{
    if (!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER)) {
        return AFATFS_OPERATION_SUCCESS;
    }
    uint32_t cluster = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
    afatfsOperationStatus_e status = afatfs_FATSetNextCluster(cluster, 0);
    if (status == AFATFS_OPERATION_SUCCESS) {
        afatfs.lastClusterAllocated = cluster;
        afatfs.filesystemFull = false;
    }
    return status;
}

/**
 * @brief Age cache activity and release an unused reservation only after the idle conditions hold.
 */
#if defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) && !AFATFS_READ_SESSION_QUERY
AFATFS_INLINE
#endif
static void afatfs_releaseIdleReservation(void)
{
    afatfs.cacheTimer++;
    if (!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) ||
        afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY || afatfs.cacheDirtyEntries ||
        afatfs.cacheFlushInProgress) {
        return;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (afatfs.openFiles[index].operation.active != AFATFS_FILE_OPERATION_NONE) {
            return;
        }
    }
    // Data access counts as activity too: a large cluster can stream many
    // sectors without touching its FAT page. Do not discard its look-ahead.
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state != AFATFS_CACHE_STATE_EMPTY && afatfs.cacheTimer - entry->accessTimestamp < 64) {
            return;
        }
    }
    afatfs_releaseReservedCluster();
}

/**
 * @brief Try to reserve one following cluster without adding a second reservation or cache buffer.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_reserveNextCluster(const afatfsFile_t *file)
{
    const afatfsAppendFreeCluster_t *state = &file->operation.state.appendFreeCluster;
    uint32_t candidate = state->searchCluster + 1;
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 || file->type != AFATFS_FILE_TYPE_NORMAL ||
        afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY ||
        (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) || candidate >= afatfs.numClusters + 2 ||
        candidate > AFATFS_CLUSTER_HINT_MASK ||
        candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR !=
            (state->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1 ? state->searchCluster
                                                                          : state->previousCluster) /
                AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR ||
        afatfs_clusterIsReserved(candidate)) {
        return;
    }
    // An already requested peer takes priority over optional look-ahead.
    // Its EOC can share the dirty pair without allocating a speculative target.
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *peer = &afatfs.openFiles[index];
        if (peer != file && peer->type == AFATFS_FILE_TYPE_NORMAL &&
            peer->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
            return;
        }
    }
    // The current-EOC or previous-link sector and both mirrors were just acquired.
    // This look-ahead EOC shares that write, but is never used until its ACKs.
    uint32_t next;
    if (afatfs_FATGetNextCluster(0, candidate, &next) == AFATFS_OPERATION_SUCCESS && fat_isFreeSpace(next)) {
        afatfsOperationStatus_e status = afatfs_FATSetNextCluster(candidate, 0xFFFFFFFF);
        if (afatfs_assert(status == AFATFS_OPERATION_SUCCESS)) {
            afatfs.lastClusterAllocated = candidate | AFATFS_RESERVED_CLUSTER;
        }
    }
}
#endif

/**
 * @brief Scan FAT entries for a free cluster or chain boundary within the caller's search limit.
 * @param condition FAT or bitmap occupancy condition sought by the scan.
 * @param cluster Data-cluster number to inspect or update.
 * @param searchLimit Exclusive cluster-number boundary for this scan.
 * @return AFATFS_FIND_CLUSTER_* result distinguishing a match, pending work and an exhausted or fatal scan.
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

                    if ((fat_isFreeSpace(clusterNumber) && !afatfs_clusterIsReserved(*cluster)) == lookingForFree) {
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
 * @brief Resolve the next file cluster for FAT-linked, freefile or exFAT contiguous streams.
 *
 * @details Get the cluster that follows the currentCluster in the FAT chain for the given file.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - *nextCluster is set to the next cluster number
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param currentCluster Current cluster whose successor is required.
 * @param nextCluster Successor cluster value or output receiving the decoded successor.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Fill FAT entries with a free-space or sequential-link pattern through writable cache pages.
 *
 * @details Update the FAT to fill the contiguous series of clusters with indexes [*startCluster...endCluster) with
 * the
 * specified pattern.
 *
 * AFATFS_FAT_PATTERN_TERMINATED_CHAIN - Chain the clusters together in linear sequence and terminate the final
 * cluster
 * AFATFS_FAT_PATTERN_CHAIN            - Chain the clusters together without terminating the final entry
 * AFATFS_FAT_PATTERN_FREE             - Mark the clusters as free space
 *
 * Returns -
 *     AFATFS_OPERATION_SUCCESS        - When the entire chain has been written
 *     AFATFS_OPERATION_IN_PROGRESS    - Call again later with the updated *startCluster value in order to resume
 * writing.
 * @param pattern Wildcard or FAT-fill pattern interpreted by this helper.
 * @param startCluster First cluster included in this traversal or update.
 * @param endCluster Cluster boundary at which traversal or modification must stop.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_FATFillWithPattern(
    afatfsFATPattern_e pattern, LBA_t *startCluster, LBA_t endCluster)
{
    afatfsFATSector_t sector;
    uint8_t *mirror;
    LBA_t fatSectorIndex;
    LBA_t firstEntryIndex;
    uint8_t fatEntrySize;
    LBA_t nextCluster;
    afatfsOperationStatus_e result;
    uint32_t eraseSectorCount;

    // Find the position of the initial cluster to begin our fill
    afatfs_getFATPositionForCluster(*startCluster, &fatSectorIndex, &firstEntryIndex);

    // How many consecutive FAT sectors will we be overwriting?
    eraseSectorCount = (endCluster - *startCluster + firstEntryIndex + afatfs_fatEntriesPerSector() - 1) /
        afatfs_fatEntriesPerSector();

    while (*startCluster < endCluster) {
        // The last entry we will fill inside this sector (exclusive):
        uint32_t lastEntryIndex = MIN(firstEntryIndex + (endCluster - *startCluster), afatfs_fatEntriesPerSector());

        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE;

        if (firstEntryIndex > 0 || lastEntryIndex < afatfs_fatEntriesPerSector() ||
            afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
            // We're not overwriting the entire FAT sector so we must read the existing contents
            cacheFlags |= AFATFS_CACHE_READ;
        }

        result = afatfs_FATWriteSector(
            fatSectorIndex, &sector, &mirror, cacheFlags, AFATFS_FAT_MIRROR_STATE(NULL) eraseSectorCount);

        if (result != AFATFS_OPERATION_SUCCESS) {
            return result;
        }

#ifdef AFATFS_DEBUG_VERBOSE
        if (pattern == AFATFS_FAT_PATTERN_FREE) {
            fprintf(stderr, "Marking cluster %u to %u as free in FAT sector %u...\n", *startCluster, endCluster,
                afatfs_fatSectorToPhysical(0, fatSectorIndex));
        } else {
            fprintf(stderr, "Writing FAT chain from cluster %u to %u in FAT sector %u...\n", *startCluster, endCluster,
                afatfs_fatSectorToPhysical(0, fatSectorIndex));
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
                        sector.fat32[i] = (sector.fat32[i] & 0xF0000000) | (nextCluster & 0x0FFFFFFF);
                    }
                }

                *startCluster += lastEntryIndex - firstEntryIndex;

                if (pattern == AFATFS_FAT_PATTERN_TERMINATED_CHAIN && *startCluster == endCluster) {
                    // We completed the chain! Overwrite the last entry we wrote with the terminator for the end of the chain
                    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                        sector.fat16[lastEntryIndex - 1] = 0xFFFF;
                    } else {
                        sector.fat32[lastEntryIndex - 1] |= 0x0FFFFFFF;
                    }
                    break;
                }
                break;
            case AFATFS_FAT_PATTERN_FREE:
                fatEntrySize = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT16 ? sizeof(uint16_t) : sizeof(uint32_t);

                if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32) {
                    for (uint32_t index = firstEntryIndex; index < lastEntryIndex; index++) {
                        sector.fat32[index] &= 0xF0000000;
                    }
                } else {
                    memset(sector.bytes + firstEntryIndex * fatEntrySize, 0,
                        (lastEntryIndex - firstEntryIndex) * fatEntrySize);
                }

                *startCluster += lastEntryIndex - firstEntryIndex;
                break;
        }

        if (mirror) {
            memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
        }
        fatSectorIndex++;
        eraseSectorCount--;
        firstEntryIndex = 0;
    }

    return AFATFS_OPERATION_SUCCESS;
}

#endif


/**
 * @brief Persist the selected fields of a file's directory entry through the metadata cache.
 *
 * @details Write the directory entry for the file into its `directoryEntryPos` position in its containing
 * directory.
 *
 * mode:
 *     AFATFS_SAVE_DIRECTORY_NORMAL    - Store the file's physical size, not the logical size, in the directory
 * entry
 *     AFATFS_SAVE_DIRECTORY_FOR_CLOSE - We're done extending the file so we can write the logical size now.
 *     AFATFS_SAVE_DIRECTORY_DELETED   - Mark the directory entry as deleted
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - The directory entry has been stored into the directory sector in cache.
 *     AFATFS_OPERATION_IN_PROGRESS - Cache is too busy, retry later
 *     AFATFS_OPERATION_FAILURE - If the filesystem enters the fatal state
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param mode AFATFS_SAVE_DIRECTORY_* field-selection mode.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    if (file->directoryEntryPos.sectorNumberPhysical == 0) {
        return AFATFS_OPERATION_SUCCESS; // Root directories don't have a directory entry
    }
    if (afatfs_lfnDeletionPending(file, mode)) {
        return afatfs_lfnDelete(file);
    }
    if (file->modified && file->type == AFATFS_FILE_TYPE_NORMAL) {
        file->attrib |= FAT_FILE_ATTRIBUTE_ARCHIVE;
    }

    if (afatfs_exfatActive()) {
        return afatfs_exfatSaveDirectoryEntry(file, mode);
    }

    result = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &sector,
        AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, 0);

#ifdef AFATFS_DEBUG_VERBOSE
    fprintf(stderr, "Saving directory entry to sector %u...\n", file->directoryEntryPos.sectorNumberPhysical);
#endif

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs_assert(file->directoryEntryPos.entryIndex >= 0)) {
#ifdef AFATFS_PIPELINE_ALLOCATION
            file->directoryExact = mode == AFATFS_SAVE_DIRECTORY_FOR_CLOSE;
#endif
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
            if (afatfs_lfnBeginDeletion(file, mode)) {
                return afatfs_lfnDelete(file);
            }
        } else {
            return AFATFS_OPERATION_FAILURE;
        }
    }

    return result;
}

#ifdef AFATFS_PIPELINE_ALLOCATION
#if defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) && !AFATFS_READ_SESSION_QUERY
AFATFS_INLINE
#elif defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
/**
 * @brief Test whether an active backend write blocks advancement of shared allocation barriers.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_allocationWriteInProgress(void)
{
    return afatfs.cacheFlushInProgress &&
        (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_READY ||
            afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_CLOSING);
}

/**
 * @brief Test whether a pending FAT32 allocation is eligible for grouping with another writer.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_groupableAllocation(const afatfsFile_t *file)
{
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
        file->type == AFATFS_FILE_TYPE_NORMAL &&
        (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER ||
            file->operation.active == AFATFS_FILE_OPERATION_COLLECT_SYNC ||
            file->operation.active == AFATFS_FILE_OPERATION_COLLECT_CLOSE);
}

/**
 * @brief Test whether a small-write allocation window can accept another cached request.
 * @param file Writer whose pending allocation is inspected.
 * @return True only while its requested EOCs are still being collected.
 */
static bool afatfs_collectWritable(const afatfsFile_t *file)
{
    return file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
        file->operation.state.appendFreeCluster.collecting &&
        file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
}

/** @brief Close every open collection window before an explicit durability barrier. */
static void afatfs_sealCollections(void)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
        if (afatfs_collectWritable(file)) {
            afatfs_fileUnlockCacheSector(file);
            file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
        }
    }
}

/**
 * @brief Keep collected EOCs buffered during opportunistic flushing; explicit barriers seal them.
 * @param sector Physical page considered for background submission.
 * @return True when a collecting writer owns EOCs in this FAT pair.
 */
static bool afatfs_collectFatPage(LBA_t sector)
{
    // Reject pages outside both FAT copies before scanning collecting writers.
    // Subtractions also preserve the range test when physical LBAs wrap.
    LBA_t fatOffset = sector - afatfs.fatStartSector;
    if (fatOffset >= afatfs.fatSectors && fatOffset - afatfs.fatSectors >= afatfs.fatSectors) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *file = &afatfs.openFiles[index];
        if (afatfs_collectWritable(file)) {
            uint32_t page = file->operation.state.appendFreeCluster.searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
            if (sector == afatfs_fatSectorToPhysical(0, page) || sector == afatfs_fatSectorToPhysical(1, page)) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Find another writer with a compatible allocation already pending.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_pendingAllocationPeer(const afatfsFile_t *file)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (&afatfs.openFiles[index] != file && afatfs_groupableAllocation(&afatfs.openFiles[index])) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Test whether at least two pending writers can share an allocation publication pass.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_pendingAllocationGroup(void)
{
#if defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT) || AFATFS_NUM_CACHE_SECTORS >= 8
    if (!afatfs.cacheDirtyEntries || afatfs.cacheFlushInProgress ||
        afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 || afatfs.sectorsPerCluster != 1) {
        return false;
    }
    bool foundWriter = false;
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->type == AFATFS_FILE_TYPE_NORMAL &&
            file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
            if (foundWriter) {
                return true;
            }
            foundWriter = true;
        }
    }
    return false;
#else
    if (!afatfs.cacheDirtyEntries || afatfs.cacheFlushInProgress) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (afatfs_groupableAllocation(&afatfs.openFiles[index])) {
            return afatfs_pendingAllocationPeer(&afatfs.openFiles[index]);
        }
    }
    return false;
#endif
}

// Called only after a shared allocation barrier has drained both FAT copies.
// Advance every waiter before modifying either page again: a later clean-page
// observation must never stand in for the ACK of an older submitted image.
/**
 * @brief Advance grouped waiters only after their shared FAT durability barrier has completed.
 */
static void afatfs_groupDurableAllocations(void)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *peer = &afatfs.openFiles[index];
        afatfsAppendFreeCluster_t *state = &peer->operation.state.appendFreeCluster;
        if (!afatfs_groupableAllocation(peer)) {
            continue;
        }
        if (state->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE) {
            state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
        } else if (state->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE) {
            if (state->wrapped) {
                state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS;
                continue;
            }
            state->phase = state->previousCluster ? AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2
                                                  : AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
        }
    }
}

/**
 * @brief Resolve an end-of-chain allocation already prepared by another pending writer.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_pendingAllocationEoc(const afatfsFile_t *file)
{
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *peer = &afatfs.openFiles[index];
        if (peer == file || !afatfs_groupableAllocation(peer)) {
            continue;
        }
        switch (peer->operation.state.appendFreeCluster.phase) {
            case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
            case AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN:
            case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
            case AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE:
                return true;
            default:
                break;
        }
    }
    return false;
}

/**
 * @brief Test whether both FAT copies can share another pending allocation before submission.
 * @param sector Relative FAT sector containing the unique look-ahead reservation.
 * @return True when both cached copies are dirty; false if either copy is missing or in another state.
 */
static bool afatfs_reservationPairDirty(uint32_t sector)
{
    unsigned found = 0;
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state != AFATFS_CACHE_STATE_DIRTY) {
            continue;
        }
        if (entry->sectorIndex == afatfs_fatSectorToPhysical(0, sector)) {
            found |= 1;
        } else if (entry->sectorIndex == afatfs_fatSectorToPhysical(1, sector)) {
            found |= 2;
        }
    }
    return found == 3;
}

enum {
    AFATFS_BATCH_HEADER_PRIMARY,
    AFATFS_BATCH_HEADER_MIRROR,
    AFATFS_BATCH_REPAIR_ALLOCATIONS,
    AFATFS_BATCH_REPAIR_LINKS,
    AFATFS_BATCH_WINDOW_START,
    AFATFS_BATCH_WINDOW_CHECK,
    AFATFS_BATCH_MARK_REFERENCES,
    AFATFS_BATCH_DIRECTORY_START,
    AFATFS_BATCH_DIRECTORY_NEXT,
    AFATFS_BATCH_DIRECTORY_PARENT,
    AFATFS_BATCH_SWEEP,
    AFATFS_BATCH_DRAIN,
    AFATFS_BATCH_CLEAN,
    AFATFS_BATCH_CLEAN_ACK
};

#define AFATFS_BATCH_CLEAN_BIT 0x08000000U
#define AFATFS_BATCH_WINDOW_CLUSTERS (AFATFS_SECTOR_SIZE * 8U)

/**
 * @brief Change only the standard clean-shutdown bit, preserving each FAT page independently.
 * @param clean True to publish a completed session; false before creating batch EOCs.
 * @return SUCCESS after buffering both changes, or IN_PROGRESS while cache/backend work is pending.
 */
static afatfsOperationStatus_e afatfs_batchSetClean(bool clean)
{
    if (afatfs.cacheFlushInProgress) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    for (unsigned copy = 0; copy < AFATFS_NUM_FATS; copy++) {
        uint8_t *buffer;
        afatfsOperationStatus_e status = afatfs_cacheSector(afatfs_fatSectorToPhysical(copy, 0),
            &buffer, AFATFS_CACHE_READ | AFATFS_CACHE_METADATA, 0);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        uint32_t *entries = (uint32_t *)buffer;
        uint32_t value = clean ? entries[1] | AFATFS_BATCH_CLEAN_BIT : entries[1] & ~AFATFS_BATCH_CLEAN_BIT;
        if (value != entries[1]) {
            afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer(buffer));
            entries[1] = value;
        }
    }
    return AFATFS_OPERATION_SUCCESS;
}

/**
 * @brief Update one recovery entry without replacing unrelated entries in the other FAT.
 * @param copy FAT copy to update.
 * @param cluster Cluster whose low 28 bits are changed.
 * @param value New FAT32 value; the reserved high nibble is preserved.
 * @return SUCCESS after buffering the entry, or the cache operation status.
 */
static afatfsOperationStatus_e afatfs_batchWriteEntry(unsigned copy, uint32_t cluster, uint32_t value)
{
    if (afatfs.cacheFlushInProgress) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    uint8_t *buffer;
    afatfsOperationStatus_e status = afatfs_cacheSector(
        afatfs_fatSectorToPhysical(copy, cluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR),
        &buffer, AFATFS_CACHE_READ | AFATFS_CACHE_METADATA, 0);
    if (status == AFATFS_OPERATION_SUCCESS) {
        uint32_t *entry = (uint32_t *)buffer + cluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
        uint32_t next = (*entry & 0xF0000000U) | (value & 0x0FFFFFFFU);
        if (*entry != next) {
            afatfs_cacheSectorMarkDirty(afatfs_getCacheDescriptorForBuffer(buffer));
            *entry = next;
        }
    }
    return status;
}

/**
 * @brief Test whether a low-28-bit FAT value is structurally representable on this volume.
 * @param value Decoded FAT entry.
 * @return True for free, reserved/bad, EOC or an in-range successor.
 */
static bool afatfs_batchValidValue(uint32_t value)
{
    return value == 0 || value >= 0x0FFFFFF0U ||
        (value >= FAT_SMALLEST_LEGAL_CLUSTER_NUMBER && value < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);
}

/**
 * @brief Protect a referenced cluster in the current recovery window.
 * @param state Mount-only state stored in the existing directory-operation union.
 * @param cluster Referenced cluster; values outside the current window are ignored.
 */
static void afatfs_batchProtect(afatfsBatchRecovery_t *state, uint32_t cluster)
{
    if (cluster >= state->windowStart && cluster - state->windowStart < AFATFS_BATCH_WINDOW_CLUSTERS) {
        uint32_t offset = cluster - state->windowStart;
        afatfs_cacheSectorGetMemory(state->bitmapIndex)[offset / 8] |= (uint8_t)(1U << (offset % 8));
    }
}

/**
 * @brief Reset an iterative directory traversal for one FAT copy, without a recursion stack.
 * @param state Recovery cursor and traversal limits.
 */
static void afatfs_batchStartDirectories(afatfsBatchRecovery_t *state)
{
    afatfs.currentDirectory.firstCluster = afatfs.rootDirectoryCluster;
    afatfs.currentDirectory.cursorCluster = afatfs.rootDirectoryCluster;
    state->directoryEntry = 0;
    state->resumeChild = 0;
    state->hops = 0;
    state->budget = 2 * (afatfs.numClusters + 1);
    afatfs_batchProtect(state, afatfs.rootDirectoryCluster);
    state->phase = AFATFS_BATCH_DIRECTORY_NEXT;
}

/**
 * @brief Visit one directory entry, using on-disk parent links to return from children.
 * @param state Existing mount scratch state; directory cursors live in the existing CWD handle.
 * @return SUCCESS when this FAT copy's tree is complete, IN_PROGRESS while traversing, or FAILURE on corruption.
 */
static afatfsOperationStatus_e afatfs_batchDirectoryNext(afatfsBatchRecovery_t *state)
{
    uint8_t *buffer;
    if (state->phase == AFATFS_BATCH_DIRECTORY_PARENT) {
        afatfsOperationStatus_e status = afatfs_cacheSector(
            afatfs_fileClusterToPhysical(afatfs.currentDirectory.firstCluster, 0), &buffer, AFATFS_CACHE_READ, 0);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        const fatDirectoryEntry_t *parentEntry = (const fatDirectoryEntry_t *)buffer + 1;
        if (parentEntry->fileName[0] != '.' || parentEntry->fileName[1] != '.' ||
            !(parentEntry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) || state->budget == 0) {
            return AFATFS_OPERATION_FAILURE;
        }
        uint32_t parent = (((uint32_t)parentEntry->firstClusterHigh << 16) | parentEntry->firstClusterLow) & 0x0FFFFFFFU;
        if (!parent) {
            parent = afatfs.rootDirectoryCluster;
        }
        if (parent < 2 || parent >= afatfs.numClusters + 2 || parent == afatfs.currentDirectory.firstCluster) {
            return AFATFS_OPERATION_FAILURE;
        }
        state->budget--;
        state->resumeChild = afatfs.currentDirectory.firstCluster;
        afatfs.currentDirectory.firstCluster = parent;
        afatfs.currentDirectory.cursorCluster = parent;
        state->directoryEntry = 0;
        state->hops = 0;
        state->phase = AFATFS_BATCH_DIRECTORY_NEXT;
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (state->directoryEntry == AFATFS_SECTOR_SIZE / FAT_DIRECTORY_ENTRY_SIZE) {
        uint32_t next;
        afatfsOperationStatus_e status = afatfs_FATGetNextCluster(state->tableCopy,
            afatfs.currentDirectory.cursorCluster, &next);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        if (fat32_isEndOfChainMarker(next)) {
            if (state->resumeChild) {
                return AFATFS_OPERATION_FAILURE;
            }
            if (afatfs.currentDirectory.firstCluster == afatfs.rootDirectoryCluster) {
                return AFATFS_OPERATION_SUCCESS;
            }
            state->phase = AFATFS_BATCH_DIRECTORY_PARENT;
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (next < 2 || next >= afatfs.numClusters + 2 || ++state->hops > afatfs.numClusters) {
            return AFATFS_OPERATION_FAILURE;
        }
        afatfs.currentDirectory.cursorCluster = next;
        state->directoryEntry = 0;
    }
    afatfsOperationStatus_e status = afatfs_cacheSector(
        afatfs_fileClusterToPhysical(afatfs.currentDirectory.cursorCluster, 0), &buffer, AFATFS_CACHE_READ, 0);
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    const fatDirectoryEntry_t *entry = (const fatDirectoryEntry_t *)buffer + state->directoryEntry;
    if (entry->fileName[0] == 0) {
        state->directoryEntry = AFATFS_SECTOR_SIZE / FAT_DIRECTORY_ENTRY_SIZE;
        // A zero entry ends this directory, even if allocated slack follows it.
        if (state->resumeChild) {
            return AFATFS_OPERATION_FAILURE;
        }
        if (afatfs.currentDirectory.firstCluster == afatfs.rootDirectoryCluster) {
            return AFATFS_OPERATION_SUCCESS;
        }
        state->phase = AFATFS_BATCH_DIRECTORY_PARENT;
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (entry->fileName[0] == FAT_DELETED_FILE_MARKER || (entry->attrib & FAT_FILE_ATTRIBUTE_VOLUME_ID)) {
        state->directoryEntry++;
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    uint32_t first = (((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow) & 0x0FFFFFFFU;
    bool directory = (entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool dot = entry->fileName[0] == '.' && (entry->fileName[1] == ' ' ||
        (entry->fileName[1] == '.' && entry->fileName[2] == ' '));
    if (!first && dot) {
        first = afatfs.rootDirectoryCluster;
    }
    if ((!first && (directory || entry->fileSize)) || (first && (first < 2 || first >= afatfs.numClusters + 2))) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (first) {
        afatfs_batchProtect(state, first);
        uint32_t next;
        status = afatfs_FATGetNextCluster(state->tableCopy, first, &next);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        if (!next || !afatfs_batchValidValue(next) || (next >= 0x0FFFFFF0U && next < 0x0FFFFFF8U)) {
            return AFATFS_OPERATION_FAILURE;
        }
    }
    state->directoryEntry++;
    if (state->resumeChild) {
        if (directory && !dot && first == state->resumeChild) {
            state->resumeChild = 0;
        }
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (directory && !dot) {
        if (!state->budget) {
            return AFATFS_OPERATION_FAILURE;
        }
        state->budget--;
        afatfs.currentDirectory.firstCluster = first;
        afatfs.currentDirectory.cursorCluster = first;
        state->directoryEntry = 0;
        state->hops = 0;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * @brief Recover unreferenced allocation heads after an interrupted dirty batch session.
 * @return SUCCESS after mirrored recovery and clean-bit ACKs, IN_PROGRESS while polling, or FAILURE on corruption.
 * @details Uses one existing cache sector as a 4096-cluster incoming-reference bitmap. Directory traversal is
 * iterative; both FATs and all directory first-cluster references protect live allocations. Only unreferenced
 * heads are freed, and both copies are acknowledged before a new reference scan can free their successors.
 */
static afatfsOperationStatus_e afatfs_batchRecoverContinue(void)
{
    afatfsBatchRecovery_t *state = &afatfs.currentDirectory.operation.state.batchRecovery;
    if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 || afatfs.sectorsPerCluster != 1) {
        return AFATFS_OPERATION_SUCCESS;
    }
    for (unsigned work = 0; work < 32; work++) {
        if (afatfs.cacheFlushInProgress) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        uint8_t *buffer;
        afatfsOperationStatus_e status;
        switch (state->phase) {
            case AFATFS_BATCH_HEADER_PRIMARY:
            case AFATFS_BATCH_HEADER_MIRROR:
                status = afatfs_cacheSector(afatfs_fatSectorToPhysical(
                    state->phase == AFATFS_BATCH_HEADER_PRIMARY ? 0 : 1, 0), &buffer, AFATFS_CACHE_READ, 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                afatfs.batchDirtyMarked |= (((uint32_t *)buffer)[1] & AFATFS_BATCH_CLEAN_BIT) == 0;
                if (state->phase++ == AFATFS_BATCH_HEADER_PRIMARY) {
                    break;
                }
                if (!afatfs.batchDirtyMarked) {
                    return AFATFS_OPERATION_SUCCESS;
                }
                state->bitmapIndex = UINT8_MAX;
                for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
                    afatfsCacheBlockDescriptor_t *descriptor = &afatfs.cacheDescriptor[index];
                    if (descriptor->state == AFATFS_CACHE_STATE_EMPTY ||
                        (descriptor->state == AFATFS_CACHE_STATE_IN_SYNC && !descriptor->locked && !descriptor->retainCount)) {
                        state->bitmapIndex = (uint8_t)index;
                        break;
                    }
                }
                if (state->bitmapIndex == UINT8_MAX) {
                    return AFATFS_OPERATION_FAILURE;
                }
                afatfs_cacheSectorInit(&afatfs.cacheDescriptor[state->bitmapIndex], UINT32_MAX, true);
                afatfs.cacheDescriptor[state->bitmapIndex].state = AFATFS_CACHE_STATE_IN_SYNC;
                afatfs.cacheDescriptor[state->bitmapIndex].retainCount = 1;
                state->candidate = 2;
                state->phase = AFATFS_BATCH_REPAIR_ALLOCATIONS;
                break;
            case AFATFS_BATCH_REPAIR_ALLOCATIONS:
            case AFATFS_BATCH_REPAIR_LINKS: {
                if (state->candidate >= afatfs.numClusters + 2) {
                    state->candidate = 2;
                    state->nextPhase = state->phase == AFATFS_BATCH_REPAIR_ALLOCATIONS
                        ? AFATFS_BATCH_REPAIR_LINKS : AFATFS_BATCH_WINDOW_START;
                    state->phase = AFATFS_BATCH_DRAIN;
                    break;
                }
                uint32_t primary;
                uint32_t mirror;
                status = afatfs_FATGetNextCluster(0, state->candidate, &primary);
                if (status == AFATFS_OPERATION_SUCCESS) {
                    status = afatfs_FATGetNextCluster(1, state->candidate, &mirror);
                }
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!afatfs_batchValidValue(primary)) {
                    return AFATFS_OPERATION_FAILURE;
                }
                if (primary && primary != mirror) {
                    uint32_t value = primary;
                    if (state->phase == AFATFS_BATCH_REPAIR_ALLOCATIONS) {
                        if (mirror && afatfs_batchValidValue(mirror) &&
                            !(mirror >= 0x0FFFFFF0U && mirror < 0x0FFFFFF8U)) {
                            state->candidate++;
                            break;
                        }
                        if (primary < 0x0FFFFFF0U || fat32_isEndOfChainMarker(primary)) {
                            value = 0x0FFFFFFFU;
                        }
                    }
                    status = afatfs_batchWriteEntry(1, state->candidate, value);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                }
                state->candidate++;
                break;
            }
            case AFATFS_BATCH_WINDOW_START:
                if (state->windowStart >= afatfs.numClusters + 2) {
                    if (state->passChanged) {
                        state->passChanged = false;
                        state->windowStart = 0;
                    } else {
                        state->phase = AFATFS_BATCH_CLEAN;
                        break;
                    }
                }
                memset(afatfs_cacheSectorGetMemory(state->bitmapIndex), 0, AFATFS_SECTOR_SIZE);
                state->windowAllocated = false;
                state->windowChanged = false;
                state->tableCopy = 0;
                state->tableSector = state->windowStart / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                state->phase = AFATFS_BATCH_WINDOW_CHECK;
                break;
            case AFATFS_BATCH_WINDOW_CHECK:
            case AFATFS_BATCH_MARK_REFERENCES: {
                uint32_t limit = state->phase == AFATFS_BATCH_WINDOW_CHECK
                    ? MIN(afatfs.fatSectors, (state->windowStart + AFATFS_BATCH_WINDOW_CLUSTERS) / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR)
                    : afatfs.fatSectors;
                if (state->tableSector >= limit) {
                    if (++state->tableCopy < AFATFS_NUM_FATS) {
                        state->tableSector = state->phase == AFATFS_BATCH_WINDOW_CHECK
                            ? state->windowStart / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR : 0;
                    } else if (state->phase == AFATFS_BATCH_WINDOW_CHECK) {
                        if (!state->windowAllocated) {
                            state->windowStart += AFATFS_BATCH_WINDOW_CLUSTERS;
                            state->phase = AFATFS_BATCH_WINDOW_START;
                        } else {
                            state->tableCopy = 0;
                            state->tableSector = 0;
                            state->phase = AFATFS_BATCH_MARK_REFERENCES;
                        }
                    } else {
                        state->tableCopy = 0;
                        state->phase = AFATFS_BATCH_DIRECTORY_START;
                    }
                    break;
                }
                status = afatfs_cacheSector(afatfs_fatSectorToPhysical(state->tableCopy, state->tableSector),
                    &buffer, AFATFS_CACHE_READ, 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                for (unsigned index = 0; index < AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR; index++) {
                    uint32_t cluster = state->tableSector * AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR + index;
                    if (cluster < 2 || cluster >= afatfs.numClusters + 2) {
                        continue;
                    }
                    uint32_t value = ((uint32_t *)buffer)[index] & 0x0FFFFFFFU;
                    if (state->phase == AFATFS_BATCH_WINDOW_CHECK) {
                        if (value && cluster != afatfs.rootDirectoryCluster &&
                            !(value >= 0x0FFFFFF0U && value < 0x0FFFFFF8U)) {
                            state->windowAllocated = true;
                        }
                    } else if (value >= 2 && value < afatfs.numClusters + 2) {
                        afatfs_batchProtect(state, value);
                    }
                }
                state->tableSector++;
                break;
            }
            case AFATFS_BATCH_DIRECTORY_START:
                afatfs_batchStartDirectories(state);
                break;
            case AFATFS_BATCH_DIRECTORY_NEXT:
            case AFATFS_BATCH_DIRECTORY_PARENT:
                status = afatfs_batchDirectoryNext(state);
                if (status == AFATFS_OPERATION_FAILURE) {
                    return status;
                }
                if (status == AFATFS_OPERATION_SUCCESS) {
                    if (++state->tableCopy < AFATFS_NUM_FATS) {
                        state->phase = AFATFS_BATCH_DIRECTORY_START;
                    } else {
                        state->candidate = MAX(2U, state->windowStart);
                        state->phase = AFATFS_BATCH_SWEEP;
                    }
                }
                // Yield after a possible directory read, keeping tree traversal bounded.
                return AFATFS_OPERATION_IN_PROGRESS;
            case AFATFS_BATCH_SWEEP: {
                if (state->candidate >= afatfs.numClusters + 2 ||
                    state->candidate - state->windowStart >= AFATFS_BATCH_WINDOW_CLUSTERS) {
                    if (!state->windowChanged) {
                        state->windowStart += AFATFS_BATCH_WINDOW_CLUSTERS;
                    }
                    state->nextPhase = AFATFS_BATCH_WINDOW_START;
                    state->phase = AFATFS_BATCH_DRAIN;
                    break;
                }
                uint32_t offset = state->candidate - state->windowStart;
                if (afatfs_cacheSectorGetMemory(state->bitmapIndex)[offset / 8] & (1U << (offset % 8))) {
                    state->candidate++;
                    break;
                }
                uint32_t primary;
                uint32_t mirror;
                status = afatfs_FATGetNextCluster(0, state->candidate, &primary);
                if (status == AFATFS_OPERATION_SUCCESS) {
                    status = afatfs_FATGetNextCluster(1, state->candidate, &mirror);
                }
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if ((primary || mirror) && !(primary >= 0x0FFFFFF0U && primary < 0x0FFFFFF8U) &&
                    !(mirror >= 0x0FFFFFF0U && mirror < 0x0FFFFFF8U)) {
                    status = afatfs_batchWriteEntry(0, state->candidate, 0);
                    if (status == AFATFS_OPERATION_SUCCESS) {
                        status = afatfs_batchWriteEntry(1, state->candidate, 0);
                    }
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    state->windowChanged = true;
                    state->passChanged = true;
                }
                state->candidate++;
                break;
            }
            case AFATFS_BATCH_DRAIN:
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                state->phase = state->nextPhase;
                break;
            case AFATFS_BATCH_CLEAN:
                status = afatfs_batchSetClean(true);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                state->phase = AFATFS_BATCH_CLEAN_ACK;
                break;
            case AFATFS_BATCH_CLEAN_ACK:
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                afatfs.cacheDescriptor[state->bitmapIndex].state = AFATFS_CACHE_STATE_EMPTY;
                afatfs.cacheDescriptor[state->bitmapIndex].locked = false;
                afatfs.cacheDescriptor[state->bitmapIndex].retainCount = 0;
                afatfs.batchDirtyMarked = false;
                afatfs.batchMarkingClean = false;
                afatfs.lastClusterAllocated = FAT_SMALLEST_LEGAL_CLUSTER_NUMBER;
                afatfs_chdir(NULL);
                return AFATFS_OPERATION_SUCCESS;
            default:
                return AFATFS_OPERATION_FAILURE;
        }
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * @brief Publish a clean session only when no other operation can still own unlinked batch targets.
 * @param file Handle being synchronized.
 * @param phase Existing sync/close sub-operation phase; phases four through six publish the clean marker.
 * @return SUCCESS once clean or safely left dirty, otherwise IN_PROGRESS until both marker ACKs arrive.
 */
static afatfsOperationStatus_e afatfs_batchFinishSync(const afatfsFile_t *file, uint8_t *phase)
{
    if (!afatfs.batchDirtyMarked || afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32) {
        return AFATFS_OPERATION_SUCCESS;
    }
    // fsync confirms every byte and the exact directory image below. Keep a
    // collected session conservatively dirty until close, avoiding a clean /
    // dirty FAT-header round trip at every small durability point.
    if (afatfs.batchCollectedSession && file->operation.active == AFATFS_FILE_OPERATION_SYNC) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (*phase == 4) {
        for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
            const afatfsFile_t *peer = &afatfs.openFiles[index];
            if (peer != file && peer->operation.active != AFATFS_FILE_OPERATION_NONE) {
                return AFATFS_OPERATION_SUCCESS;
            }
        }
        if (afatfs.currentDirectory.operation.active != AFATFS_FILE_OPERATION_NONE) {
            return AFATFS_OPERATION_SUCCESS;
        }
        afatfs.batchMarkingClean = true;
        *phase = 5;
    }
    if (*phase == 5) {
        afatfsOperationStatus_e status = afatfs_batchSetClean(true);
        if (status != AFATFS_OPERATION_SUCCESS) {
            return status;
        }
        *phase = 6;
    }
    if (!afatfs_flushCache()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    afatfs.batchDirtyMarked = false;
    afatfs.batchMarkingClean = false;
    afatfs.batchCollectedSession = false;
    return AFATFS_OPERATION_SUCCESS;
}

/**
 * @brief Initialize only clusters covered by the current write request, within one FAT page.
 * @param file Writer whose existing append state stores the requested run and progress.
 * @return SUCCESS after preparing EOCs, IN_PROGRESS while waiting, or FAILURE on allocation failure.
 */
static afatfsOperationStatus_e afatfs_prepareRequestedBatch(afatfsFile_t *file)
{
    if (afatfs.cacheFlushInProgress || afatfs.batchMarkingClean) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    afatfsAppendFreeCluster_t *state = &file->operation.state.appendFreeCluster;
    bool ownsReservation = (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) != 0;
    uint32_t stride = 1;
    uint32_t firstFree = 0;
    if (ownsReservation) {
        if (!afatfs_flushAllocationCache(file)) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        afatfs_groupDurableAllocations();
        state->searchCluster = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
    } else {
        if (state->collecting &&
            state->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR ==
                afatfs.lastClusterAllocated / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR) {
            state->searchCluster = MIN(state->searchCluster, afatfs.lastClusterAllocated);
        }
        afatfsFindClusterStatus_e found = afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE,
            &state->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);
        if (found == AFATFS_FIND_CLUSTER_IN_PROGRESS) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        if (found != AFATFS_FIND_CLUSTER_FOUND) {
            state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
            return AFATFS_OPERATION_FAILURE;
        }
    }
    if (state->collecting) {
        firstFree = state->searchCluster;
        uint32_t writers = 0;
        uint32_t lane = 0;
        for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
            const afatfsFile_t *peer = &afatfs.openFiles[index];
            if (peer->type == AFATFS_FILE_TYPE_NORMAL &&
                (peer->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND))) {
                if (peer < file) {
                    lane++;
                }
                writers++;
            }
        }
        stride = MIN(writers, 3);
        lane %= stride;
        if (!ownsReservation && stride > 1) {
            uint32_t candidate = firstFree;
            uint32_t limit = MIN(afatfs.numClusters + 2,
                (firstFree / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR + 1) * AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
            for (; candidate < limit; candidate++) {
                uint32_t value;
                if (candidate % stride == lane && afatfs_FATGetNextCluster(0, candidate, &value) == AFATFS_OPERATION_SUCCESS &&
                    fat_isFreeSpace(value) && !afatfs_clusterIsReserved(candidate)) {
                    break;
                }
            }
            if (candidate < limit) {
                state->searchCluster = candidate;
            } else {
                stride = 1;
            }
        }
        if (!ownsReservation) {
            afatfs.lastClusterAllocated = firstFree;
        }
    }
    afatfsFATSector_t sector;
    uint8_t *mirror;
    afatfsOperationStatus_e status = afatfs_FATWriteSector(
        state->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR, &sector, &mirror,
        AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, AFATFS_FAT_MIRROR_STATE(NULL) 0);
    if (status != AFATFS_OPERATION_SUCCESS) {
        return status;
    }
    uint32_t count = 0;
    uint32_t offset = state->searchCluster % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
    uint32_t limit = MIN(state->progress.zeroSector, AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR - offset);
    limit = MIN(limit, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER - state->searchCluster);
    limit = MIN(limit, (UINT32_MAX - file->physicalSize) / AFATFS_SECTOR_SIZE);
    while (count < limit) {
        uint32_t next = sector.fat32[offset + count];
        if (!(ownsReservation && count == 0) && !fat_isFreeSpace(next & 0x0FFFFFFF)) {
            break;
        }
        sector.fat32[offset + count] = (next & 0xF0000000) | 0x0FFFFFFF;
        count++;
    }
    memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
    if (!count) {
        state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
        return AFATFS_OPERATION_FAILURE;
    }
    state->progress.zeroSector = state->searchCluster + count;
    afatfs.lastClusterAllocated = state->collecting ? firstFree : state->progress.zeroSector - 1;
    if (state->collecting) {
        state->progress.zeroSector |= (stride - 1) << 28;
    }
    file->cursorCluster = state->searchCluster;
    file->physicalSize += count * AFATFS_SECTOR_SIZE;
#ifdef AFATFS_PIPELINE_ALLOCATION
    file->directoryExact = false;
#endif
    if (!state->previousCluster) {
        file->firstCluster = state->searchCluster;
    }
    state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
    return AFATFS_OPERATION_SUCCESS;
}

/**
 * @brief Add only the next cluster now needed by an arriving small request to an open EOC window.
 * @param file Writer at the end of its already requested allocation.
 * @return True once the requested cluster is prepared; false while busy or after sealing the window.
 */
static bool afatfs_collectNext(afatfsFile_t *file)
{
    afatfsAppendFreeCluster_t *state = &file->operation.state.appendFreeCluster;
    uint32_t stride = (state->progress.zeroSector >> 28) + 1;
    uint32_t last = (state->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
    uint32_t candidate = last + stride;
    if ((last - state->searchCluster) / stride + 1 >= AFATFS_SMALL_WRITE_WINDOW_CLUSTERS ||
        candidate >= afatfs.numClusters + 2 ||
        candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR != state->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR ||
        (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) || file->physicalSize > UINT32_MAX - AFATFS_SECTOR_SIZE) {
        afatfs_sealCollections();
        return false;
    }
    afatfsFATSector_t sector;
    uint8_t *mirror;
    uint32_t offset = candidate % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
    LBA_t primarySector = afatfs_fatSectorToPhysical(0, candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    int primaryIndex = -1;
    int mirrorIndex = -1;
    for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state != AFATFS_CACHE_STATE_DIRTY) {
            continue;
        }
        if (entry->sectorIndex == primarySector) {
            primaryIndex = index;
            if (mirrorIndex >= 0) {
                break;
            }
        } else if (entry->sectorIndex == primarySector + afatfs.fatSectors) {
            mirrorIndex = index;
            if (primaryIndex >= 0) {
                break;
            }
        }
    }
    if (primaryIndex >= 0 && mirrorIndex >= 0) {
        // An already-dirty native pair has identical writable images. Neither
        // page can be evicted or backend-owned. Preserve the generic path's
        // read, primary acquisition and mirror acquisition access accounting.
        sector.bytes = afatfs_cacheSectorGetMemory(primaryIndex);
        mirror = afatfs_cacheSectorGetMemory(mirrorIndex);
        afatfs.cacheDescriptor[primaryIndex].accessTimestamp = ++afatfs.cacheTimer;
        if (!fat_isFreeSpace(sector.fat32[offset] & 0x0FFFFFFFU) || afatfs_clusterIsReserved(candidate)) {
            afatfs_sealCollections();
            return false;
        }
        afatfs.cacheDescriptor[primaryIndex].accessTimestamp = ++afatfs.cacheTimer;
        afatfs.cacheDescriptor[mirrorIndex].accessTimestamp = ++afatfs.cacheTimer;
        afatfs.fsInfoDirty = true;
        sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | 0x0FFFFFFFU;
        memcpy(mirror + offset * sizeof(uint32_t), &sector.fat32[offset], sizeof(uint32_t));
    } else {
        uint32_t value;
        if (afatfs_FATGetNextCluster(0, candidate, &value) != AFATFS_OPERATION_SUCCESS) {
            return false;
        }
        if (!fat_isFreeSpace(value) || afatfs_clusterIsReserved(candidate)) {
            afatfs_sealCollections();
            return false;
        }
        if (afatfs_FATWriteSector(candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR, &sector, &mirror,
                AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, AFATFS_FAT_MIRROR_STATE(NULL) 0) !=
            AFATFS_OPERATION_SUCCESS) {
            return false;
        }
        sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | 0x0FFFFFFFU;
        memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
    }
    state->progress.zeroSector = (state->progress.zeroSector & ~AFATFS_CLUSTER_HINT_MASK) | (candidate + 1);
    file->cursorCluster = candidate;
    file->physicalSize += AFATFS_SECTOR_SIZE;
    return true;
}
#endif

/**
 * @brief Advance single-cluster allocation while ordering data, both FAT copies and publication.
 *
 * @details Attempt to add a free cluster to the end of the given file. If the file was previously empty, the
 * directory entry
 * is updated to point to the new cluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The cluster has been appended
 *     AFATFS_OPERATION_IN_PROGRESS - Cache was busy, so call again later to continue
 *     AFATFS_OPERATION_FAILURE     - Cluster could not be appended because the filesystem ran out of space
 *                                    (afatfs.filesystemFull is set to true)
 *
 * If the file's operation was AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER, the file operation is cleared upon
 * completion,
 * otherwise it is left alone so that this operation can be called as a sub-operation of some other operation on
 * the
 * file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeClusterContinue(afatfsFile_t *file)
{
    afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;
    afatfsOperationStatus_e status;
#ifdef AFATFS_ENABLE_FAT_JOURNAL
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.blockAllocation &&
        opState->phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#endif
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
#ifdef AFATFS_PIPELINE_ALLOCATION
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT:
            if (afatfs.cacheFlushInProgress) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (opState->collectIdle == 63) {
                afatfs_sealCollections();
                goto doMore;
            }
            opState->collectIdle++;
            return AFATFS_OPERATION_IN_PROGRESS;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND:
            if (afatfs.batchMarkingClean) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (!afatfs.batchDirtyMarked) {
                status = afatfs_batchSetClean(false);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_MARK_DURABLE;
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            status = afatfs_prepareRequestedBatch(file);
            if (status == AFATFS_OPERATION_SUCCESS) {
                if (opState->collecting && file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
                    afatfs.batchCollectedSession = true;
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT;
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                if (afatfs_pendingAllocationPeer(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                goto doMore;
            }
            if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
                goto doMore;
            }
            return AFATFS_OPERATION_IN_PROGRESS;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_MARK_DURABLE:
            if (!afatfs_flushAllocationCache(file)) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            afatfs.batchDirtyMarked = true;
            afatfs_groupDurableAllocations();
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_LINKS:
            if (afatfs.cacheFlushInProgress || afatfs.batchMarkingClean) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            // Every destination EOC has been acknowledged in both FATs.
            if ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                afatfsFATSector_t sector;
                uint8_t *mirror;
                status = afatfs_FATWriteSector(opState->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR,
                    &sector, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA,
                    AFATFS_FAT_MIRROR_STATE(NULL) 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                // The requested window stays in one FAT page. Its destination
                // EOCs are durable in both copies before any link is prepared.
                while ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                    uint32_t stride = (opState->progress.zeroSector >> 28) + 1;
                    uint32_t last = (opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
                    uint32_t offset = (last - stride) % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                    sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | last;
                    opState->progress.zeroSector -= stride;
                }
                memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
            }
            if (opState->previousCluster) {
                status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
            }
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
            if (afatfs_pendingAllocationPeer(file)) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            goto doMore;
#endif
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (file->type != AFATFS_FILE_TYPE_NORMAL && afatfs_releaseReservedCluster() != AFATFS_OPERATION_SUCCESS) {
                return AFATFS_OPERATION_IN_PROGRESS;
            }
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && file->type == AFATFS_FILE_TYPE_NORMAL &&
                (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER)) {
                uint32_t reserved = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
                uint32_t sector = reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
                    const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
                    if ((entry->sectorIndex == afatfs_fatSectorToPhysical(0, sector) ||
                            entry->sectorIndex == afatfs_fatSectorToPhysical(1, sector)) &&
                        entry->state != AFATFS_CACHE_STATE_EMPTY && entry->state != AFATFS_CACHE_STATE_IN_SYNC) {
                        if (!afatfs.cacheFlushInProgress && afatfs_pendingAllocationPeer(file) && afatfs_reservationPairDirty(sector)) {
                            // Keep the unique look-ahead reservation; allocate this already requested peer normally.
                            goto findFreshBatchCluster;
                        }
                        // Keep the old directory image buffered while both reservation FAT pages drain.
                        afatfs_flushAllocationCache(file);
                        return AFATFS_OPERATION_IN_PROGRESS;
                    }
                }
                afatfs.lastClusterAllocated = reserved;
                opState->searchCluster = reserved;
                file->cursorCluster = reserved;
                file->physicalSize += afatfs_clusterSize();
                if (!opState->previousCluster) {
                    file->firstCluster = reserved;
                }
                // This EOC is already durable in both copies. Retain the link and
                // directory barriers; only the redundant EOC write is skipped.
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY;
                goto doMore;
            }
#endif
#ifdef AFATFS_PIPELINE_ALLOCATION
findFreshBatchCluster:
#endif
            switch (afatfs_findClusterWithCondition(
                CLUSTER_SEARCH_FREE, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                case AFATFS_FIND_CLUSTER_FOUND:
#ifdef AFATFS_PIPELINE_ALLOCATION
                    if (!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER)) {
                        afatfs.lastClusterAllocated = opState->searchCluster;
                    }
#else
                    afatfs.lastClusterAllocated = opState->searchCluster;
#endif

                    // Make the cluster available for us to write in
                    file->cursorCluster = opState->searchCluster;
                    file->physicalSize += afatfs_clusterSize();

                    if (opState->previousCluster == 0) {
                        // This is the new first cluster in the file
                        file->firstCluster = opState->searchCluster;
                    }

                    opState->progress.convertCluster = file->firstCluster;
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN;
                    goto doMore;
                    break;
                case AFATFS_FIND_CLUSTER_NOT_FOUND:
#ifdef AFATFS_PIPELINE_ALLOCATION
                    if (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) {
                        // The speculative reservation is still available after its ACKs.
                        // A failed fresh-peer search must not turn that space into ENOSPC.
                        opState->searchCluster = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
                        afatfs_flushAllocationCache(file);
                        return AFATFS_OPERATION_IN_PROGRESS;
                    }
#endif
                    // Fall through
                case AFATFS_FIND_CLUSTER_FATAL:
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
                while (opState->progress.convertCluster < opState->previousCluster) {
                    status = afatfs_FATSetNextCluster(
                        opState->progress.convertCluster, opState->progress.convertCluster + 1);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    ++opState->progress.convertCluster;
                }
                file->exfatNoFatChain = false;
            }
#endif
            opState->progress.zeroSector = 0;
            opState->phase = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                ? AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY
                : AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY:
            if (file->type == AFATFS_FILE_TYPE_DIRECTORY) {
                while (opState->progress.zeroSector < afatfs.sectorsPerCluster) {
                    uint8_t *sector;
                    status = afatfs_cacheSector(
                        afatfs_fileClusterToPhysical(opState->searchCluster, opState->progress.zeroSector), &sector,
                        AFATFS_CACHE_WRITE, 0);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    memset(sector, 0, AFATFS_SECTOR_SIZE);
                    ++opState->progress.zeroSector;
                }
                if (!afatfs_flushCache()) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
            }
            opState->phase = afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                ? AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1
                : opState->previousCluster ? AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2
                                           : AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
            // Terminate the new cluster
            status = afatfs_FATSetNextCluster(opState->searchCluster, 0xFFFFFFFF);

            if (status == AFATFS_OPERATION_SUCCESS) {
#ifdef AFATFS_PIPELINE_ALLOCATION
                // Prime the single look-ahead EOC while this new FAT sector is
                // already writable, including the first cluster after a boundary.
                // The existing durability phase still confirms both EOCs before use.
                afatfs_reserveNextCluster(file);
#endif
                if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT) {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
                } else if (opState->previousCluster) {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
                } else {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
                }

#ifdef AFATFS_PIPELINE_ALLOCATION
                if (afatfs_groupableAllocation(file) && afatfs_pendingAllocationPeer(file)) {
                    // Yield once so already pending peers can prepare their
                    // EOCs before the poll's background flush. Never wait for
                    // a future request or weaken the following ACK barrier.
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
#endif
                goto doMore;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (!afatfs_flushAllocationCache(file)) {
#else
            if (!afatfs_allocationMetadataBuffered(file) && !afatfs_flushCache()) {
#endif
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (afatfs_groupableAllocation(file)) {
                afatfs_groupDurableAllocations();
                goto doMore;
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (afatfs_groupableAllocation(file) && afatfs_pendingAllocationEoc(file)) {
                // A peer has already requested an allocation. Give its EOC
                // barrier a chance to finish before submitting our ready link.
                if (!afatfs_flushAllocationCache(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
                afatfs_groupDurableAllocations();
                if (afatfs_pendingAllocationEoc(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
            }
#endif
            // Add the new cluster to the pre-existing chain
            status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
#ifdef AFATFS_PIPELINE_ALLOCATION
                afatfs_reserveNextCluster(file);
#endif
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE;
#ifdef AFATFS_PIPELINE_ALLOCATION
                if (afatfs_groupableAllocation(file) && afatfs_pendingAllocationPeer(file)) {
                    return AFATFS_OPERATION_IN_PROGRESS;
                }
#endif
                goto doMore;
            }
            break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAT_DURABLE:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (!afatfs_flushAllocationCache(file)) {
#else
            if (!afatfs_allocationMetadataBuffered(file) && !afatfs_flushCache()) {
#endif
                return AFATFS_OPERATION_IN_PROGRESS;
            }
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (afatfs_groupableAllocation(file)) {
                afatfs_groupDurableAllocations();
                goto doMore;
            }
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
            if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BITMAP;
                goto doMore;
            }
#endif
            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
            goto doMore;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
#ifdef AFATFS_PIPELINE_ALLOCATION
            if (file->operation.active == AFATFS_FILE_OPERATION_COLLECT_SYNC ||
                file->operation.active == AFATFS_FILE_OPERATION_COLLECT_CLOSE) {
                // The queued durability point will publish the exact size once.
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                goto doMore;
            }
#endif
            if (afatfs_saveDirectoryEntry(file,
#ifdef AFATFS_PIPELINE_ALLOCATION
                    opState->collecting ? AFATFS_SAVE_DIRECTORY_FOR_CLOSE :
#endif
                    AFATFS_SAVE_DIRECTORY_NORMAL) == AFATFS_OPERATION_SUCCESS) {
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

/**
 * @brief Initialize the per-file workspace for a single-cluster append.
 * @param state Per-file allocation workspace to initialize.
 * @param previousCluster Previous chain tail, or zero when allocating the first cluster.
 */
static void afatfs_appendRegularFreeClusterInitOperationState(
    afatfsAppendFreeCluster_t *state, uint32_t previousCluster)
{
    state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_INITIAL;
    state->previousCluster = previousCluster;
    state->searchCluster = afatfs.lastClusterAllocated;
#ifdef AFATFS_PIPELINE_ALLOCATION
    state->searchCluster &= AFATFS_CLUSTER_HINT_MASK;
#endif
    state->wrapped = false;
#ifdef AFATFS_PIPELINE_ALLOCATION
    state->collecting = false;
    state->collectIdle = 0;
#endif
}

/**
 * @brief Start or resume appending one regular cluster to a file.
 *
 * @details Queue up an operation to append a free cluster to the file and update the file's cursorCluster to point
 * to it.
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
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeCluster(afatfsFilePtr_t file)
{
    if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    if ((afatfs.filesystemFull
#ifdef AFATFS_PIPELINE_ALLOCATION
            && !(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER)
#endif
                ) ||
        afatfs_fileIsBusy(file)) {
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
 * @brief Return the byte span represented by one FAT-sector-sized freefile supercluster.
 *
 * @details Size of a AFATFS supercluster in bytes
 * @return Supercluster size in bytes.
 */
ONLY_EXPOSE_FOR_TESTING
LBA_t afatfs_superClusterSize()
{
    return afatfs_fatEntriesPerSector() * afatfs_clusterSize();
}

/**
 * @brief Advance allocation of a contiguous supercluster from the internal freefile.
 *
 * @details Continue to attempt to add a supercluster to the end of the given file.
 *
 * If the file operation was set to AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER and the operation completes, the
 * file's
 * operation is cleared.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On completion
 *     AFATFS_OPERATION_IN_PROGRESS - Operation still in progress
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Start or resume a freefile supercluster append.
 *
 * @details Attempt to queue up an operation to append the first supercluster of the freefile to the given `file`
 * (file's cursor
 * must be at end-of-file).
 *
 * The new cluster number will be set into the file's cursorCluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The append completed successfully and the file's cursorCluster has been
 * updated
 *     AFATFS_OPERATION_IN_PROGRESS - The operation was queued on the file and will complete later, or there is
 * already an
 *                                    append in progress.
 *     AFATFS_OPERATION_FAILURE     - Operation could not be queued (file was busy) or append failed (filesystem is
 * full).
 *                                    Check afatfs.fileSystemFull
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Select the supported regular or freefile allocation path for a file append.
 *
 * @details Queue an operation to add a cluster of free space to the end of the file. Must be called when the
 * file's cursor
 * is beyond the last allocated cluster.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static AFATFS_INLINE afatfsOperationStatus_e afatfs_appendFreeCluster(afatfsFilePtr_t file)
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
 * @brief Test whether the cursor has reached its allocated chain boundary or fixed-root limit.
 *
 * @details Returns true if the file's cursor is sitting beyond the end of the last allocated cluster (i.e. the
 * logical fileSize
 * is not checked).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
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
/**
 * @brief Attempt best-effort read-ahead without advancing the cursor or retaining another page.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
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
    uint32_t cursorSectorInCluster = afatfs_sectorIndexInCluster(file->cursorOffset);
    uint32_t ahead = (inSector == 0) ? 0 : 1;
    if (cursorSectorInCluster + ahead >= afatfs.sectorsPerCluster) {
        return; // The next sector is in another cluster.
    }
    uint8_t *buffer;
    afatfs_cacheSector(afatfs_fileGetCursorPhysicalSector(file) + ahead, &buffer, AFATFS_CACHE_READ, 0);
}

/**
 * @brief Acquire or reuse the retained cache page at the current read cursor.
 *
 * @details Take a lock on the sector at the current file cursor position.
 *
 * Returns a pointer to the sector buffer if successful, or NULL if at the end of file (check
 * afatfs_isEndOfAllocatedFile())
 * or the sector has not yet been read in from disk.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return Retained sector bytes, or NULL at the chain boundary or while data is unavailable.
 */
#ifdef _MSC_VER
__forceinline
#else
__attribute__((always_inline)) inline
#endif
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

    afatfs.cacheDescriptor[file->readRetainCacheIndex].discardable = false;
    return result;
}

/**
 * @brief Acquire the current writable page, preserving existing bytes for partial-sector writes.
 *
 * @details Lock the sector at the file's cursor position for write, and return a reference to the memory for that
 * sector.
 *
 * Returns NULL if the cache was too busy, try again later.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param isFullSector True when the caller will replace all 512 bytes and needs no read-modify-write.
 * @return Locked writable sector bytes, or NULL while cache or allocation work is pending.
 */
static AFATFS_INLINE uint8_t *afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file, bool isFullSector)
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
 * @brief Attempt a bounded cursor move without creating an asynchronous file operation.
 *
 * @details Attempt to seek the file pointer by the offset, relative to the current position.
 *
 * Returns true if the seek was completed, or false if you should try again later by calling this routine again
 * (the
 * cursor is not moved and no seek operation is queued on the file for you).
 *
 * You can only seek forwards by the size of a cluster or less, or backwards to stay within the same cluster.
 * Otherwise
 * false will always be returned (calling this routine again will never make progress on the seek).
 *
 * This amount of seek is special because we will have to wait on at most one read operation, so it's easy to make
 * the seek atomic.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param offset Signed relative byte move, restricted to the atomic seek limits.
 * @return True when the bounded move completed; false when the caller must queue or retry it.
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
 * @brief Advance a queued seek until its FAT traversal can complete.
 *
 * @details Returns true if the seek was completed, or false if it is still in progress.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True once the queued seek completes; false while cache or FAT traversal is pending.
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
 * @brief Move the cursor immediately when possible, otherwise queue the forward seek.
 *
 * @details Seek the file pointer forwards by offset bytes. Calls the callback when the seek is complete.
 *
 * Will happily seek beyond the logical end of the file.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The seek was completed immediately
 *     AFATFS_OPERATION_IN_PROGRESS - The seek was queued and will complete later
 *     AFATFS_OPERATION_FAILURE     - The seek could not be queued because the file was busy with another
 * operation,
 *                                    try again later.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param offset Unsigned forward byte distance to consume immediately or through a queued seek.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Release cursor cache ownership and restore the file's initial cluster and offset.
 *
 * @details Attempt to seek the file cursor from the given point (`whence`) by the given offset, just like C's
 * fseek().
 *
 * AFATFS_SEEK_SET with offset 0 will always return AFATFS_OPERATION_SUCCESS.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The seek was completed immediately
 *     AFATFS_OPERATION_IN_PROGRESS - The seek was queued and will complete later. Feel free to attempt read/write
 *                                    operations on the file, they'll fail until the seek is complete.
 *     AFATFS_OPERATION_FAILURE     - The seek could not be queued because the file was busy with another
 * operation,
 *                                    try again later.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_fileRewind(afatfsFilePtr_t file)
{
    afatfs_fileUnlockCacheSector(file);
    file->cursorPreviousCluster = 0;
    file->cursorCluster = file->firstCluster;
    file->cursorOffset = 0;
}

/**
 * @brief Validate a public seek request and resolve its origin before moving the file cursor.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param offset Signed byte offset relative to whence; the public API clamps it to the logical file range.
 * @param whence Seek origin: AFATFS_SEEK_SET, AFATFS_SEEK_CUR or AFATFS_SEEK_END.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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
 * @brief Return the file's current byte offset when its operation state permits inspection.
 *
 * @details Get the byte-offset of the file's cursor from the start of the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param position Output or cursor describing a directory position or byte offset.
 * @return True when position has been stored; false for an invalid argument or a busy file.
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
 * @brief Return a file's logical byte size after reconciling completed operations.
 *
 * @details Get the logical size of the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param size Receives the reconciled logical file size in bytes.
 * @return True when size has been stored; false for an invalid argument or a busy file.
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
 * @brief Read the native FAT directory information associated with an open file.
 *
 * @details Get the information about the file.
 *
 * Returns true on success, or false if the file is busy (try again later).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param entry Receives a borrowed cache pointer, or NULL for the root directory; consume it before another cache
 * operation can evict the sector.
 * @return True when entry is available (including NULL for the root directory); false for exFAT or while the file
 * or cache read is busy.
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


/**
 * @brief Advance a native directory finder to the next available directory entry.
 *
 * @details Attempt to advance the directory pointer `finder` to the next entry in the directory.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS -     A pointer to the next directory entry has been loaded into *dirEntry. If the
 *                                    directory was exhausted then *dirEntry will be set to NULL.
 *     AFATFS_OPERATION_IN_PROGRESS - The disk is busy. The pointer is not advanced, call again later to retry.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @param finder Native directory traversal workspace.
 * @param dirEntry Receives a borrowed cache entry on success, or NULL at end of directory; consume it before
 * another cache operation can evict the sector.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Position a native finder at the last entry in a specified directory.
 *
 * @details Release resources associated with a find operation. Calling this more than once is harmless.
 * @param directory Native directory handle used for enumeration or entry allocation.
 */
void afatfs_findLast(afatfsFilePtr_t directory)
{
    afatfs_fileUnlockCacheSector(directory);
}

// Special internal findLast that frees the CWD mutex. Calling this more than once is actually dangerous.
/**
 * @brief Position a native finder at the end of the current working directory.
 * @param directory Native directory handle used for enumeration or entry allocation.
 */
static void afatfs_findLastCWD(afatfsFile_t *directory)
{
    afatfs.cwdWalkMutex = NULL;
    afatfs_findLast(directory);
}

/**
 * @brief Initialize a native finder at the beginning of a directory.
 *
 * @details Initialise the finder so that the first call with the directory to findNext() will return the first
 * file in the
 * directory.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @param finder Native directory traversal workspace.
 */
void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder)
{
    afatfs_fileRewind(directory);
    finder->entryIndex = -1;
}

// Special, mutex-guarded findFirst variant for internal use. Returns AFATFS_OPERATION_FAILURE if
// the mutex was not free, returns AFATFS_OPERATION_SUCCESS if it was free and has now been taken.
// Must be paired with findLastCWD to free the mutex, otherwise the filesystem will deadlock.
/**
 * @brief Initialize a native finder for the current working directory.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @param finder Native directory traversal workspace.
 * @param caller Owner requesting the shared operation or cache resource.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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

/**
 * @brief Advance a directory extension while preserving initialized-sector publication order.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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
 * @brief Start or resume allocating space for a growing subdirectory.
 *
 * @details Queue an operation to add a cluster to a sub-directory.
 *
 * The new cluster is zero-filled. "." and ".." entries are added if it is the first cluster of a new
 * subdirectory.
 *
 * The directory must not be busy, otherwise AFATFS_OPERATION_FAILURE is returned immediately.
 * Accepted operations return AFATFS_OPERATION_IN_PROGRESS without executing the allocation state machine;
 * the regular directory poller resumes it and delivers the callback after completion.
 *
 * The directory's cursor must lie at the end of the directory file (i.e. isEndOfAllocatedFile() would return
 * true).
 *
 * You must provide parentDirectory if this is the first extension to the subdirectory, otherwise pass NULL for
 * that argument.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @param parentDirectory Directory handle containing the entry to inspect or publish.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return AFATFS_OPERATION_IN_PROGRESS when queued, or AFATFS_OPERATION_FAILURE when the directory cannot grow.
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

    // The existing poller resumes this operation. Do not retain the caller's
    // create/mount frames while allocating and publishing the directory cluster.
    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * @brief Locate or extend the current directory to obtain an entry slot.
 *
 * @details Allocate space for a new directory entry to be written, store the position of that entry in the finder,
 * and set
 * the *dirEntry pointer to point to the entry within the cached FAT sector. This pointer's lifetime is only as
 * good
 * as the life of the cache, so don't dawdle.
 *
 * Before the first call to this function, call afatfs_findFirst() on the directory.
 *
 * The directory sector in the cache is marked as dirty, so any changes written through to the entry will be
 * flushed out
 * in a subsequent poll cycle.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - Call again later to continue
 *     AFATFS_OPERATION_SUCCESS     - Entry has been inserted and *dirEntry and *finder have been updated
 *     AFATFS_OPERATION_FAILURE     - When the directory is full.
 * @param directory Native directory handle used for enumeration or entry allocation.
 * @param dirEntry On-disk directory record used to initialize the file metadata.
 * @param finder Native directory traversal workspace.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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
 * @brief Reserve an unused entry in the statically allocated file-handle array.
 *
 * @details Return a pointer to a free entry in the open files table (a file whose type is "NONE"). You should
 * initialize
 * the file afterwards with afatfs_initFileHandle().
 * @return Reserved static handle, or NULL when no handle is available.
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
 * @brief Advance truncation and chain release without publishing references to freed clusters.
 *
 * @details Continue the file truncation.
 *
 * When truncation finally succeeds or fails, the current operation is cleared on the file (if the current
 * operation
 * was a truncate), then the truncate operation's callback is called. This allows the truncation to be called as a
 * sub-operation without it clearing the parent file operation.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param markDeleted True when the released entry set must also be marked deleted.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_ftruncateContinue(afatfsFilePtr_t file, bool markDeleted)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    afatfsOperationStatus_e status = AFATFS_OPERATION_FAILURE;
    if (!afatfs_exfatPrepareTruncate(file)) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

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
            while (opState->traversal.clustersToKeep > 1) {
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
                opState->traversal.clustersToKeep--;
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
            opState->traversal.startCluster = opState->currentCluster;
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
            afatfs_exfatFinishTruncate(file, opState->targetSize);
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
            if (opState->keepCluster && !afatfs_exfatLinearAllocation(file)) {
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

            afatfs.freeFile.firstCluster = opState->traversal.startCluster;

            freeFileGrow = (oldFreeFileStart - opState->traversal.startCluster) * afatfs_clusterSize();

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
                        opState->traversal.nextCluster = opState->currentCluster + 1 == opState->endCluster
                            ? 0xFFFFFFFF
                            : opState->currentCluster + 1;
                        status = AFATFS_OPERATION_SUCCESS;
                    } else {
                        status = afatfs_FATGetNextCluster(0, opState->currentCluster, &opState->traversal.nextCluster);
                    }
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    if (!afatfs_FATIsEndOfChainMarker(opState->traversal.nextCluster) &&
                        (opState->traversal.nextCluster < 2 ||
                            opState->traversal.nextCluster - 2 >= afatfs.numClusters)) {
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
#ifdef AFATFS_PIPELINE_ALLOCATION
                if (!(afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER)) {
                    afatfs.lastClusterAllocated = MIN(afatfs.lastClusterAllocated, opState->currentCluster);
                }
#else
                afatfs.lastClusterAllocated = MIN(afatfs.lastClusterAllocated, opState->currentCluster);
#endif
                opState->currentCluster = opState->traversal.nextCluster;
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
 * @brief Queue truncation of an open file to an empty file.
 *
 * @details Queue an operation to truncate the file at its current cursor, like FatFs f_truncate.
 *
 * Returns true if the operation was successfully queued or false if the file is busy (try again later).
 *
 * The callback is called once the file has been truncated (some time after this routine returns).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
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

/**
 * @brief Queue truncation to a validated logical byte size.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param size Requested logical file size in bytes.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 */
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
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.journal.ready &&
        file->firstCluster == afatfs.journal.firstCluster) {
        return false;
    }
#endif

#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs_releaseReservedCluster() != AFATFS_OPERATION_SUCCESS) {
        return false;
    }
#endif

    file->operation.active = AFATFS_FILE_OPERATION_TRUNCATE;

    opState = &file->operation.state.truncateFile;
    opState->callback = callback;
    opState->user = user;
    opState->phase = AFATFS_TRUNCATE_FILE_INITIAL;
    opState->traversal.startCluster = file->firstCluster;
    opState->currentCluster = opState->traversal.startCluster;
    opState->nextClusterValid = false;
    opState->fatCleared = false;
    opState->targetSize = size;
    opState->keepCluster = size ? file->firstCluster : 0;
    opState->previousCluster = 0;
    opState->traversal.clustersToKeep = size ? (size - 1) / afatfs_clusterSize() + 1 : 0;
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

    afatfs_exfatConfigureTruncate(file);

    return true;
}

/**
 * @brief Load a directory entry into a file handle's runtime metadata.
 *
 * @details Load details from the given FAT directory entry into the file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 */
static void afatfs_fileLoadDirectoryEntry(afatfsFile_t *file, fatDirectoryEntry_t *entry)
{
    file->firstCluster = ((uint32_t)entry->firstClusterHigh << 16) | entry->firstClusterLow;
    file->logicalSize = entry->fileSize;
    file->physicalSize = roundUpTo(entry->fileSize, afatfs_clusterSize());
    file->attrib = entry->attrib;
    file->type = entry->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY ? AFATFS_FILE_TYPE_DIRECTORY : AFATFS_FILE_TYPE_NORMAL;
}


// Reuse the open-file table: readers may share a file, but a writer must be exclusive.
// Entry identity distinguishes empty files and equal names in different directories.
/**
 * @brief Reject conflicting open handles and access modes for the same directory entry.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_fileOpenAllowed(afatfsFilePtr_t file)
{
    if (file->firstCluster >= 2 &&
        (file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA))) {
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.journal.ready &&
            file->firstCluster == afatfs.journal.firstCluster) {
            return false;
        }
#endif
#ifdef AFATFS_ENABLE_FAT_JOURNAL
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.ready &&
            file->firstCluster == afatfs.journal.firstCluster) {
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


/**
 * @brief Advance native short-name lookup, creation and open completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
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
 * @brief Reset a static file-handle slot and its cache ownership indices.
 *
 * @details Reset the in-memory data for the given handle back to the zeroed initial state
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_initFileHandle(afatfsFilePtr_t file)
{
    memset(file, 0, sizeof(*file));
    file->writeLockedCacheIndex = -1;
    file->readRetainCacheIndex = -1;
}

/**
 * @brief Advance deletion after releasing the file's allocated chain safely.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_funlinkContinue(afatfsFilePtr_t file)
{
    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;
    afatfsOperationStatus_e status;

    if (opState->truncateFile.phase == AFATFS_TRUNCATE_FILE_CHECK_DIRECTORY) {
        afatfsTruncateFile_t *scan = &opState->truncateFile;
        bool empty = afatfs_FATIsEndOfChainMarker(scan->currentCluster);
        if (!empty && scan->traversal.clustersToKeep == afatfs.sectorsPerCluster) {
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
            scan->traversal.clustersToKeep = 0;
            if (++scan->previousCluster > afatfs.numClusters) {
                afatfs_fileError(file, AFATFS_ERROR_BAD_FILESYSTEM_HEADER);
                goto rejectedDirectory;
            }
            empty = afatfs_FATIsEndOfChainMarker(next);
        }
        if (!empty) {
            uint8_t *sector;
            if (afatfs_cacheSector(afatfs_fileClusterToPhysical(scan->currentCluster, scan->traversal.clustersToKeep),
                    &sector, AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS) {
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
                scan->traversal.clustersToKeep++;
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
 * @brief Queue removal of a file through its native file handle.
 *
 * @details Delete and close the file.
 *
 * Returns true if the operation was successfully queued (callback will be called some time after this routine
 * returns)
 * or false if the file is busy and you should try again later.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 */
bool afatfs_funlink(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
#if ASYNCFS_ENABLE_FATFS_API
    if (afatfs_request && !afatfs_request->driving) {
        return afatfs_fileError(file, AFATFS_ERROR_BUSY);
    }
#endif

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
#if ASYNCFS_ENABLE_FATFS_API
        for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next) {
            if (dir->cursor.firstCluster == file->firstCluster) {
                return afatfs_fileError(file, AFATFS_ERROR_DENIED);
            }
        }
#endif
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
 * @brief Initialize the native lookup/create operation and its completion callback.
 *
 * @details Open (or create) a file in the CWD with the given fileName.
 *
 * file             - Memory location to store the newly opened file details
 * name             - Filename in "name.ext" format. No path.
 * attrib           - FAT file attributes to give the file (if created)
 * fileMode         - Bitset of AFATFS_FILE_MODE_* constants. Include AFATFS_FILE_MODE_CREATE to create the file if
 *                    it does not exist.
 * callback         - Called when the operation is complete
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param name Filename, pathname component or label used by this operation.
 * @param attrib Native directory-entry attribute bits.
 * @param fileMode AFATFS_FILE_MODE_* access flags for the pending open operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return The decoded value, count or status described above; see the traversal contract for sentinels.
 */
static afatfsFilePtr_t afatfs_createFile(
    afatfsFilePtr_t file, const char *name, uint8_t attrib, uint8_t fileMode, afatfsFileCallback_t callback, void *user)
{
    afatfsCreateFile_t *opState = &file->operation.state.createFile;

    afatfs_initFileHandle(file);
    opState->directory = (attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0;
#if defined(ASYNCFS_ENABLE_EXFAT) && ASYNCFS_ENABLE_FATFS_API
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs_request &&
        afatfs_request->operation == AFATFS_API_RENAME && name == afatfs_request->otherName &&
        (fileMode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE)) {
        afatfsFilePtr_t source = afatfs_request->file;
        unsigned names = 2 + (((exFatStreamEntry_t *)(source->exfatEntries + 32))->nameLength + 14) / 15;
        opState->location.exfat.extraEntryCount = (uint8_t)(source->exfatEntryCount - names);
    }
#endif
    if (!name || !name[0]) {
        afatfs_fileError(file, AFATFS_ERROR_INVALID_NAME);
        if (callback) {
            callback(NULL, user);
        }
        return file;
    }
#ifdef AFATFS_ENABLE_FAT_JOURNAL
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&
        (fileMode &
            (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_METADATA)) &&
        !(afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION && afatfs.journal.provision &&
            file == &afatfs.openFiles[0])) {
        const char *reserved = AFATFS_JOURNAL_NAME;
        unsigned index = 0;
        while (name[index] && reserved[index] && afatfs_lfnUpper((uint8_t)name[index]) == (uint8_t)reserved[index]) {
            index++;
        }
        if (!name[index] && !reserved[index]) {
            afatfs_fileError(file, AFATFS_ERROR_DENIED);
            if (callback) {
                callback(NULL, user);
            }
            return file;
        }
    }
#endif
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
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
        !(afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION && afatfs.journal.provision &&
            file == &afatfs.openFiles[0]) &&
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
/**
 * @brief Synchronize cached file contents and publish directory metadata in durability order.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param phase State-machine phase to enter or inspect.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_syncFileContinue(afatfsFilePtr_t file, uint8_t *phase)
{
    if (*phase == 0) {
        afatfs_fileUnlockCacheSector(file);
        *phase = 1;
    }
    if (*phase == 1) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs_releaseReservedCluster() != AFATFS_OPERATION_SUCCESS) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
#endif
#ifdef AFATFS_PIPELINE_ALLOCATION
        // Keep this handle's old directory image buffered until phase two
        // replaces it with the exact logical size. Data and both FATs still drain.
        if (!afatfs_flushAllocationCache(file)) {
#else
        if (!afatfs_flushCache()) {
#endif
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;
    }
    if (*phase == 2) {
        bool needsCommit = true;
#ifdef AFATFS_PIPELINE_ALLOCATION
        needsCommit = file->modified || !file->directoryExact;
#endif
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
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (*phase < 4) {
#endif
    if (!afatfs_flushCache()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (!afatfs_journalCommitted()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    afatfs_exfatCompleteSync(file);
    if (!sdcard_poll()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    if (!afatfs_endReadSession()) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
#endif
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
#ifdef AFATFS_PIPELINE_ALLOCATION
        *phase = 4;
    }
    return afatfs_batchFinishSync(file, phase);
#else
    return AFATFS_OPERATION_SUCCESS;
#endif
}

/**
 * @brief Advance an explicit file sync and invoke its callback only after the required barriers.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
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

/**
 * @brief Queue synchronization of a file's data and metadata.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 *
 * @see afatfs_poll
 * @see afatfs_canPowerOff
 */
bool afatfs_fsync(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (file && file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
        file->operation.state.appendFreeCluster.collecting) {
        afatfs_fileUpdateFilesize(file);
        afatfs_sealCollections();
        file->operation.active = AFATFS_FILE_OPERATION_COLLECT_SYNC;
        file->operation.state.collectSync.callback.sync = callback;
        file->operation.state.collectSync.user = user;
        return true;
    }
#endif
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

/**
 * @brief Advance close, synchronization and static-handle release.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
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
 * @brief Queue a close while retaining the handle until outstanding work completes.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 */
bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (file && file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
        file->operation.state.appendFreeCluster.collecting) {
        afatfs_fileUpdateFilesize(file);
        afatfs_sealCollections();
        file->operation.active = AFATFS_FILE_OPERATION_COLLECT_CLOSE;
        file->operation.state.collectSync.callback.close = callback;
        file->operation.state.collectSync.user = user;
        return true;
    }
#endif
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
 * @brief Queue creation of a directory relative to the current working directory.
 *
 * @details Create a new directory with the given name, or open the directory if it already exists.
 *
 * The directory will be passed to the callback, or NULL if the creation failed.
 *
 * Returns true if the directory creation was begun, or false if there are too many open files.
 * @param fileName Native filename relative to the current working directory.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 */
bool afatfs_mkdir(const char *fileName, afatfsFileCallback_t callback, void *user)
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        return false;
    }
#if ASYNCFS_ENABLE_FATFS_API
    if (afatfs_request && !afatfs_request->driving) {
        return false;
    }
#endif
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
 * @brief Change the native current working directory using an existing directory handle.
 *
 * @details Change the working directory to the directory with the given handle (use fopen). Pass NULL for
 * `directory` in order to
 * change to the root directory.
 *
 * Returns true on success, false if you should call again later to retry. After changing into a directory, your
 * handle
 * to that directory may be closed by fclose().
 * @param directory Directory handle to select; NULL selects the filesystem root.
 * @return True when the condition or completion described above holds; false otherwise.
 */
bool afatfs_chdir(afatfsFilePtr_t directory)
{
    if (!afatfs_mediaSessionActive()) {
        return false;
    }
    if (afatfs.cwdWalkMutex) {
        return false;
    }
#if ASYNCFS_ENABLE_FATFS_API
    if (afatfs_request && !afatfs_request->driving) {
        return false;
    }
#endif
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
 * @brief Queue native file lookup or creation using the requested mode string.
 *
 * @details Begin the process of opening a file with the given name in the current working directory (paths in the
 * fileName are
 * not supported) using the given mode.
 *
 * To open the current working directory, pass "." for fileName.
 *
 * The complete() callback is called when finished with either a file handle (file was opened) or NULL upon
 * failure.
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
 * ws   If the file is already non-empty or freefile support is not compiled in then it will fall back to non-
 * contiguous
 *      operation.
 *
 * All other mode strings are illegal. In particular, don't add "b" to the end of the mode string.
 *
 * Returns false if the the open failed really early (out of file handles).
 * @param fileName Native filename relative to the current working directory.
 * @param mode Native access mode string, for example "r", "w" or an append mode.
 * @param complete Native file-open completion callback.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 */
bool afatfs_fopen(const char *fileName, const char *mode, afatfsFileCallback_t complete, void *user)
{
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY) {
        return false;
    }
#if ASYNCFS_ENABLE_FATFS_API
    if (afatfs_request && !afatfs_request->driving && mode && mode[0] != 'r') {
        return false;
    }
#endif
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
 * @brief Attempt to append one byte through the asynchronous buffered-write path.
 *
 * @details Write one byte. Return 1 if accepted, -1 if rejected (including a busy cache).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param character Unsigned character value to fold using the supported name encoding.
 * @return Number of bytes accepted, either zero or one.
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
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs_collectWritable(file)) {
        return afatfs_fwrite(file, &character, 1) == 1 ? 1 : -1;
    }
#endif
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
 * @brief Copy as many bytes as possible into the writable cache without blocking for storage.
 * @note A small-write EOC window may accept additional requests while allocation is pending.
 * Bytes are accepted only into the existing cache; sync/close wait behind both FAT barriers.
 *
 * @details Attempt to write `len` bytes from `buffer` into the `file`.
 *
 * Returns the number of bytes actually written.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *
 * Fewer bytes will be written than requested when:
 *     The write spanned a sector boundary and the next sector's contents/location was not yet available in the
 * cache.
 *     Or you tried to extend the length of the file but the filesystem is full (check afatfs_isFull()).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param buffer Source containing at least len bytes; only the returned prefix is consumed.
 * @param len Maximum number of bytes requested by the caller.
 * @return Number of bytes accepted into the cache, possibly less than len; acceptance is not durability.
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

    if (afatfs_fileIsBusy(file)
#ifdef AFATFS_PIPELINE_ALLOCATION
        && !afatfs_collectWritable(file)
#endif
    ) {
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

#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs_collectWritable(file) && afatfs_isEndOfAllocatedFile(file) && !afatfs_collectNext(file)) {
            break;
        }
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL && file->writeLockedCacheIndex == -1 &&
            AFATFS_REQUESTED_BATCH_CLUSTERS > 1 &&
            (len >= 2 * AFATFS_SECTOR_SIZE || (AFATFS_SMALL_WRITE_WINDOW_CLUSTERS && len < AFATFS_SECTOR_SIZE)) &&
            file->operation.active == AFATFS_FILE_OPERATION_NONE && afatfs_isEndOfAllocatedFile(file)) {
            file->operation.active = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
            afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster,
                file->cursorPreviousCluster);
            file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_BATCH_FIND;
            file->operation.state.appendFreeCluster.progress.zeroSector = MAX(1, MIN(len / AFATFS_SECTOR_SIZE, AFATFS_REQUESTED_BATCH_CLUSTERS));
            // Native FAT32 otherwise does not use the exFAT wrap marker.
            file->operation.state.appendFreeCluster.wrapped = true;
            file->operation.state.appendFreeCluster.collecting = AFATFS_SMALL_WRITE_WINDOW_CLUSTERS && len < AFATFS_SECTOR_SIZE;
            break;
        }
#endif
        sectorBuffer = afatfs_fileLockCursorSectorForWrite(file, writingFullSector);
        if (!sectorBuffer) {
            // Cache is currently busy
            break;
        }

        memcpy(sectorBuffer + cursorOffsetInSector, buffer, bytesToWriteThisSector);
        file->modified = true;
        afatfs_exfatAccountWrite(file, bytesToWriteThisSector);

        writtenBytes += bytesToWriteThisSector;

#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs_collectWritable(file)) {
            file->operation.state.appendFreeCluster.collectIdle = 0;
            file->cursorOffset += bytesToWriteThisSector;
            afatfs_fileUpdateFilesize(file);
            if (cursorOffsetInSector + bytesToWriteThisSector == AFATFS_SECTOR_SIZE) {
                afatfs_fileUnlockCacheSector(file);
                file->cursorPreviousCluster = file->cursorCluster;
                file->cursorCluster = 0;
            }
            len -= bytesToWriteThisSector;
            buffer += bytesToWriteThisSector;
            cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
            continue;
        }
#endif

        if (cursorOffsetInSector + bytesToWriteThisSector < AFATFS_SECTOR_SIZE) {
            // The request ends within the locked page; no seek state changes.
            file->cursorOffset += bytesToWriteThisSector;
            return writtenBytes;
        }

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
 * @brief Copy available cached bytes into the caller's buffer and advance the read cursor.
 *
 * @details Attempt to read `len` bytes from `file` into the `buffer`.
 *
 * Returns the number of bytes actually read.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *     EOF was reached (check afatfs_feof())
 *
 * Fewer bytes than requested will be read when:
 *     The read spans a AFATFS_SECTOR_SIZE boundary and the following sector was not available in the cache yet.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param buffer Destination with room for at least len bytes; only the returned prefix is written.
 * @param len Maximum number of bytes requested by the caller.
 * @return Number of bytes copied, possibly less than len; zero may mean EOF, busy state or an error.
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

#if defined(AFATFS_PIPELINE_ALLOCATION) && !defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT)
        if (bytesToReadThisSector == AFATFS_SECTOR_SIZE) {
            // A full-sector read starts at offset zero. Expose its fixed size.
            memcpy(buffer, sectorBuffer, AFATFS_SECTOR_SIZE);
        } else {
            memcpy(buffer, sectorBuffer + cursorOffsetInSector, bytesToReadThisSector);
        }
#else
        memcpy(buffer, sectorBuffer + cursorOffsetInSector, bytesToReadThisSector);
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT &&
            file->validDataLength < file->cursorOffset + bytesToReadThisSector) {
            uint32_t valid =
                file->validDataLength > file->cursorOffset ? file->validDataLength - file->cursorOffset : 0;
            memset(buffer + valid, 0, bytesToReadThisSector - valid);
        }
#endif

        readBytes += bytesToReadThisSector;

#if defined(AFATFS_PIPELINE_ALLOCATION) && !defined(AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT)
        if (len < AFATFS_SECTOR_SIZE - cursorOffsetInSector) {
#else
        if (cursorOffsetInSector + bytesToReadThisSector < AFATFS_SECTOR_SIZE) {
#endif
            // This request ends inside the retained sector; no seek state can change.
            file->cursorOffset += bytesToReadThisSector;
            return readBytes;
        }

        /*
         * If the seek doesn't complete immediately then we'll break and wait for that seek to complete by waiting for
         * the file to be non-busy on entry again.
         *
         * A seek operation should always be able to queue on the file since we have checked that the file wasn't busy
         * on entry (fseek will never return AFATFS_OPERATION_FAILURE).
         */
        afatfsOperationStatus_e seekStatus = afatfs_fseekInternal(file, bytesToReadThisSector, NULL, NULL);
        if (seekStatus == AFATFS_OPERATION_IN_PROGRESS) {
            break;
        }
        // The partial-sector case already returned. A pending seek cannot prefetch.
        afatfs_fprefetch(file);

        len -= bytesToReadThisSector;
        buffer += bytesToReadThisSector;
        cursorOffsetInSector = 0;
    }

    return readBytes;
}

/**
 * @brief Test whether the cursor has reached the file's logical end.
 *
 * @details Returns true if the file's pointer position currently lies at the end-of-file point (i.e. one byte
 * beyond the last
 * byte in the file).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
bool afatfs_feof(afatfsFilePtr_t file)
{
    return file->cursorOffset >= file->logicalSize;
}

/**
 * @brief Dispatch one file's active asynchronous state machine.
 *
 * @details Continue any queued operations on the given file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 */
static void afatfs_fileOperationContinue(afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE) {
        return;
    }

    switch (file->operation.active) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        case AFATFS_FILE_OPERATION_COLLECT_SYNC:
        case AFATFS_FILE_OPERATION_COLLECT_CLOSE:
            if (afatfs_appendRegularFreeClusterContinue(file) != AFATFS_OPERATION_IN_PROGRESS) {
                void *user = file->operation.state.collectSync.user;
                if (file->operation.active == AFATFS_FILE_OPERATION_COLLECT_CLOSE) {
                    afatfsCallback_t callback = file->operation.state.collectSync.callback.close;
                    file->operation.state.closeFile.callback = callback;
                    file->operation.state.closeFile.user = user;
                    file->operation.state.closeFile.phase = 0;
                    file->operation.active = AFATFS_FILE_OPERATION_CLOSE;
                } else {
                    afatfsFileCallback_t callback = file->operation.state.collectSync.callback.sync;
                    file->operation.state.syncFile.callback = callback;
                    file->operation.state.syncFile.user = user;
                    file->operation.state.syncFile.phase = 0;
                    file->operation.active = AFATFS_FILE_OPERATION_SYNC;
                }
            }
            break;
#endif
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
 * @brief Advance pending operations in internal handles and the public static handle array.
 *
 * @details Check files for pending operations and execute them.
 */
static void afatfs_fileOperationsPoll()
{
    if (afatfs.currentDirectory.operation.active != AFATFS_FILE_OPERATION_NONE) {
        afatfs_fileOperationContinue(&afatfs.currentDirectory);
    }

#ifdef AFATFS_USE_FREEFILE
    // Closing the internal freefile now yields while its writes are synchronized too.
    if (afatfs.freeFile.operation.active != AFATFS_FILE_OPERATION_NONE) {
        afatfs_fileOperationContinue(&afatfs.freeFile);
    }
#endif

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (afatfs.introSpecLog.operation.active != AFATFS_FILE_OPERATION_NONE) {
        afatfs_fileOperationContinue(&afatfs.introSpecLog);
    }
#endif

    for (int index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->operation.active == AFATFS_FILE_OPERATION_NONE) {
            continue;
        }
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER &&
            file->type != AFATFS_FILE_TYPE_NONE) {
            if (file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT) {
                // Preserve the original increment and full sealing boundary.
                if (afatfs.cacheFlushInProgress) {
                    continue;
                }
                if (file->operation.state.appendFreeCluster.collectIdle != 63) {
                    file->operation.state.appendFreeCluster.collectIdle++;
                    continue;
                }
            }
            afatfs_appendRegularFreeClusterContinue(file);
        } else {
            afatfs_fileOperationContinue(file);
        }
#else
        afatfs_fileOperationContinue(file);
#endif
    }
#if ASYNCFS_ENABLE_FATFS_API
    afatfs_apiPoll();
#endif
}

/**
 * @brief Inspect the contiguous space represented by the current freefile state.
 *
 * @details Return the available size of the freefile (used for files in contiguous append mode)
 * @return Available freefile length in bytes, or zero when freefile support is disabled.
 */
uint32_t afatfs_getContiguousFreeSpace()
{
#ifdef AFATFS_USE_FREEFILE
    return afatfs.freeFile.logicalSize;
#else
    return 0;
#endif
}

#ifdef AFATFS_USE_FREEFILE
/**
 * @brief Initialize a FAT scan for the largest contiguous free extent.
 *
 * @details Call to set up the initial state for finding the largest block of free space on the device whose
 * corresponding FAT
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
 * @brief Advance the largest-free-extent scan without blocking on cache misses.
 *
 * @details Call to continue the search for the largest contiguous block of free space on the device.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - SD card is busy, call again later to resume
 *     AFATFS_OPERATION_SUCCESS - When the search has finished and afatfs.initState.freeSpaceSearch has been
 * updated with the details of the best gap.
 *     AFATFS_OPERATION_FAILURE - When a read error occured
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
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

/**
 * @brief Finish initialization after the internal free-space file has been opened or created.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 */
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

/**
 * @brief Finish opening the internal introspection log.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 */
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


/**
 * @brief Advance mount discovery and filesystem initialization by one nonblocking step.
 */
static void afatfs_initContinue()
{
#ifdef AFATFS_USE_FREEFILE
    afatfsOperationStatus_e status;
#endif

    uint8_t *sector;

doMore:
    if (afatfs_exfatContinueGPT()) {
        return;
    }

    switch (afatfs.initPhase) {
        case AFATFS_INITIALIZATION_READ_MBR:
            if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0) ==
                AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(sector)) {
                    afatfs.initPhase = AFATFS_INITIALIZATION_READ_VOLUME_ID;
                    afatfs_exfatSelectMountPhase();
                    goto doMore;
                } else {
                    afatfs.lastError = AFATFS_ERROR_BAD_MBR;
                    afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
            break;
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
            if (afatfs_exfatActive()) {
                afatfs_exfatMountRoot();
                break;
            }
            afatfs_chdir(NULL);
#ifdef AFATFS_PIPELINE_ALLOCATION
            memset(&afatfs.currentDirectory.operation.state.batchRecovery, 0,
                sizeof(afatfs.currentDirectory.operation.state.batchRecovery));
#endif
            afatfs.initPhase++;
            break;

#ifdef AFATFS_PIPELINE_ALLOCATION
        case AFATFS_INITIALIZATION_BATCH_RECOVERY: {
            afatfsOperationStatus_e status = afatfs_batchRecoverContinue();
            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase++;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BAD_FILESYSTEM_HEADER;
                afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
            break;
        }
#endif

#ifdef AFATFS_ENABLE_FAT_JOURNAL
        case AFATFS_INITIALIZATION_FAT_JOURNAL:
            if (afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 ||
                afatfs_fatJournalMount() == AFATFS_OPERATION_SUCCESS) {
                afatfs.initPhase++;
                if (afatfs.readOnly) {
                    afatfs.initPhase = AFATFS_INITIALIZATION_DONE;
                }
            }
            break;
#endif

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
            afatfs_journalFinishMount();
            afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_READY;
            break;
    }
}

/**
 * @brief Drive backend progress, ordered cache writes and active filesystem state machines.
 *
 * @details Check to see if there are any pending operations on the filesystem and perform a little work (without
 * waiting on the
 * sdcard). You must call this periodically.
 */
void afatfs_poll()
{
    if (!afatfs_mediaSessionActive()) {
        if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPING) {
            sdcard_poll();
        }
        return;
    }
    // Only attempt to continue FS operations if the card is present & ready, otherwise we would just be wasting time
    if (sdcard_poll()) {
        if (afatfs_apiPollFormat()) {
            return;
        }
#ifdef AFATFS_ENABLE_JOURNAL
        afatfs_journalPoll();
        if (!afatfs_deferBackgroundFlush()) {
            afatfs_flushCache();
        }
#elif defined(AFATFS_PIPELINE_ALLOCATION)
        afatfs_releaseIdleReservation();
        if (afatfs.cacheFlushInProgress && afatfs_allocationWriteInProgress()) {
            // No shared barrier can finish while the backend still owns a
            // write. Keep the original single polling pass on this hot path.
            afatfs_fileOperationsPoll();
            return;
        }
        if (afatfs.cacheDirtyEntries && !afatfs_pendingAllocationGroup() &&
            !(afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION &&
                afatfs.initPhase == AFATFS_INITIALIZATION_BATCH_RECOVERY)) {
            afatfs_flushCacheFiltered(0, true);
        }
#else
        afatfs_flushCache();
#endif

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
#ifdef AFATFS_PIPELINE_ALLOCATION
                if (afatfs.cacheDirtyEntries && afatfs_pendingAllocationGroup()) {
                    // A later writer may have crossed its EOC barrier. Give
                    // earlier ready links one more pass before background IO.
                    // Separate passes preserve the original polling stack.
                    afatfs_fileOperationsPoll();
                } else {
                    // No operation runs before the final identical group test.
                    // Its false result cannot change, so avoid scanning twice.
                    return;
                }
#endif
                break;
            default:;
        }
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs.cacheDirtyEntries && afatfs_pendingAllocationGroup()) {
            // Let already pending writers prepare a common FAT image first.
            afatfs_flushCacheFiltered(0, true);
        }
#endif
    }
}

#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING

/**
 * @brief Append a backend timing sample to the optional introspection log.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param blockIndex Physical sector at the beginning of the announced read session.
 * @param duration Backend timing measurement associated with the completed sector operation.
 */
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

/**
 * @brief Read the current mount and teardown lifecycle state.
 * @return The current afatfsFilesystemState_e lifecycle state.
 */
afatfsFilesystemState_e afatfs_getFilesystemState()
{
    return afatfs.fileSystemState;
}

/**
 * @brief Read the last diagnostic associated with the current media session.
 * @return The session diagnostic, which does not by itself replace per-file error inspection.
 */
afatfsError_e afatfs_getLastError()
{
    return afatfs.lastError;
}

/**
 * @brief Test whether a file still owns cache pages or asynchronous work relevant to idle status.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_fileHasPendingWork(afatfsFilePtr_t file)
{
    if (afatfs_lfnFileHasPendingWork(file)) {
        return true;
    }
    if (afatfs_fileIsBusy(file)) {
        return true;
    }
    if (afatfs_exfatFileHasPendingWork(file)) {
        return true;
    }
    return false;
}

/**
 * @brief Test global quiescence, including cache ownership and pending backend session stops.
 * @return True when the condition or completion described above holds; false otherwise.
 */
bool afatfs_isIdle(void)
{
#ifdef AFATFS_PIPELINE_ALLOCATION
    if (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) {
        return false;
    }
#endif
    if (!afatfs_apiIsIdle()) {
        return false;
    }
    if (afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED) {
        return true;
    }
    if (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_READY || afatfs.cacheDirtyEntries ||
        afatfs.cacheFlushInProgress || afatfs.cwdWalkMutex) {
        return false;
    }
    if (!afatfs_journalIsIdle()) {
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
    if (!afatfs_exfatIsIdle()) {
        return false;
    }
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
    return afatfs_endReadSession();
#else
    return true;
#endif
}

/**
 * @brief Report whether the current state permits power removal under the library's durability contract.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @see afatfs_poll
 * @see afatfs_canPowerOff
 */
bool afatfs_canPowerOff(void)
{
    return afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_STOPPED;
}

/**
 * @brief Reset runtime state and begin asynchronous media discovery.
 */
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

/**
 * @brief Cancel the old media generation and invalidate its handles and outstanding requests.
 */
void afatfs_notifyMediaRemoved(void)
{
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_REMOVED);
    afatfs.readOnly = true;
    afatfs.lastError = AFATFS_ERROR_MEDIA_REMOVED;
}

// Allocation changes invalidate FAT32 FSInfo hints. Write "unknown", rather than
// leaving a believable stale count for the next driver. The closed first handle
// supplies the two-sector scratch state; no counter or sector buffer is allocated.
/**
 * @brief Update supported free-space metadata before completing filesystem shutdown.
 * @return True when the condition or completion described above holds; false otherwise.
 */
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
        scratch->traversal.startCluster = 0;
        scratch->endCluster = 0;
        if (primary && primary < volume->fat.reservedSectorCount && primary < afatfs.partitionSectors) {
            scratch->traversal.startCluster = primary;
            if (backup && backup != 0xFFFF && backup + primary < volume->fat.reservedSectorCount &&
                backup + primary < afatfs.partitionSectors) {
                scratch->endCluster = backup + primary;
            }
        }
        afatfs.initPhase = AFATFS_SHUTDOWN_FSINFO_PRIMARY;
    }
    while (afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_PRIMARY || afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_BACKUP) {
        uint32_t relative =
            afatfs.initPhase == AFATFS_SHUTDOWN_FSINFO_PRIMARY ? scratch->traversal.startCluster : scratch->endCluster;
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
 * @brief Advance forced cancellation or a clean, durable filesystem teardown.
 *
 * @details Shut down the filesystem, flushing all data to the disk. Keep calling until it returns true.
 *
 * dirty - Set to true to skip the flush operation and terminate immediately (buffered data will be lost!)
 * @param dirty True to cancel without flushing; false to finish a clean, durable shutdown.
 * @return True when the requested teardown is complete; false while a clean teardown still requires progress.
 *
 * @see afatfs_poll
 * @see afatfs_canPowerOff
 */
bool afatfs_destroy(bool dirty)
{
    if (!afatfs_apiShutdownReady(dirty)) {
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
    if (!dirty && afatfs_journalUnresolvedError()) {
        return false;
    }
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
        if (!afatfs_journalShutdown()) {
            return false;
        }

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
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT
        if (!afatfs_endReadSession()) {
            return false;
        }
#endif
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

/**
 * @brief Test whether an open file participates in writable-buffer capacity accounting.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_bufferWritableFile(afatfsFilePtr_t file)
{
    return file && file->type == AFATFS_FILE_TYPE_NORMAL &&
        (!afatfs_fileIsBusy(file)
#ifdef AFATFS_PIPELINE_ALLOCATION
            || afatfs_collectWritable(file)
#endif
        ) &&
        (file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE));
}

/**
 * @brief Test whether a cache descriptor can supply reusable write-buffer space.
 * @param descriptor Cache descriptor whose state and ownership flags are inspected or updated.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_bufferReusableSector(const afatfsCacheBlockDescriptor_t *descriptor)
{
    return descriptor->state == AFATFS_CACHE_STATE_EMPTY ||
        (descriptor->state == AFATFS_CACHE_STATE_IN_SYNC && !descriptor->locked && !descriptor->retainCount);
}

/**
 * @brief Count writable bytes remaining in an open file's current cached sector.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param cacheIndex Index of the cache page being inspected or modified.
 * @return Number of writable bytes remaining in the file tail sector.
 */
static uint32_t afatfs_bufferFileTail(afatfsFilePtr_t file, int cacheIndex)
{
    return afatfs_bufferWritableFile(file) && file->writeLockedCacheIndex == cacheIndex
        ? AFATFS_SECTOR_SIZE - file->cursorOffset % AFATFS_SECTOR_SIZE
        : 0;
}

/**
 * @brief Estimate immediately reusable buffered-write capacity for a particular file.
 *
 * @details Shared cache capacity plus this file's own writable sector tail. No I/O or allocation.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return Estimated immediately reusable write-buffer capacity in bytes for this file.
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
 * @brief Estimate globally reusable buffered-write capacity without allocating more memory.
 *
 * @details Count reusable cache sectors and writable tails already owned by open files.
 * Each cache sector is counted once; no additional bookkeeping storage is needed.
 * @return Estimated immediately reusable write-buffer capacity in bytes.
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

#if ASYNCFS_ENABLE_FATFS_API
/* ===== Optional api implementation; guarded by the enclosing feature condition. ===== */
/* Extended asynchronous API. One caller-owned request is driven by the original poll loop.
 * No heap, second cache, or synchronous storage path is used. */
#include <stdarg.h>
#include <stdio.h>

/**
 * @brief Cancel the active compatibility request and retire directory iterators.
 * @param state Lifecycle state determining removal versus cancellation status.
 */
static AFATFS_INLINE void afatfs_apiResetSession(afatfsFilesystemState_e state)
{
    if (afatfs_request) {
        afatfs_request->result =
            state == AFATFS_FILESYSTEM_STATE_REMOVED ? AFATFS_ERROR_MEDIA_REMOVED : AFATFS_ERROR_CANCELED;
        afatfs_request->complete = true;
        afatfs_request = NULL;
    }
    afatfs_directories = NULL;
}

/**
 * @brief Test compatibility request and directory-iterator ownership before global idle.
 * @return True when no request or directory read is pending.
 */
static AFATFS_INLINE bool afatfs_apiIsIdle(void)
{
    if (afatfs_request) {
        return false;
    }
    for (afatfsDirectory_t *dir = afatfs_directories; dir; dir = dir->next) {
        if (dir->reading) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Advance a pending compatibility request before a clean shutdown.
 * @param dirty True for forced teardown, which may cancel pending requests.
 * @return True when teardown may continue; false after polling an unfinished clean request.
 */
static AFATFS_INLINE bool afatfs_apiShutdownReady(bool dirty)
{
    if (!dirty && afatfs_request) {
        afatfs_poll();
        return false;
    }
    return true;
}

/**
 * @brief Give an active format or partition request exclusive use of the polling pass.
 * @return True when format progress consumed this pass; false for normal filesystem polling.
 */
static AFATFS_INLINE bool afatfs_apiPollFormat(void)
{
    if (afatfs_request && afatfs_request->operation >= AFATFS_API_MKFS) {
        afatfs_apiPoll();
        return true;
    }
    return false;
}

/**
 * @brief Set the compatibility API's session diagnostic and return operation failure.
 * @param error Filesystem diagnostic to record or return.
 * @return AFATFS_OPERATION_FAILURE after recording the diagnostic when a media session is active.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static afatfsOperationStatus_e afatfs_apiError(afatfsError_e error)
{
    if (afatfs_mediaSessionActive()) {
        afatfs.lastError = error;
    }
    return AFATFS_OPERATION_FAILURE;
}

/**
 * @brief Fold an ASCII name character using the active filesystem's supported case mapping.
 * @param character Unsigned character value to fold using the supported name encoding.
 * @return Folded character value using the supported case mapping.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static unsigned afatfs_apiUpper(unsigned character)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && character < 128) {
        return afatfs.upcaseAscii[character];
    }
#endif
    return character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character;
}

/**
 * @brief Render an on-disk 8.3 name using its lowercase-display flags.
 * @param out Destination for a rendered 8.3 name, including its terminator.
 * @param name Filename, pathname component or label used by this operation.
 * @param caseFlags FAT short-name lowercase-display flags.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Convert a native FAT directory entry to compatibility file information.
 * @param info Destination for decoded file or volume information.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param longName True when file information is being filled from a validated long-name sequence.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Inspect an already-open handle without opening a second file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param info Destination for decoded file or volume information.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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
    afatfs_exfatAdjustFileInfo(file, info);
    return AFATFS_OPERATION_SUCCESS;
}

/**
 * @brief Initialize compatibility directory enumeration from a native directory handle.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Acquire the next raw entry needed by compatibility directory enumeration.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param position Output or cursor describing a directory position or byte offset.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Match a filename against the compatibility wildcard pattern.
 * @param pattern Wildcard or FAT-fill pattern interpreted by this helper.
 * @param name Filename, pathname component or label used by this operation.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Decode and return the next supported FAT or exFAT directory item.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param info Destination for decoded file or volume information.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Read the next compatibility directory item without blocking on missing sectors.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param info Destination for decoded file or volume information.
 * @return Success with an empty info name at end-of-directory, pending status on cache wait, or failure.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
afatfsOperationStatus_e afatfs_readdir(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    afatfsOperationStatus_e status = afatfs_apiReadDirectory(dir, info);
    if (dir && dir->generation == afatfs_mediaGeneration) {
        dir->reading = status == AFATFS_OPERATION_IN_PROGRESS;
    }
    return status;
}
/**
 * @brief Return the next directory item matching the find-first wildcard pattern.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param info Destination for decoded file or volume information.
 * @return Success for the next match or an empty info name at end-of-directory; pending or failure otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
afatfsOperationStatus_e afatfs_findnext(afatfsDirectory_t *dir, afatfsFileInfo_t *info)
{
    return afatfs_readdir(dir, info);
}
/**
 * @brief Reset a compatibility directory iterator to its first entry.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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
/**
 * @brief Close a compatibility directory iterator through the native close state machine.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Claim the single caller-owned compatibility request slot.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True when the request slot was claimed; false when validation or ownership prevents starting.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Release the active compatibility request and report its final result.
 * @param result Final operation error code or result returned by the state machine.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Resume a compatibility request after native file-open completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_apiOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->file = file;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_NOT_FOUND;
    }
}
/**
 * @brief Resume a compatibility request after native file-close completion.
 * @param user Application context passed back to the completion callback.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_apiClosed(void *user)
{
    ((afatfsRequest_t *)user)->waiting = false;
}
/**
 * @brief Resume a compatibility request after native file-sync completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_apiSynced(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError;
    }
}

/**
 * @brief Allocate a native handle and start opening the name held by a compatibility request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param mode AFATFS_FILE_MODE_* flags used to open the request pathname.
 * @return True once a native handle was reserved and its open operation started; false when none is available.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Close temporary handles before completing a compatibility request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Queue a pathname lookup that returns compatibility file information.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param info Destination for decoded file or volume information.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue opening a directory for compatibility enumeration.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue directory opening and initialize a wildcard-filtered compatibility search.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param dir Compatibility directory iterator, updated as entries are consumed.
 * @param info Destination for decoded file or volume information.
 * @param name Filename, pathname component or label used by this operation.
 * @param pattern Wildcard or FAT-fill pattern interpreted by this helper.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Validate the compatibility drive selection for the single supported volume.
 * @param drive Compatibility drive index; this library supports one volume.
 * @return AFATFS_OPERATION_SUCCESS for drive zero; failure with AFATFS_ERROR_UNSUPPORTED otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
afatfsOperationStatus_e afatfs_chdrive(unsigned drive)
{
    return drive ? afatfs_apiError(AFATFS_ERROR_UNSUPPORTED) : AFATFS_OPERATION_SUCCESS;
}
/**
 * @brief Validate the compatibility code-page selection against the supported name encoding.
 * @param codePage Requested compatibility code-page identifier.
 * @return AFATFS_OPERATION_SUCCESS for code page 20127 (ASCII); unsupported-operation failure otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
afatfsOperationStatus_e afatfs_setcp(unsigned codePage)
{
    return codePage != 20127 ? afatfs_apiError(AFATFS_ERROR_UNSUPPORTED) : AFATFS_OPERATION_SUCCESS;
}
/**
 * @brief Seek an open file to byte zero through the native seek implementation.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
afatfsOperationStatus_e afatfs_rewind(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)
{
    return afatfs_fseek(file, 0, AFATFS_SEEK_SET, callback, user);
}

/**
 * @brief Prepare a compatibility request operating on a pathname.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param name Filename, pathname component or label used by this operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static bool afatfs_apiNamed(
    afatfsRequest_t *request, unsigned operation, const char *name, afatfsResultCallback_t callback, void *user)
{
    if (!name || !afatfs_apiBegin(request, operation, callback, user)) {
        return false;
    }
    request->name = name;
    return true;
}
/**
 * @brief Queue pathname-based file deletion through the native unlink implementation.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_unlink(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiNamed(request, AFATFS_API_UNLINK, name, callback, user);
}
/**
 * @brief Queue removal of an eligible empty directory.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_rmdir(afatfsRequest_t *request, const char *name, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiNamed(request, AFATFS_API_RMDIR, name, callback, user);
}
/**
 * @brief Queue renaming or moving a directory entry with ordered metadata publication.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param oldName Existing source pathname for the rename operation.
 * @param newName Destination pathname for the rename operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_rename(
    afatfsRequest_t *request, const char *oldName, const char *newName, afatfsResultCallback_t callback, void *user)
{
    if (!newName || !afatfs_apiNamed(request, AFATFS_API_RENAME, oldName, callback, user)) {
        return false;
    }
    request->otherName = newName;
    return true;
}
/**
 * @brief Queue an attribute update restricted by the supplied attribute mask.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param attr New attribute bits selected by the attribute mask.
 * @param mask Attribute bits that this operation is allowed to change.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue replacement of a pathname's packed FAT modification timestamp.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param modified Packed FAT modification date and time.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_utime(
    afatfsRequest_t *request, const char *name, uint32_t modified, afatfsResultCallback_t callback, void *user)
{
    if (!afatfs_apiNamed(request, AFATFS_API_UTIME, name, callback, user)) {
        return false;
    }
    request->value[0] = modified;
    return true;
}
/**
 * @brief Queue a free-space scan and return compatibility volume information.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param info Destination for decoded file or volume information.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_getfree(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user)
{
    if (!info || !afatfs_apiBegin(request, AFATFS_API_FREE, callback, user)) {
        return false;
    }
    request->output = info;
    request->value[0] = 2;
    return true;
}
/**
 * @brief Queue reading the supported volume-label metadata.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param info Destination for decoded file or volume information.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_getlabel(afatfsRequest_t *request, afatfsVolumeInfo_t *info, afatfsResultCallback_t callback, void *user)
{
    if (!info || !afatfs_apiBegin(request, AFATFS_API_GETLABEL, callback, user)) {
        return false;
    }
    request->output = info;
    return true;
}
/**
 * @brief Queue creation or replacement of the supported volume label.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue reconstruction of the current directory pathname into caller-owned storage.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param buffer Destination with capacity bytes for the pathname and its terminator.
 * @param capacity Capacity of caller-owned storage in bytes, including any required terminator.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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

/**
 * @brief Advance the compatibility free-space scan and assemble volume information.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Advance reading or publishing the volume-label entry.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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
/**
 * @brief Walk parent directories to construct the current working directory pathname.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Resume a metadata operation after opening its second native handle.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_apiOtherOpened(afatfsFilePtr_t file, void *user)
{
    afatfsRequest_t *request = (afatfsRequest_t *)user;
    request->other = file;
    request->waiting = false;
    if (!file) {
        request->result = afatfs.lastError ? afatfs.lastError : AFATFS_ERROR_EXISTS;
    }
}

/**
 * @brief Advance attribute, timestamp, deletion and related compatibility metadata operations.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Advance rename publication, source-entry removal and required checkpoints.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Prepare a compatibility request tied to an already-open native handle.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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
/**
 * @brief Queue a compatibility file open and return the handle through caller-owned storage.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param name Filename, pathname component or label used by this operation.
 * @param mode Compatibility access mode string interpreted by the native open implementation.
 * @param file Receives the opened native handle; retain this output storage until completion.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue closing an existing file through a compatibility request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_close(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_CLOSE, file, callback, user);
}
/**
 * @brief Queue durable synchronization of an existing file through a compatibility request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 *
 * @see afatfs_poll
 * @see afatfs_canPowerOff
 */
bool afatfs_sync(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_SYNC, file, callback, user);
}
/**
 * @brief Queue truncation at the current cursor through a compatibility request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_truncate(afatfsRequest_t *request, afatfsFilePtr_t file, afatfsResultCallback_t callback, void *user)
{
    return afatfs_apiFileRequest(request, AFATFS_API_TRUNCATE, file, callback, user);
}
/**
 * @brief Queue copying bytes from an open file into a caller-owned buffer.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param buffer Destination with room for count bytes; keep it valid until the callback completes.
 * @param count Number of bytes requested for a transfer.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue copying caller-owned bytes into an open file's buffered-write path.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param buffer Source containing count bytes; keep it unchanged until the callback completes.
 * @param count Number of bytes requested for a transfer.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue writing a null-terminated string without its terminator.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param text Null-terminated input text; keep valid while the request is active.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue reading a bounded, null-terminated text line.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param buffer Destination with capacity bytes for the line and its null terminator.
 * @param capacity Capacity of caller-owned storage in bytes, including any required terminator.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Format text into caller-owned scratch storage and queue its write.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param workspace Caller-owned formatting scratch buffer; keep valid until the write completes.
 * @param capacity Capacity of caller-owned storage in bytes, including any required terminator.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @param format printf-style formatting string.
 * @param ... Arguments corresponding to the format string.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Queue forwarding file bytes to an application consumer without a second sector buffer.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param consumer Application callback that accepts forwarded file bytes.
 * @param consumerUser Application context supplied to the forwarding consumer.
 * @param count Number of bytes requested for a transfer.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Advance compatibility byte, text and forwarding transfers using the native cache.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Queue validation or allocation of a contiguous extent for an open file.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param size Requested logical file size in bytes.
 * @param allocate True to allocate the validated extent; false to inspect availability only.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
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
/**
 * @brief Advance contiguous-extent validation and optional allocation.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/**
 * @brief Dispatch one step of the active compatibility operation.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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
/**
 * @brief Advance the active compatibility request while marking native calls as request-owned.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
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

/* ===== Optional format implementation; guarded by the enclosing feature condition. ===== */
/* Explicit-range quick format/partition operations, driven one sector at a time.
 * Only available while unmounted; cache[0] is the DMA buffer, never an extra sector array. */
/**
 * @brief Store a 32-bit little-endian field in a generated filesystem sector.
 * @param buffer At least four writable bytes receiving the little-endian value.
 * @param value Integer value to encode, store or round.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_formatPut32(uint8_t *buffer, uint32_t value)
{
    for (unsigned n = 0; n < 4; n++) {
        buffer[n] = (uint8_t)(value >> (8 * n));
    }
}

/**
 * @brief Claim an explicit-range formatting request only while the filesystem is unmounted.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param options Caller-owned format or partition options; keep valid until request completion.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
static bool afatfs_formatBegin(
    afatfsRequest_t *request, unsigned operation, const void *options, afatfsResultCallback_t callback, void *user)
{
    if (!request || !options || afatfs_request ||
        (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_STOPPED &&
            afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_UNKNOWN)) {
        afatfs.lastError = AFATFS_ERROR_BUSY;
        return false;
    }
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_INITIALIZATION);
    memset(request, 0, sizeof(*request));
    request->operation = (uint8_t)operation;
    request->output = (void *)options;
    request->callback = callback;
    request->user = user;
    request->checkpoint = true;
    afatfs_request = request;
    return true;
}

/**
 * @brief Queue quick formatting of an explicitly specified, validated sector range.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param options Caller-owned format or partition options; keep valid until request completion.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_mkfs(
    afatfsRequest_t *request, const afatfsFormatOptions_t *options, afatfsResultCallback_t callback, void *user)
{
    if (!options || !options->startSector || options->sectorCount < 128 ||
        options->sectorCount > UINT32_MAX - options->startSector ||
        (options->type != FAT_FILESYSTEM_TYPE_FAT16 && options->type != FAT_FILESYSTEM_TYPE_FAT32
#ifdef ASYNCFS_ENABLE_EXFAT
            && options->type != FAT_FILESYSTEM_TYPE_EXFAT
#endif
            )) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        return false;
    }
    uint32_t spc = options->sectorsPerCluster ? options->sectorsPerCluster : 1;
    uint32_t fatSectors = 0;
    uint32_t clusters = 0;
    uint32_t heap = 0;
    for (;;) {
        if (!isPowerOfTwo(spc) || spc > (options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 65536U : 128U)) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        uint32_t reserved = options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 24
            : options->type == FAT_FILESYSTEM_TYPE_FAT32               ? 32
                                                                       : 1;
        uint32_t fats = options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 1 : 2;
        uint32_t root = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 32 : 0;
        fatSectors = (uint32_t)((((uint64_t)options->sectorCount / spc + 2) *
                                        (options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4) +
                                    511) /
            512);
        uint64_t start = (uint64_t)reserved + (uint64_t)fats * fatSectors + root;
        if (options->type == FAT_FILESYSTEM_TYPE_EXFAT) {
            start = (start + spc - 1) / spc * spc;
        }
        if (start >= options->sectorCount) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        heap = (uint32_t)start;
        clusters = (options->sectorCount - heap) / spc;
        uint32_t maximum = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT16_MAX_CLUSTERS
            : options->type == FAT_FILESYSTEM_TYPE_FAT32              ? 0x0FFFFFF5U
                                                                      : 0xFFFFFFF5U;
        uint32_t minimum = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT12_MAX_CLUSTERS + 1
            : options->type == FAT_FILESYSTEM_TYPE_FAT32              ? FAT16_MAX_CLUSTERS + 1
                                                                      : 256;
        if (clusters <= maximum && clusters >= minimum) {
            break;
        }
        if (options->sectorsPerCluster || clusters < minimum) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        spc *= 2;
    }
    if (!afatfs_formatBegin(request, AFATFS_API_MKFS, options, callback, user)) {
        return false;
    }
    request->value[0] = options->sectorCount;
    request->value[1] = spc;
    request->value[2] = fatSectors;
    request->value[3] = heap;
    request->value[4] = clusters;
    request->value[12] = options->type;
    if (options->type == FAT_FILESYSTEM_TYPE_EXFAT) {
        request->value[5] = (uint32_t)(((uint64_t)clusters + 7) / 8 + (uint64_t)spc * 512 - 1) / (spc * 512);
        request->value[6] = 2 + request->value[5];
        request->value[7] = request->value[6] + 1;
        request->value[11] = request->value[5] + 2;
    } else {
        request->value[7] = options->type == FAT_FILESYSTEM_TYPE_FAT32 ? 2 : 0;
    }
    return true;
}

/**
 * @brief Queue partition-table creation within an explicitly specified device range.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param options Caller-owned format or partition options; keep valid until request completion.
 * @param callback Completion callback, or NULL when no completion notification is needed.
 * @param user Application context passed back to the completion callback.
 * @return True if the asynchronous request was accepted; false if it could not be started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
bool afatfs_fdisk(
    afatfsRequest_t *request, const afatfsPartitionOptions_t *options, afatfsResultCallback_t callback, void *user)
{
    if (!options || options->sectorCount < (options->gpt ? 68U : 2U)) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        return false;
    }
#ifndef ASYNCFS_ENABLE_EXFAT
    if (options->gpt) {
        afatfs.lastError = AFATFS_ERROR_UNSUPPORTED;
        return false;
    }
#endif
    if (options->gpt) {
        bool nonzero = false;
        for (unsigned n = 0; n < 16; n++) {
            nonzero |= options->diskGuid[n] != 0;
        }
        if (!nonzero) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
    }
    for (unsigned n = 0; n < 4; n++) {
        if (!options->length[n]) {
            continue;
        }
        uint64_t end = (uint64_t)options->start[n] + options->length[n];
        if (options->start[n] < (options->gpt ? 34U : 1U) || end > options->sectorCount - (options->gpt ? 33U : 0U) ||
            (!options->gpt && !options->type[n])) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        for (unsigned m = 0; m < n; m++) {
            if (options->length[m] && options->start[n] < (uint64_t)options->start[m] + options->length[m] &&
                options->start[m] < end) {
                afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
                return false;
            }
        }
    }
    return afatfs_formatBegin(request, AFATFS_API_FDISK, options, callback, user);
}

/**
 * @brief Generate the supported exFAT uppercase-table sector.
 * @param buffer Existing 512-byte sector buffer receiving uppercase-table data.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_formatUpcase(uint8_t *buffer)
{
    memset(buffer, 0, 512);
    for (unsigned n = 0; n < 128; n++) {
        buffer[n * 2] = (uint8_t)afatfs_apiUpper(n);
    }
    buffer[256] = buffer[257] = 0xFF;
    buffer[258] = 0x7F;
    buffer[259] = 0xFF;
    buffer[260] = buffer[261] = 0xFF;
}

/**
 * @brief Generate one FAT or exFAT format sector in the existing aligned cache buffer.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param relative Sector offset within the filesystem being formatted.
 * @param buffer Existing 512-byte aligned sector buffer receiving generated format data.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
static void afatfs_formatVolumeSector(afatfsRequest_t *request, uint32_t relative, uint8_t *buffer)
{
    const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
    uint32_t spc = request->value[1];
    uint32_t fatSectors = request->value[2];
    uint32_t heap = request->value[3];
    uint32_t count = request->value[4];
    bool exfat = options->type == FAT_FILESYSTEM_TYPE_EXFAT;
    bool fat32 = options->type == FAT_FILESYSTEM_TYPE_FAT32;
    memset(buffer, 0, 512);
    if ((exfat && relative < 24) || relative == 0 || (fat32 && relative == 6)) {
        uint32_t boot = exfat ? relative % 12 : 0;
        if (boot == 0) {
            fatVolumeID_t *volume = (fatVolumeID_t *)buffer;
            volume->jmpBoot[0] = 0xEB;
            volume->jmpBoot[1] = exfat ? 0x76 : fat32 ? 0x58 : 0x3C;
            volume->jmpBoot[2] = 0x90;
            memcpy(volume->oemName, exfat ? "EXFAT   " : "ASYNCFS ", 8);
            if (exfat) {
                volume->exFat.partitionOffset = options->startSector;
                volume->exFat.volumeLength = options->sectorCount;
                volume->exFat.fatOffset = 24;
                volume->exFat.fatLength = fatSectors;
                volume->exFat.clusterHeapOffset = heap;
                volume->exFat.clusterCount = count;
                volume->exFat.rootDirectoryCluster = request->value[7];
                volume->exFat.volumeSerialNumber = options->serial;
                volume->exFat.fileSystemRevision = 0x100;
                volume->exFat.bytesPerSectorShift = 9;
                for (uint32_t n = spc; n > 1; n >>= 1) {
                    volume->exFat.sectorsPerClusterShift++;
                }
                volume->exFat.numFATs = 1;
                volume->exFat.driveSelect = 0x80;
                volume->exFat.percentInUse = (uint8_t)((uint64_t)request->value[11] * 100 / count);
            } else {
                volume->fat.bytesPerSector = 512;
                volume->fat.sectorsPerCluster = (uint8_t)spc;
                volume->fat.reservedSectorCount = fat32 ? 32 : 1;
                volume->fat.numFATs = 2;
                volume->fat.rootEntryCount = fat32 ? 0 : 512;
                volume->fat.media = 0xF8;
                volume->fat.hiddenSectors = options->startSector;
                volume->fat.sectorsPerTrack = 32;
                volume->fat.numHeads = 64;
                if (options->sectorCount < 65536) {
                    volume->fat.totalSectors16 = (uint16_t)options->sectorCount;
                } else {
                    volume->fat.totalSectors32 = options->sectorCount;
                }
                if (fat32) {
                    fat32Descriptor_t *descriptor = &volume->fat.fatDescriptor.fat32;
                    descriptor->FATSize32 = fatSectors;
                    descriptor->rootCluster = 2;
                    descriptor->fsInfo = 1;
                    descriptor->backupBootSector = 6;
                    descriptor->driveNumber = 0x80;
                    descriptor->bootSignature = 0x29;
                    descriptor->volumeID = options->serial;
                    memcpy(descriptor->volumeLabel, "NO NAME    ", 11);
                    memcpy(descriptor->fileSystemType, "FAT32   ", 8);
                } else {
                    fat16Descriptor_t *descriptor = &volume->fat.fatDescriptor.fat16;
                    volume->fat.FATSize16 = (uint16_t)fatSectors;
                    descriptor->driveNumber = 0x80;
                    descriptor->bootSignature = 0x29;
                    descriptor->volumeID = options->serial;
                    memcpy(descriptor->volumeLabel, "NO NAME    ", 11);
                    memcpy(descriptor->fileSystemType, "FAT16   ", 8);
                }
            }
            buffer[510] = 0x55;
            buffer[511] = 0xAA;
        } else if (boot <= 8) {
            afatfs_formatPut32(buffer + 508, 0xAA550000);
        } else if (boot == 11) {
            for (unsigned n = 0; n < 512; n += 4) {
                afatfs_formatPut32(buffer + n, request->value[9]);
            }
        }
        return;
    }
    if (fat32 && (relative == 1 || relative == 7)) {
        afatfs_formatPut32(buffer, 0x41615252);
        afatfs_formatPut32(buffer + 484, 0x61417272);
        afatfs_formatPut32(buffer + 488, count - 1);
        afatfs_formatPut32(buffer + 492, 3);
        afatfs_formatPut32(buffer + 508, 0xAA550000);
        return;
    }
    uint32_t fatStart = exfat ? 24 : fat32 ? 32 : 1;
    if (relative >= fatStart && relative - fatStart < fatSectors * (exfat ? 1U : 2U)) {
        uint32_t width = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4;
        uint32_t base = (relative - fatStart) % fatSectors * (512 / width);
        for (unsigned n = 0; n < 512 / width; n++) {
            uint32_t cluster = base + n;
            uint32_t value = 0;
            if (cluster == 0) {
                value = width == 2 ? 0xFFF8 : 0xFFFFFFF8;
            } else if (cluster == 1 || (fat32 && cluster == 2)) {
                value = 0xFFFFFFFF;
            } else if (exfat && cluster >= 2 && cluster < 2 + request->value[5]) {
                value = cluster + 1 == 2 + request->value[5] ? 0xFFFFFFFF : cluster + 1;
            } else if (exfat && (cluster == request->value[6] || cluster == request->value[7])) {
                value = 0xFFFFFFFF;
            }
            if (width == 4) {
                afatfs_formatPut32(buffer + n * 4, value);
            } else {
                buffer[n * 2] = (uint8_t)value;
                buffer[n * 2 + 1] = (uint8_t)(value >> 8);
            }
        }
        return;
    }
    if (!exfat || relative < heap) {
        return;
    }
    uint32_t dataSector = relative - heap;
    if (dataSector < request->value[5] * spc) {
        uint32_t byteBase = dataSector * 512;
        for (unsigned n = 0; n < 512; n++) {
            uint64_t bit = (uint64_t)(byteBase + n) * 8;
            for (unsigned b = 0; b < 8; b++) {
                if (bit + b < request->value[11] || bit + b >= count) {
                    buffer[n] |= (uint8_t)(1U << b);
                }
            }
        }
    } else if (dataSector == (request->value[6] - 2) * spc) {
        afatfs_formatUpcase(buffer);
    } else if (dataSector == (request->value[7] - 2) * spc) {
        exFatUpTableEntry_t *bitmap = (exFatUpTableEntry_t *)buffer;
        exFatUpTableEntry_t *upcase = bitmap + 1;
        bitmap->type = EXFAT_DIR_BITMAP;
        bitmap->firstCluster = 2;
        bitmap->dataSize = ((uint64_t)count + 7) / 8;
        upcase->type = EXFAT_DIR_UPCASE;
        upcase->firstCluster = request->value[6];
        upcase->dataSize = 262;
        upcase->tableChecksum = request->value[10];
    }
}

/**
 * @brief Generate one partition-table sector for the active partitioning request.
 * @param request Caller-owned request workspace; keep it valid until completion.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @param buffer Existing 512-byte aligned sector buffer receiving partition-table data.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 *
 * @note Drive completion with afatfs_poll(). Request acceptance does not imply durable storage.
 * @note Keep the request, referenced names and buffers valid until completion; do not modify pending input data.
 */
static void afatfs_formatPartitionSector(afatfsRequest_t *request, uint32_t sector, uint8_t *buffer)
{
    const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
    memset(buffer, 0, 512);
    if (!sector) {
        mbrPartitionEntry_t *entries = (mbrPartitionEntry_t *)(buffer + 446);
        if (options->gpt) {
            entries[0].type = MBR_PARTITION_TYPE_GPT;
            entries[0].lbaBegin = 1;
            entries[0].numSectors = options->sectorCount - 1;
            entries[0].chsBegin[1] = 2;
            memset(entries[0].chsEnd, 0xFF, 3);
        } else {
            for (unsigned n = 0; n < 4; n++) {
                if (options->length[n]) {
                    entries[n].type = options->type[n];
                    entries[n].lbaBegin = options->start[n];
                    entries[n].numSectors = options->length[n];
                    memset(entries[n].chsBegin, 0xFF, 3);
                    memset(entries[n].chsEnd, 0xFF, 3);
                }
            }
        }
        buffer[510] = 0x55;
        buffer[511] = 0xAA;
        return;
    }
    if (!options->gpt) {
        return;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (sector == 1 || sector == options->sectorCount - 1) {
        memcpy(buffer, "EFI PART", 8);
        afatfs_formatPut32(buffer + 8, 0x10000);
        afatfs_formatPut32(buffer + 12, 92);
        afatfs_formatPut32(buffer + 24, sector);
        afatfs_formatPut32(buffer + 32, sector == 1 ? options->sectorCount - 1 : 1);
        afatfs_formatPut32(buffer + 40, 34);
        afatfs_formatPut32(buffer + 48, options->sectorCount - 34);
        memcpy(buffer + 56, options->diskGuid, 16);
        afatfs_formatPut32(buffer + 72, sector == 1 ? 2 : options->sectorCount - 33);
        afatfs_formatPut32(buffer + 80, 128);
        afatfs_formatPut32(buffer + 84, 128);
        afatfs_formatPut32(buffer + 88, request->value[9]);
        afatfs_formatPut32(buffer + 16, ~afatfs_crc32Update(UINT32_MAX, buffer, 92));
    } else if (sector == 2 || sector == options->sectorCount - 33) {
        static const uint8_t basic[16] = {
            0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
        for (unsigned n = 0; n < 4; n++) {
            if (options->length[n]) {
                uint8_t *entry = buffer + n * 128;
                memcpy(entry, basic, 16);
                memcpy(entry + 16, options->diskGuid, 16);
                entry[31] ^= (uint8_t)(n + 1);
                entry[16] |= 1;
                afatfs_formatPut32(entry + 32, options->start[n]);
                afatfs_formatPut32(entry + 40, options->start[n] + options->length[n] - 1);
            }
        }
    }
#else
    (void)request;
#endif
}

/**
 * @brief Acknowledge a formatting transfer only for its original media generation.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @param buffer Original format-sector buffer, or NULL when the backend transfer failed.
 * @param generation Media-generation identifier supplied when the transfer started.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_formatComplete(
    sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    (void)operation;
    (void)sector;
    if (generation != afatfs_mediaGeneration || !afatfs_request) {
        return;
    }
    afatfs_request->waiting = false;
    afatfs_request->value[15] = buffer ? 1 : 2;
    if (!buffer) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }
}
/**
 * @brief Advance formatting or partitioning one sector transfer at a time.
 * @param request Caller-owned request workspace; keep it valid until completion.
 *
 * @note Available when ASYNCFS_ENABLE_FATFS_API is enabled.
 */
static void afatfs_apiFormat(afatfsRequest_t *request)
{
    if (request->waiting) {
        return;
    }
    uint8_t *buffer = afatfs.cache;
    bool disk = request->operation == AFATFS_API_FDISK;
    if (!request->phase) {
        if (disk) {
            const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (options->gpt) {
                if (!request->value[13]) {
                    request->value[9] = UINT32_MAX;
                }
                afatfs_formatPartitionSector(request, 2 + request->value[13], buffer);
                request->value[9] = afatfs_crc32Update(request->value[9], buffer, 512);
                if (++request->value[13] < 32) {
                    return;
                }
                request->value[9] = ~request->value[9];
            }
#else
            (void)options;
#endif
        } else if (request->value[12] == FAT_FILESYSTEM_TYPE_EXFAT) {
            afatfs_formatUpcase(buffer);
            for (unsigned n = 0; n < 262; n++) {
                request->value[10] = (request->value[10] >> 1 | request->value[10] << 31) + buffer[n];
            }
            for (unsigned page = 0; page < 11; page++) {
                afatfs_formatVolumeSector(request, page, buffer);
                for (unsigned n = 0; n < 512; n++) {
                    if (page || (n != 106 && n != 107 && n != 112)) {
                        request->value[9] = (request->value[9] >> 1 | request->value[9] << 31) + buffer[n];
                    }
                }
            }
        }
        request->phase = 1;
    }
    uint32_t last;
    uint32_t sector;
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        last = options->gpt ? 67 : 3;
        sector = request->value[8] < 34 ? request->value[8] : options->sectorCount - 33 + request->value[8] - 34;
    } else {
        const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
        last = request->value[3] +
            (options->type == FAT_FILESYSTEM_TYPE_EXFAT          ? request->value[11] * request->value[1]
                    : options->type == FAT_FILESYSTEM_TYPE_FAT32 ? request->value[1]
                                                                 : 0);
        sector = options->startSector + request->value[8];
    }
    if (request->value[15] == 1) {
        request->value[8]++;
        request->value[15] = 0;
    }
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        // Clear stale GPT signatures when switching to MBR, without touching any new partition's data.
        while (!options->gpt && request->value[8] > 0 && request->value[8] < last) {
            uint32_t probe = request->value[8] == 1 ? 1 : options->sectorCount - 1;
            bool occupied = request->value[8] == 2 && options->sectorCount == 2;
            for (unsigned n = 0; n < 4; n++) {
                occupied |= options->length[n] && probe >= options->start[n] &&
                    (uint64_t)probe < (uint64_t)options->start[n] + options->length[n];
            }
            if (!occupied) {
                break;
            }
            request->value[8]++;
        }
    }
    if (request->value[8] >= last) {
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
        sdcardOperationStatus_e stopped = sdcard_endWriteBlocks();
        if (stopped != SDCARD_OPERATION_SUCCESS) {
            if (stopped == SDCARD_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            }
            return;
        }
#endif
        if (!sdcard_poll()) {
            return;
        }
        afatfsResultCallback_t callback = request->callback;
        void *user = request->user;
        afatfs_request = NULL;
        afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_STOPPED);
        request->complete = true;
        request->result = AFATFS_ERROR_NONE;
        request->driving = false;
        if (callback) {
            callback(AFATFS_ERROR_NONE, user);
        }
        return;
    }
    // Recompute after advancing an acknowledged sector. The buffer remains untouched while DMA owns it.
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        sector = options->gpt
            ? (request->value[8] < 34 ? request->value[8] : options->sectorCount - 33 + request->value[8] - 34)
            : request->value[8] == 2 ? options->sectorCount - 1
                                     : request->value[8];
        afatfs_formatPartitionSector(request, sector, buffer);
    } else {
        const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
        sector = options->startSector + request->value[8];
        afatfs_formatVolumeSector(request, request->value[8], buffer);
    }
    request->value[15] = 0;
    switch (sdcard_writeBlock(sector, buffer, afatfs_formatComplete, afatfs_mediaGeneration)) {
        case SDCARD_OPERATION_IN_PROGRESS:
            request->waiting = true;
            break;
        case SDCARD_OPERATION_SUCCESS:
            request->value[15] = 1;
            break;
        case SDCARD_OPERATION_FAILURE:
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            break;
        default:
            break;
    }
}

#endif

/* ==================== LFN ==================== */

#ifdef ASYNCFS_ENABLE_LFN
/**
 * @brief Test whether deletion must resume an unfinished LFN sequence.
 * @param file Handle retaining the directory deletion progress.
 * @param mode Requested directory-entry publication mode.
 * @return True when the caller must resume the recorded LFN deletion.
 */
static AFATFS_INLINE bool afatfs_lfnDeletionPending(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    return mode == AFATFS_SAVE_DIRECTORY_DELETED && file->lfnDeleteIndex;
}

/**
 * @brief Begin LFN cleanup only after the short directory entry was deleted.
 * @param file Handle whose short entry has already been marked deleted.
 * @param mode Requested directory-entry publication mode.
 * @return True after initializing LFN deletion progress; false when no cleanup is required.
 */
static AFATFS_INLINE bool afatfs_lfnBeginDeletion(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    if (mode == AFATFS_SAVE_DIRECTORY_DELETED && file->lfnEntryCount) {
        file->lfnDeleteIndex = 1;
        return true;
    }
    return false;
}

/**
 * @brief Test whether an open file owns an unfinished LFN output operation.
 * @param file File handle whose optional work is inspected.
 * @return True while a live handle retains the caller-owned long-name output.
 */
static AFATFS_INLINE bool afatfs_lfnFileHasPendingWork(afatfsFilePtr_t file)
{
    return file->type != AFATFS_FILE_TYPE_NONE && file->lfnOutput;
}

/* ===== Optional lfn implementation; guarded by the enclosing feature condition. ===== */
/* VFAT support uses the original CWD walker, cache and per-file operation union.
 * Only the requested name lives in the operation workspace; sectors are streamed.
 * See Microsoft FAT specification, section 7 (ordinals, checksum and padding). */
static const uint8_t afatfs_lfnOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

/**
 * @brief Fold a supported VFAT name character for case-insensitive comparison.
 * @param character Unsigned character value to fold using the supported name encoding.
 * @return ASCII uppercase value, or the original non-lowercase character.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
static unsigned afatfs_lfnUpper(unsigned character)
{
    return character >= 'a' && character <= 'z' ? character - 'a' + 'A' : character;
}

/**
 * @brief Validate a pathname component against the supported VFAT character and length rules.
 * @param name Filename, pathname component or label used by this operation.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Decode one UTF-16 code unit from a VFAT long-name directory entry.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param index UTF-16 code-unit index within the 13-unit VFAT entry.
 * @return Decoded 16-bit VFAT name code unit.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
static uint16_t afatfs_lfnUnit(const uint8_t *entry, unsigned index)
{
    unsigned offset = afatfs_lfnOffsets[index];
    return entry[offset] | ((uint16_t)entry[offset + 1] << 8);
}

// Invalid/orphan chains fall back to the short entry; they never name another file.
/**
 * @brief Accumulate and validate a streaming VFAT name chain before its short entry.
 * @param state Streaming VFAT-chain workspace, updated for each directory entry.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param wanted Name to compare with the decoded directory entry.
 * @param output Optional destination for the decoded name; NULL when only matching is needed.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Test whether a validated long-name chain belongs to the following short entry.
 * @param state Decoded VFAT-chain state preceding the short directory entry.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
static bool afatfs_lfnAttached(const afatfsLongNameState_t *state, const uint8_t *entry)
{
    return state->count && !state->remaining && state->checksum == fat_createCheckSum((const char *)entry);
}

// Returns true for a legal 8.3 spelling, even if a case-preserving LFN is needed.
/**
 * @brief Determine whether a component can be represented directly as an 8.3 disk name.
 * @param name Filename, pathname component or label used by this operation.
 * @param output Eleven-byte destination for the padded on-disk short name.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Generate the next short-name alias candidate for a pending long-name creation.
 * @param state Creation workspace holding the requested name and alias attempt.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Remember the first directory slot occupied by a file's validated long-name chain.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param finder Native directory traversal workspace.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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
/**
 * @brief Acquire a selected directory slot belonging to a file's long-name entry sequence.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param index Entry offset within the recorded long-name/short-name sequence.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param writing True to acquire or submit writable data; false to read existing data.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Generate one VFAT long-name entry with its ordinal, checksum and padding.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @param name Filename, pathname component or label used by this operation.
 * @param ordinal One-based position of this entry in the VFAT long-name sequence.
 * @param count Total number of long-name entries in the sequence.
 * @param checksum Checksum linking the long-name entries to their short entry.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

/**
 * @brief Remove the long-name entries attached to a file while respecting metadata journaling.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

#if ASYNCFS_ENABLE_FATFS_API
// Caller-owned output survives cache misses; no 256-byte buffer is added to a handle.
/**
 * @brief Recover a file's long name from its recorded directory-entry sequence.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param info Destination for decoded file or volume information.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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

#endif

/**
 * @brief Advance long-name lookup, alias selection and ordered directory publication.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_LFN is enabled.
 */
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
                memset(&state->scan.lfn, 0, sizeof(state->scan.lfn));
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
                    memset(&state->scan.lfn, 0, sizeof(state->scan.lfn));
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
                    state->scan.counters.freeEntries = 0;
                    state->scan.counters.writeIndex = 0;
                    state->phase = AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB;
                    break;
                }
                uint8_t *bytes = (uint8_t *)entry;
                if (fat_isDirectoryEntryEmpty(entry)) {
                    memset(&state->scan.lfn, 0, sizeof(state->scan.lfn));
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
                    afatfs_lfnConsume(&state->scan.lfn, bytes, finding ? name : candidate, NULL);
                    break;
                }
                bool attached = afatfs_lfnAttached(&state->scan.lfn, bytes);
                bool matched = (attached && state->scan.lfn.matched) ||
                    ((!finding || !state->location.aliasNumber) && !memcmp(bytes, state->shortName, 11));
                if ((!finding || !fat_isVolumeLabel(entry)) && matched) {
#if ASYNCFS_ENABLE_FATFS_API
                    // A rename may resolve the new spelling to its own source (case/alias change).
                    // Keep the source occupied during alias selection, then use the normal publish/delete flow.
                    if (finding && (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) && afatfs_request &&
                        afatfs_request->operation == AFATFS_API_RENAME && afatfs_request->file &&
                        afatfs_request->file->directoryEntryPos.sectorNumberPhysical ==
                            file->directoryEntryPos.sectorNumberPhysical &&
                        afatfs_request->file->directoryEntryPos.entryIndex == file->directoryEntryPos.entryIndex) {
                        memset(&state->scan.lfn, 0, sizeof(state->scan.lfn));
                        break;
                    }
#endif
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
                        file->lfnEntryCount = attached ? state->scan.lfn.count : 0;
                        file->lfnAscii = attached && state->scan.lfn.ascii;
                        if (file->mode & AFATFS_FILE_MODE_EXCLUSIVE_CREATE) {
                            file->error = AFATFS_ERROR_EXISTS;
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs_findLastCWD(&afatfs.currentDirectory);
                        state->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                        return AFATFS_OPERATION_SUCCESS;
                    }
                }
                memset(&state->scan.lfn, 0, sizeof(state->scan.lfn));
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
                    state->scan.counters.freeEntries = 0;
                    state->location.orphanSector =
                        fat_isLfnEntry(entry) ? file->directoryEntryPos.sectorNumberPhysical : 0;
                    state->orphanIndex = (uint8_t)file->directoryEntryPos.entryIndex;
                    break;
                }
                if (!state->scan.counters.freeEntries) {
                    afatfs_lfnRememberStart(file, &file->directoryEntryPos);
                }
                if (++state->scan.counters.freeEntries <= file->lfnEntryCount) {
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
                while (state->scan.counters.writeIndex < file->lfnEntryCount) {
                    uint8_t *bytes;
                    status = afatfs_lfnEntry(file, state->scan.counters.writeIndex, &bytes, true);
                    if (status != AFATFS_OPERATION_SUCCESS) {
                        return status;
                    }
                    afatfs_lfnWrite(bytes, name, file->lfnEntryCount - state->scan.counters.writeIndex,
                        file->lfnEntryCount, fat_createCheckSum((const char *)state->shortName));
                    state->scan.counters.writeIndex++;
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
#endif

/* ==================== exFAT ==================== */

/**
 * @brief Read the exFAT stream information associated with an open file.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param entry Receives a borrowed pointer into the file handle, or NULL when no entry set is stored; do not
 * retain it across handle mutation or close.
 * @param stream Receives the borrowed stream entry in the file handle, or NULL when no entry set is stored;
 * lifetime matches entry.
 * @return True when the stored exFAT entries have been returned; false for other filesystems, a busy file or a
 * build without exFAT.
 */
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

#ifdef ASYNCFS_ENABLE_EXFAT
/**
 * @brief Reconcile exFAT data lengths before publishing the truncated directory entry.
 * @param file Handle whose truncation cursor and allocated size have been updated.
 * @param targetSize Logical size selected by the existing truncate operation.
 */
static AFATFS_INLINE void afatfs_exfatFinishTruncate(afatfsFilePtr_t file, uint32_t targetSize)
{
    file->validDataLength = MIN(file->validDataLength, targetSize);
    file->committedDataLength = file->validDataLength;
    file->dataDirty = false;
    if (!targetSize) {
        file->exfatNoFatChain = false;
    }
}

/**
 * @brief Test whether exFAT allocation is represented without a FAT chain.
 * @param file Handle whose allocation representation is inspected.
 * @return True for an exFAT NoFatChain stream; false for FAT-linked streams.
 */
static AFATFS_INLINE bool afatfs_exfatLinearAllocation(afatfsFilePtr_t file)
{
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && file->exfatNoFatChain;
}

/**
 * @brief Select the existing bounded release range for an exFAT truncate operation.
 * @param file Handle with an initialized truncate workspace and unchanged allocation metadata.
 */
static AFATFS_INLINE void afatfs_exfatConfigureTruncate(afatfsFilePtr_t file)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        opState->endCluster =
            file->exfatNoFatChain ? file->firstCluster + file->physicalSize / afatfs_clusterSize() : 0;
        opState->linear = file->exfatNoFatChain;
    }
}

/**
 * @brief Recognize a protective GPT partition when exFAT/GPT support is enabled.
 * @param partitionType MBR partition type byte being inspected.
 * @return True for a protective GPT entry.
 */
static AFATFS_INLINE bool afatfs_exfatProtectivePartition(uint8_t partitionType)
{
    return partitionType == MBR_PARTITION_TYPE_GPT;
}

/**
 * @brief Validate a protective MBR entry and initialize GPT discovery state.
 * @param entry Protective MBR entry selected by the existing partition scan.
 * @return True after selecting GPT discovery; false for an invalid protective extent.
 */
static AFATFS_INLINE bool afatfs_exfatSelectGPT(const mbrPartitionEntry_t *entry)
{
    if (entry->lbaBegin != 1 || entry->numSectors < 67) {
        return false;
    }
    afatfs.gpt.headerSector = 1;
    afatfs.gpt.lastSector = entry->numSectors;
    afatfs.gpt.backupKnown = entry->numSectors != UINT32_MAX;
    afatfs.partitionType = MBR_PARTITION_TYPE_GPT;

    return true;
}

/**
 * @brief Recognize an exFAT partition in a legacy MBR.
 * @param partitionType MBR partition type byte being inspected.
 * @return True for an exFAT legacy partition.
 */
static AFATFS_INLINE bool afatfs_exfatLegacyPartition(uint8_t partitionType)
{
    return partitionType == MBR_PARTITION_TYPE_EXFAT;
}

/**
 * @brief Recognize selected writable or read-only exFAT volumes before boot validation.
 * @return True when the selected partition requires the exFAT boot parser.
 */
static AFATFS_INLINE bool afatfs_exfatVolumeSelected(void)
{
    return afatfs.partitionType == MBR_PARTITION_TYPE_EXFAT ||
        afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY;
}

/**
 * @brief Validate exFAT geometry and preserve dirty-volume and GPT read-only protections.
 * @param sector Cached boot sector for the selected exFAT volume.
 * @return True for supported geometry; false for invalid, unsupported or out-of-range fields.
 */
static AFATFS_INLINE bool afatfs_exfatParseVolumeID(const uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *)sector;
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
    memcpy(&afatfs.journal.serial, sector + 100, 4);
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
    return true;
}

/**
 * @brief Route a protective MBR into GPT discovery before volume parsing.
 */
static AFATFS_INLINE void afatfs_exfatSelectMountPhase(void)
{
    if (afatfs.partitionType == MBR_PARTITION_TYPE_GPT) {
        afatfs.initPhase = AFATFS_INITIALIZATION_READ_GPT_HEADER;
    }
}

/**
 * @brief Consume one initialization step for the existing GPT discovery phases.
 * @return True when a GPT phase consumed this polling pass; false for other initialization phases.
 */
static AFATFS_INLINE bool afatfs_exfatContinueGPT(void)
{
    switch (afatfs.initPhase) {
        case AFATFS_INITIALIZATION_READ_GPT_HEADER:
        case AFATFS_INITIALIZATION_READ_GPT_ARRAY:
        case AFATFS_INITIALIZATION_READ_GPT_ENTRY:
        case AFATFS_INITIALIZATION_READ_GPT_VOLUME:
        case AFATFS_INITIALIZATION_CHECK_GPT_OVERLAP:
            if (afatfs_gptContinue() == AFATFS_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BAD_MBR;
                afatfs.fileSystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
            return true;
        default:
            return false;
    }
}

/**
 * @brief Advance exFAT root discovery without starting FAT-only freefile initialization.
 */
static AFATFS_INLINE void afatfs_exfatMountRoot(void)
{
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
}

/**
 * @brief Test whether the mounted filesystem uses the enabled exFAT implementation.
 * @return True only for an active exFAT filesystem.
 */
static AFATFS_INLINE bool afatfs_exfatActive(void)
{
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT;
}

/**
 * @brief Prepare and publish an exFAT entry set using the existing ordered metadata writer.
 * @param file Handle owning the entry-set bytes and publication cursor.
 * @param mode Size-publication or deletion mode selected by the shared state machine.
 * @return The entry-set writer status, or failure for an incomplete entry set.
 */
static AFATFS_INLINE afatfsOperationStatus_e afatfs_exfatSaveDirectoryEntry(
    afatfsFilePtr_t file, afatfsSaveDirectoryEntryMode_e mode)
{
    afatfsOperationStatus_e result;
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

/**
 * @brief Record exFAT data publication after cache and journal barriers completed.
 * @param file Handle whose data and metadata have passed the synchronization barriers.
 */
static AFATFS_INLINE void afatfs_exfatCompleteSync(afatfsFilePtr_t file)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        file->committedDataLength = MIN(file->logicalSize, file->validDataLength);
        file->dataDirty = false;
    }
}

/**
 * @brief Track written exFAT data before advancing the file cursor.
 * @param file Handle whose current cursor identifies the written range.
 * @param writtenBytes Number of bytes copied into the current cached sector.
 */
static AFATFS_INLINE void afatfs_exfatAccountWrite(afatfsFilePtr_t file, uint32_t writtenBytes)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        file->validDataLength = MAX(file->validDataLength, file->cursorOffset + writtenBytes);
        file->dataDirty = true;
    }
}

/**
 * @brief Apply exFAT valid-length and contiguous-chain fields to file information.
 * @param file Handle whose exFAT stream state is being reported.
 * @param info File information already populated by the shared stat path.
 */
static AFATFS_INLINE void afatfs_exfatAdjustFileInfo(afatfsFilePtr_t file, afatfsFileInfo_t *info)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        info->validSize = MIN(info->size, file->validDataLength);
        info->contiguous = file->exfatNoFatChain;
    }
}

/**
 * @brief Acquire exFAT allocation ownership after the required journal checkpoint.
 * @param file File whose truncate operation and allocation owner are inspected.
 * @return True when truncation may proceed; false while checkpoint or owner progress is required.
 */
static AFATFS_INLINE bool afatfs_exfatPrepareTruncate(afatfsFilePtr_t file)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (opState->phase == AFATFS_TRUNCATE_FILE_INITIAL && !afatfs_journalCommitted()) {
            afatfs.journal.blockAllocation = true;
            if (afatfs.allocationOwner == file) {
                afatfs.allocationOwner = NULL;
            }
            return false;
        }
        if (afatfs.journal.blockAllocation && opState->phase != AFATFS_TRUNCATE_FILE_SUCCESS) {
            return false;
        }
        if (afatfs.allocationOwner && afatfs.allocationOwner != file) {
            return false;
        }
        afatfs.allocationOwner = file;
    }
    return true;
}

/**
 * @brief Test whether an open file has exFAT data awaiting publication.
 * @param file File handle whose optional work is inspected.
 * @return True while a live handle has unpublished data.
 */
static AFATFS_INLINE bool afatfs_exfatFileHasPendingWork(afatfsFilePtr_t file)
{
    return file->type != AFATFS_FILE_TYPE_NONE && file->dataDirty;
}

/**
 * @brief Test whether exFAT has released its exclusive allocation owner.
 * @return True for other filesystems or when exFAT owns no allocation operation.
 */
static AFATFS_INLINE bool afatfs_exfatIsIdle(void)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        if (afatfs.allocationOwner) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Decode a little-endian 32-bit GPT field without relying on host alignment.
 * @param buffer At least four bytes containing a little-endian field.
 * @return Decoded 32-bit unsigned field.
 */
static uint32_t afatfs_gptGet32(const uint8_t *buffer)
{
    return buffer[0] | (uint32_t)buffer[1] << 8 | (uint32_t)buffer[2] << 16 | (uint32_t)buffer[3] << 24;
}

/**
 * @brief Decode a little-endian 64-bit GPT field without relying on host alignment.
 * @param buffer At least eight bytes containing a little-endian field.
 * @return Decoded 64-bit unsigned field.
 */
static uint64_t afatfs_gptGet64(const uint8_t *buffer)
{
    return afatfs_gptGet32(buffer) | (uint64_t)afatfs_gptGet32(buffer + 4) << 32;
}

/**
 * @brief Test whether a GPT partition entry has a nonzero partition type identifier.
 * @param entry Directory record or output receiving a cached entry; ownership follows the cache contract.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_gptUsedEntry(const uint8_t *entry)
{
    return afatfs_gptGet64(entry) || afatfs_gptGet64(entry + 8);
}

/**
 * @brief Validate a GPT header and initialize the partition-array scan.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @return True when the condition or completion described above holds; false otherwise.
 */
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

/**
 * @brief Switch GPT discovery to the backup header after a primary-header failure.
 * @return True when the condition or completion described above holds; false otherwise.
 */
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
/**
 * @brief Advance GPT discovery by reading and validating partition-table sectors asynchronously.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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

/**
 * @brief Test whether a cluster number belongs to the mounted exFAT data heap.
 *
 * @details Bring the logical filesize up to date with the current cursor position.
 * @param cluster Data-cluster number to inspect or update.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_exfatValidCluster(uint32_t cluster)
{
    return cluster >= 2 && cluster - 2 < afatfs.numClusters;
}

// Walk a metadata stream without restarting the walk every time an asynchronous read yields.
/**
 * @brief Resolve a sector in an exFAT stream, following its FAT chain when required.
 * @param firstCluster First cluster of the stream or extent being inspected.
 * @param offset Relative byte offset or journal-sector offset, as described by this operation.
 * @param cursorCluster Cluster traversal cursor, updated as cached FAT links are consumed.
 * @param cursorIndex Entry traversal cursor within the current FAT page.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @param flags Flags controlling the requested cache or metadata operation.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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

/**
 * @brief Read or update an exFAT allocation-bitmap bit through the sector cache.
 * @param cluster Data-cluster number to inspect or update.
 * @param write True when this operation must modify the cached metadata.
 * @param allocated Allocation state to store in the exFAT bitmap.
 * @param value Integer value to encode, store or round.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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

/**
 * @brief Publish an exFAT directory entry set with the required checksum and ordering.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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

// 1 = match, 0 = another/invalid entry set, -1 = matching but unsupported metadata.
/**
 * @brief Validate and load an exFAT primary, stream and name entry set.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param name Filename, pathname component or label used by this operation.
 * @return One for a matching valid set, zero for a mismatch, or -1 for an unsupported entry/name.
 */
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
/**
 * @brief Advance exFAT lookup or creation and publish a valid entry set asynchronously.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
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
                createState->location.exfat.entriesRead = 0;
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
                    createState->location.exfat.entriesRead = 0;
                    createState->directoryEnd = false;
                    createState->location.exfat.entryCount =
                        (uint8_t)(2 + (strlen((char *)createState->fileName) + 14) / 15 +
                            createState->location.exfat.extraEntryCount);
                    if (createState->location.exfat.entryCount > AFATFS_EXFAT_MAX_ENTRIES) {
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
                    createState->location.exfat.entriesRead = 0;
                    file->exfatEntryCount = count <= AFATFS_EXFAT_MAX_ENTRIES && count >= 3 ? (uint8_t)count : 0;
                }
                if (file->exfatEntryCount) {
                    unsigned n = createState->location.exfat.entriesRead++;
                    memcpy(file->exfatEntries + n * FAT_DIRECTORY_ENTRY_SIZE, entry, FAT_DIRECTORY_ENTRY_SIZE);
                    file->exfatPositions[n] = file->directoryEntryPos;
                    if (createState->location.exfat.entriesRead == file->exfatEntryCount) {
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
                        createState->location.exfat.entriesRead = 0;
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
                    createState->location.exfat.entriesRead = 0;
                    break;
                }
                file->exfatPositions[createState->location.exfat.entriesRead++] = file->directoryEntryPos;
                if (createState->location.exfat.entriesRead != createState->location.exfat.entryCount) {
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
                file->exfatEntryCount = createState->location.exfat.entryCount;
                file->exfatWriteIndex = 0;
                memset(file->exfatEntries, 0, sizeof(file->exfatEntries));
                {
                    exFatFileEntry_t *primary = (exFatFileEntry_t *)file->exfatEntries;
                    exFatStreamEntry_t *stream = (exFatStreamEntry_t *)(file->exfatEntries + FAT_DIRECTORY_ENTRY_SIZE);
                    primary->type = EXFAT_DIR_FILEDIR;
                    primary->numOfEntries = createState->location.exfat.entryCount - 1;
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
#if ASYNCFS_ENABLE_FATFS_API
                    if (createState->location.exfat.extraEntryCount) {
                        afatfsFilePtr_t source = afatfs_request->file;
                        memcpy(file->exfatEntries +
                                (createState->location.exfat.entryCount - createState->location.exfat.extraEntryCount) *
                                    32,
                            source->exfatEntries +
                                (source->exfatEntryCount - createState->location.exfat.extraEntryCount) * 32,
                            createState->location.exfat.extraEntryCount * 32);
                    }
#endif
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

/**
 * @brief Resume close after a required empty-file truncation completes.
 *
 * @details Returns true if an operation was successfully queued to close the file and destroy the file handle. If
 * the file is
 * currently busy, false is returned and you should retry later.
 *
 * If provided, the callback will be called after the operation completes (pass NULL for no callback).
 *
 * If this function returns true, you should not make any further calls to the file (as the handle might be reused
 * for a
 * new file).
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @param user Application context passed back to the completion callback.
 */
static void afatfs_closeAfterEmptyTruncate(afatfsFilePtr_t file, void *user)
{
    (void)user;
    afatfsCallback_t callback = file->closeAfterTruncateCallback;
    void *callbackUser = file->closeAfterTruncateUser;
    file->closeAfterTruncateCallback = NULL;
    file->closeAfterTruncateUser = NULL;
    afatfs_fclose(file, callback, callbackUser);
}

/**
 * @brief Advance exFAT boot validation, allocation metadata and journal discovery.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 */
static afatfsOperationStatus_e afatfs_exfatMountContinue(void)
{
    uint8_t *sector;
    afatfsOperationStatus_e status;
    switch (afatfs.exfatMountPhase) {
        case 0: // Validate all eleven boot sectors and the repeated checksum sector.
            while (afatfs.initState.exfat.progress.boot.offset < 12) {
                status = afatfs_cacheSector(afatfs.partitionStartSector + afatfs.initState.exfat.progress.boot.offset,
                    &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE, 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (afatfs.initState.exfat.progress.boot.offset == 11) {
                    for (unsigned n = 0; n < AFATFS_SECTOR_SIZE; n += 4) {
                        uint32_t checksum = sector[n] | ((uint32_t)sector[n + 1] << 8) |
                            ((uint32_t)sector[n + 2] << 16) | ((uint32_t)sector[n + 3] << 24);
                        if (checksum != afatfs.initState.exfat.progress.boot.checksum) {
                            return AFATFS_OPERATION_FAILURE;
                        }
                    }
                } else {
                    if (afatfs.initState.exfat.progress.boot.offset >= 1 &&
                        afatfs.initState.exfat.progress.boot.offset <= 8 &&
                        (sector[508] != 0 || sector[509] != 0 || sector[510] != 0x55 || sector[511] != 0xAA)) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    for (unsigned n = 0; n < AFATFS_SECTOR_SIZE; n++) {
                        if (afatfs.initState.exfat.progress.boot.offset == 0 && (n == 106 || n == 107 || n == 112)) {
                            continue;
                        }
                        afatfs.initState.exfat.progress.boot.checksum =
                            (afatfs.initState.exfat.progress.boot.checksum >> 1 |
                                afatfs.initState.exfat.progress.boot.checksum << 31) +
                            sector[n];
                    }
                }
                ++afatfs.initState.exfat.progress.boot.offset;
            }
            afatfs_chdir(NULL);
            afatfs_findFirst(&afatfs.currentDirectory, &afatfs.initState.exfat.progress.directory.finder);
            afatfs.initState.exfat.progress.directory.entriesRead = 0;
            afatfs.exfatMountPhase = 1;
            return AFATFS_OPERATION_IN_PROGRESS;
        case 1: // Metadata entries may occur anywhere in a fragmented root directory.
            for (;;) {
                fatDirectoryEntry_t *entry;
                status = afatfs_findNext(
                    &afatfs.currentDirectory, &afatfs.initState.exfat.progress.directory.finder, &entry);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                if (!entry || entry->fileName[0] == 0) {
                    break;
                }
                if (++afatfs.initState.exfat.progress.directory.entriesRead >
                    (uint64_t)afatfs.numClusters * afatfs_clusterSize() / FAT_DIRECTORY_ENTRY_SIZE) {
                    return AFATFS_OPERATION_FAILURE;
                }
                uint8_t type = (uint8_t)entry->fileName[0];
                uint8_t *raw = (uint8_t *)entry;
                afatfsJournal_t *journal = &afatfs.journal;
                if (!journal->found) {
                    afatfsFile_t *fileHandle = &afatfs.openFiles[0];
                    if (type == EXFAT_DIR_FILEDIR) {
                        journal->mount.foundEntries = raw[1] == 2 ? 1 : 0;
                    } else if (journal->mount.foundEntries && journal->mount.foundEntries < 3) {
                        journal->mount.foundEntries++;
                    }
                    if (journal->mount.foundEntries) {
                        unsigned n = journal->mount.foundEntries - 1;
                        memcpy(fileHandle->exfatEntries + n * FAT_DIRECTORY_ENTRY_SIZE, raw, FAT_DIRECTORY_ENTRY_SIZE);
                        fileHandle->exfatPositions[n] = afatfs.initState.exfat.progress.directory.finder;
                        if (journal->mount.foundEntries == 3) {
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
                            journal->mount.foundEntries = 0;
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
                        if (afatfs.initState.exfat.upcaseCluster || metadata->dataSize == 0 ||
                            metadata->dataSize > 131072 || (metadata->dataSize & 1)) {
                            return AFATFS_OPERATION_FAILURE;
                        }
                        afatfs.initState.exfat.upcaseCluster = metadata->firstCluster;
                        afatfs.initState.exfat.upcaseLength = (uint32_t)metadata->dataSize;
                        afatfs.initState.exfat.upcaseChecksum = metadata->tableChecksum;
                    }
                }
                if (journal->found && afatfs.bitmapCluster && afatfs.initState.exfat.upcaseCluster) {
                    break;
                }
            }
            afatfs_findLast(&afatfs.currentDirectory);
            if (!afatfs.bitmapCluster || !afatfs.initState.exfat.upcaseCluster) {
                return AFATFS_OPERATION_FAILURE;
            }
            afatfs.initState.exfat.progress.upcase.offset = 0;
            afatfs.initState.exfat.progress.upcase.cluster = 0;
            afatfs.initState.exfat.progress.upcase.clusterIndex = 0;
            afatfs.initState.exfat.progress.upcase.checksum = 0;
            afatfs.initState.exfat.progress.upcase.codepoint = 0;
            afatfs.upcaseSkip = false;
            afatfs.exfatMountPhase = 2;
            return AFATFS_OPERATION_IN_PROGRESS;
        case 2: // Verify and decompress the on-volume up-case table, retaining only the ASCII API range.
            while (afatfs.initState.exfat.progress.upcase.offset < afatfs.initState.exfat.upcaseLength) {
                status = afatfs_exfatStreamSector(afatfs.initState.exfat.upcaseCluster,
                    afatfs.initState.exfat.progress.upcase.offset, &afatfs.initState.exfat.progress.upcase.cluster,
                    &afatfs.initState.exfat.progress.upcase.clusterIndex, &sector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                unsigned n = afatfs.initState.exfat.progress.upcase.offset % AFATFS_SECTOR_SIZE;
                uint16_t word = sector[n] | ((uint16_t)sector[n + 1] << 8);
                afatfs.initState.exfat.progress.upcase.checksum =
                    (afatfs.initState.exfat.progress.upcase.checksum >> 1 |
                        afatfs.initState.exfat.progress.upcase.checksum << 31) +
                    sector[n];
                afatfs.initState.exfat.progress.upcase.checksum =
                    (afatfs.initState.exfat.progress.upcase.checksum >> 1 |
                        afatfs.initState.exfat.progress.upcase.checksum << 31) +
                    sector[n + 1];
                afatfs.initState.exfat.progress.upcase.offset += 2;
                if (afatfs.upcaseSkip) {
                    if (!word || afatfs.initState.exfat.progress.upcase.codepoint + word > 65536) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    uint32_t end = afatfs.initState.exfat.progress.upcase.codepoint + word;
                    for (; afatfs.initState.exfat.progress.upcase.codepoint < end &&
                        afatfs.initState.exfat.progress.upcase.codepoint < 128;
                        afatfs.initState.exfat.progress.upcase.codepoint++) {
                        afatfs.upcaseAscii[afatfs.initState.exfat.progress.upcase.codepoint] =
                            (uint16_t)afatfs.initState.exfat.progress.upcase.codepoint;
                    }
                    afatfs.initState.exfat.progress.upcase.codepoint = end;
                    afatfs.upcaseSkip = false;
                } else if (word == 0xFFFF && afatfs.initState.exfat.progress.upcase.codepoint != 0xFFFF) {
                    afatfs.upcaseSkip = true;
                } else {
                    if (afatfs.initState.exfat.progress.upcase.codepoint >= 65536) {
                        return AFATFS_OPERATION_FAILURE;
                    }
                    if (afatfs.initState.exfat.progress.upcase.codepoint < 128) {
                        afatfs.upcaseAscii[afatfs.initState.exfat.progress.upcase.codepoint] = word;
                    }
                    ++afatfs.initState.exfat.progress.upcase.codepoint;
                }
            }
            if (afatfs.upcaseSkip || afatfs.initState.exfat.progress.upcase.codepoint != 65536 ||
                afatfs.initState.exfat.progress.upcase.checksum != afatfs.initState.exfat.upcaseChecksum) {
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

/* ==================== Persistence journal ==================== */

#if defined(ASYNCFS_ENABLE_EXFAT) || defined(AFATFS_ENABLE_FAT_JOURNAL)
/**
 * @brief Forget undo capture when a cache descriptor is reassigned.
 * @param descriptor Cache slot being assigned to a different sector.
 */
static AFATFS_INLINE void afatfs_journalResetCacheEpoch(afatfsCacheBlockDescriptor_t *descriptor)
{
    descriptor->journalEpoch = 0;
}

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

/**
 * @brief Extend a reflected CRC-32 calculation with another byte range.
 * @param crc CRC accumulator before the supplied bytes are processed.
 * @param bytes Byte range containing the serialized data to decode, checksum or update.
 * @param length Number of input bytes included in the checksum calculation.
 * @return Updated reflected CRC-32 accumulator.
 */
static uint32_t afatfs_crc32Update(uint32_t crc, const uint8_t *bytes, unsigned length)
{
    while (length--) {
        crc = (crc >> 8) ^ afatfs_crc32Table[(crc ^ *bytes++) & 0xFF];
    }
    return crc;
}
#endif

#ifdef AFATFS_ENABLE_JOURNAL
/**
 * @brief Test whether journal protection permits buffering this file's allocation metadata.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_allocationMetadataBuffered(const afatfsFile_t *file)
{
    // Undo records are durable before either FAT copy can be modified.
    // Publication, sync/close and the journal checkpoint still drain the cache.
    return afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.ready && !afatfs.readOnly &&
        file->type == AFATFS_FILE_TYPE_NORMAL;
}


/**
 * @brief Test every journal ownership and I/O condition required for global idle.
 * @return True only when no journal work or ownership blocks quiescence.
 */
static AFATFS_INLINE bool afatfs_journalIsIdle(void)
{
    {
        const afatfsJournal_t *state = &afatfs.journal;
        if (state->ioPending || state->ioDone || state->ioFailed || state->active || state->provision ||
            state->headerPrepared || state->blockAllocation || state->phase != AFATFS_JOURNAL_IDLE) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Select clean-shutdown progress after mount without resetting active capture.
 */
static AFATFS_INLINE void afatfs_journalFinishMount(void)
{
    // Mount and shutdown share this byte. Capture may already be active
    // for freefile/log initialization, so leave its working state intact.
    afatfs.journal.lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_WAIT_COMMIT;
}

/**
 * @brief Advance filesystem-specific clean markers and release durable journal ownership.
 * @return True when clean markers and journal ownership are durable; false to retry after polling.
 */
static AFATFS_INLINE bool afatfs_journalShutdown(void)
{
#ifdef AFATFS_ENABLE_FAT_JOURNAL
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && !afatfs_fatJournalShutdown()) {
        return false;
    }
#endif

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
    return true;
}

/**
 * @brief Identify directory publication phases that still need a reusable cache page.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_directoryNeedsCache(const afatfsFile_t *file)
{
    return file->operation.active == AFATFS_FILE_OPERATION_CREATE_FILE &&
        (file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB ||
            file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_WRITE_FILE_ENTRY);
}

/**
 * @brief Test whether a file operation is currently producing ordered metadata changes.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_metadataProducerActive(const afatfsFile_t *file)
{
    switch (file->operation.active) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            return file->operation.state.createFile.phase >= AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB &&
                file->operation.state.createFile.phase < AFATFS_CREATEFILE_PHASE_SUCCESS;
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            if (afatfs_allocationMetadataBuffered(file)) {
                return file->operation.state.appendFreeCluster.phase >
                    AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE &&
                    file->operation.state.appendFreeCluster.phase <
                    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
            }
            return file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_CONVERT_CHAIN;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER:
            return file->operation.state.appendSupercluster.phase == AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT;
#endif
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            return false; // Directory initialization keeps its own durability barriers.
        case AFATFS_FILE_OPERATION_TRUNCATE:
        case AFATFS_FILE_OPERATION_UNLINK:
            return file->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL;
        default:
            return false;
    }
}

/**
 * @brief Decide whether a metadata producer may temporarily defer background flushing.
 * @return True when the condition or completion described above holds; false otherwise.
 */
static bool afatfs_deferBackgroundFlush(void)
{
    if (!afatfs.readOnly && afatfs.fileSystemState == AFATFS_FILESYSTEM_STATE_INITIALIZATION &&
        afatfs.journal.provision &&
        afatfs.journal.lifecycle.initPhase == AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT) {
        return true;
    }
    if (!afatfs.journal.ready || afatfs.readOnly || afatfs.journal.blockAllocation) {
        return false;
    }
    // A directory walk may need both a FAT sector and its target sector on
    // every retry. With only one reusable page they can evict each other
    // forever without an allocation failure ever triggering a forced flush.
    unsigned reusable = 0;
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (!entry->locked && !entry->retainCount &&
            (entry->state == AFATFS_CACHE_STATE_EMPTY || entry->state == AFATFS_CACHE_STATE_IN_SYNC)) {
            reusable++;
        }
    }
    if (reusable < 2) {
        for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
            const afatfsFile_t *file = &afatfs.openFiles[index];
            if (afatfs_directoryNeedsCache(file)) {
                return false;
            }
        }
#ifdef AFATFS_USE_FREEFILE
        if (afatfs_directoryNeedsCache(&afatfs.freeFile)) {
            return false;
        }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
        if (afatfs_directoryNeedsCache(&afatfs.introSpecLog)) {
            return false;
        }
#endif
    }
    if (afatfs_metadataProducerActive(&afatfs.currentDirectory)) {
        return true;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (afatfs_metadataProducerActive(&afatfs.openFiles[index])) {
            return true;
        }
    }
#ifdef AFATFS_USE_FREEFILE
    if (afatfs_metadataProducerActive(&afatfs.freeFile)) {
        return true;
    }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (afatfs_metadataProducerActive(&afatfs.introSpecLog)) {
        return true;
    }
#endif
    return false;
}

/* ===== Optional journal implementation; guarded by the enclosing feature condition. ===== */
/* Shared FAT32/exFAT metadata undo engine. Filesystem adapters only handle
 * discovery, extent allocation and dirty markers. Disk formats remain distinct.
 * The retained, unmodified cache entry supplies the before-image: one aligned
 * sector buffer handles I/O, records and alternating headers, without a heap. */
/**
 * @brief Select the filesystem-specific journal header or record signature.
 * @param record True to select the undo-record signature; false to select the journal-header signature.
 * @return Pointer to the selected static eight-byte journal signature.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static const char *afatfs_jmagic(bool record)
{
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT) {
        return record ? "AFUNDO02" : "AFJNL002";
    }
#endif
    return record ? "AFFUNDO1" : "AFJFT001";
}
/**
 * @brief Decode an unaligned little-endian 32-bit journal field.
 * @param bytes Byte range containing the serialized data to decode, checksum or update.
 * @return Decoded 32-bit unsigned journal field.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static uint32_t afatfs_jget(const uint8_t *bytes)
{
    return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}
/**
 * @brief Encode a little-endian 32-bit journal field.
 * @param bytes Byte range containing the serialized data to decode, checksum or update.
 * @param value Integer value to encode, store or round.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static void afatfs_jput(uint8_t *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}
/**
 * @brief Calculate the checksum protecting a journal header or record byte range.
 * @param bytes Byte range containing the serialized data to decode, checksum or update.
 * @param length Number of input bytes included in the checksum calculation.
 * @return CRC-32 of the supplied journal byte range.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static uint32_t afatfs_jcrc(const uint8_t *bytes, unsigned length)
{
    return ~afatfs_crc32Update(UINT32_MAX, bytes, length);
}
/**
 * @brief Latch an unresolved journal error and prevent unsafe writable progress.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static void afatfs_jfail(void)
{
    afatfs.journal.phase = AFATFS_JOURNAL_ERROR;
    afatfs.readOnly = true;
    afatfs.lastError = AFATFS_ERROR_RECOVERY_REQUIRED;
}
/**
 * @brief Translate a journal-relative sector to its physical extent.
 * @param offset Sector offset within the validated journal extent.
 * @return Physical sector corresponding to the journal-relative offset.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static uint32_t afatfs_jsector(uint32_t offset)
{
    return afatfs_fileClusterToPhysical(afatfs.journal.firstCluster, 0) + offset;
}
/**
 * @brief Complete a journal transfer only when its buffer and media generation still match.
 * @param operation Operation code selecting the requested action or completed backend transfer.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @param buffer Original journal scratch buffer, or NULL on backend failure.
 * @param generation Media-generation identifier supplied when the transfer started.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static void afatfs_jcomplete(sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    (void)operation;
    (void)sector;
    if (generation != afatfs_mediaGeneration || !afatfs_mediaSessionActive()) {
        return;
    }
    afatfs.journal.ioPending = false;
    afatfs.journal.ioDone = true;
    afatfs.journal.ioFailed = buffer == NULL;
}
/**
 * @brief Start or wait for one journal transfer using the journal's existing aligned scratch sector.
 * @param writing True to acquire or submit writable data; false to read existing data.
 * @param sector Physical sector, generated sector or sector-buffer output used by this operation.
 * @return True only when this transfer has completed successfully; false while busy, retrying or failed.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jio(bool writing, uint32_t sector)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (state->ioDone) {
        state->ioDone = false;
        if (state->ioFailed) {
            state->ioFailed = false;
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            return false;
        }
        return true;
    }
    if (state->ioPending) {
        return false;
    }
    if (!writing) {
        state->ioPending = sdcard_readBlock(sector, state->io, afatfs_jcomplete, afatfs_mediaGeneration);
    } else {
        sdcardOperationStatus_e status = sdcard_writeBlock(sector, state->io, afatfs_jcomplete, afatfs_mediaGeneration);
        if (status == SDCARD_OPERATION_SUCCESS) {
            return true;
        }
        state->ioPending = status == SDCARD_OPERATION_IN_PROGRESS;
        if (status == SDCARD_OPERATION_FAILURE) {
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
        }
    }
    return false;
}
/**
 * @brief Validate the journal header signature, layout and checksum.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jvalidHeader(void)
{
    const afatfsJournal_t *state = &afatfs.journal;
    const uint8_t *bytes = state->io;
    return !memcmp(bytes, afatfs_jmagic(false), 8) && afatfs_jget(bytes + 508) == afatfs_jcrc(bytes, 508) &&
        afatfs_jget(bytes + 28) == state->serial && afatfs_jget(bytes + 32) == state->firstCluster &&
        afatfs_jget(bytes + 36) == state->sectors && afatfs_jget(bytes + 40) == state->capacity &&
        afatfs_jget(bytes + 44) == afatfs.partitionStartSector && afatfs_jget(bytes + 48) == afatfs.fatStartSector &&
        afatfs_jget(bytes + 52) == afatfs.clusterStartSector && afatfs_jget(bytes + 56) == afatfs.sectorsPerCluster &&
        afatfs_jget(bytes + 60) == afatfs.numClusters && afatfs_jget(bytes + 16) <= state->capacity &&
        afatfs_jget(bytes + 20) <= 1 && afatfs_jget(bytes + 24) <= 1 &&
        (afatfs_jget(bytes + 20) || !afatfs_jget(bytes + 16));
}
/**
 * @brief Prepare and submit the next alternating journal header.
 * @return True once the new header write is acknowledged; false while transfer progress is pending.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jwriteHeader(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (!state->headerPrepared) {
        memset(state->io, 0, 512);
        memcpy(state->io, afatfs_jmagic(false), 8);
        afatfs_jput(state->io + 8, ++state->sequence);
        afatfs_jput(state->io + 12, state->epoch);
        afatfs_jput(state->io + 16, state->count);
        afatfs_jput(state->io + 20, state->active);
        afatfs_jput(state->io + 24, state->owned);
        afatfs_jput(state->io + 28, state->serial);
        afatfs_jput(state->io + 32, state->firstCluster);
        afatfs_jput(state->io + 36, state->sectors);
        afatfs_jput(state->io + 40, state->capacity);
        afatfs_jput(state->io + 44, afatfs.partitionStartSector);
        afatfs_jput(state->io + 48, afatfs.fatStartSector);
        afatfs_jput(state->io + 52, afatfs.clusterStartSector);
        afatfs_jput(state->io + 56, afatfs.sectorsPerCluster);
        afatfs_jput(state->io + 60, afatfs.numClusters);
        afatfs_jput(state->io + 508, afatfs_jcrc(state->io, 508));
        state->headerPrepared = true;
    }
    if (!afatfs_jio(true, afatfs_jsector(state->headerSlot ^ 1))) {
        return false;
    }
    state->headerSlot ^= 1;
    state->headerPrepared = false;
    return true;
}
/**
 * @brief Validate an undo record's signature, epoch, checksum and target-sector range.
 * @return True when the record in the scratch sector is valid and targets permitted non-journal metadata.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jrecord(void)
{
    const afatfsJournal_t *state = &afatfs.journal;
    uint32_t target = afatfs_jget(state->io + 12);
    return !memcmp(state->io, afatfs_jmagic(true), 8) && afatfs_jget(state->io + 8) == state->epoch &&
        afatfs_jget(state->io + 508) == afatfs_jcrc(state->io, 508) && target >= afatfs.fatStartSector &&
        (uint64_t)target < (uint64_t)afatfs.partitionStartSector + afatfs.partitionSectors &&
        !(target >= afatfs_jsector(0) && target - afatfs_jsector(0) < state->sectors);
}
/**
 * @brief Test whether one file operation permits a shared journal checkpoint.
 * @param file File handle whose cursor, metadata or pending operation is accessed.
 * @return True when this file operation permits a shared checkpoint, even if the handle remains open.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jfileQuiescent(const afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE || file->operation.active == AFATFS_FILE_OPERATION_NONE) {
        return true;
    }
    switch (file->operation.active) {
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_LOCKED:
            // A freefile reservation has no metadata work of its own. The owning
            // file remains responsible for completing its allocation and close.
            return true;
#endif
        case AFATFS_FILE_OPERATION_CLOSE:
            return file->operation.state.closeFile.phase >= 3;
        case AFATFS_FILE_OPERATION_SYNC:
            return file->operation.state.syncFile.phase >= 3;
        case AFATFS_FILE_OPERATION_UNLINK:
        case AFATFS_FILE_OPERATION_TRUNCATE:
            return file->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_SUCCESS ||
                (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.journal.blockAllocation &&
                    file->operation.state.truncateFile.phase == AFATFS_TRUNCATE_FILE_INITIAL);
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            return afatfs.journal.blockAllocation &&
                file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
        case AFATFS_FILE_OPERATION_CREATE_FILE:
#ifdef AFATFS_USE_FREEFILE
            if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) &&
                file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_SUCCESS &&
                afatfs.freeFile.operation.active == AFATFS_FILE_OPERATION_LOCKED) {
                return true; // Waiting for the owner's close to release the reservation.
            }
#endif
            return afatfs.journal.blockAllocation &&
                (file->operation.state.createFile.phase == AFATFS_CREATEFILE_PHASE_INITIAL ||
                    (file->operation.state.createFile.phase ==
                            (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT
                                    ? AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE
                                    : AFATFS_CREATEFILE_PHASE_WRITE_FILE_ATTRIB) &&
                        afatfs.currentDirectory.operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY));
        default:
            return false;
    }
}
/**
 * @brief Test whether all relevant files permit the journal's current checkpoint boundary.
 * @return True when file and cache state permits the shared checkpoint boundary.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_jquiescent(void)
{
#if ASYNCFS_ENABLE_FATFS_API
    if (afatfs_request && !afatfs_request->checkpoint) {
        return false;
    }
#endif
    if (afatfs.cacheFlushInProgress || (afatfs.cwdWalkMutex && !afatfs.journal.blockAllocation)) {
        return false;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_EXFAT && afatfs.allocationOwner) {
        return false;
    }
#endif
    for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state == AFATFS_CACHE_STATE_WRITING ||
            (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked)) {
            return false;
        }
    }
    if (!afatfs_jfileQuiescent(&afatfs.currentDirectory)) {
        return false;
    }
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        if (!afatfs_jfileQuiescent(&afatfs.openFiles[index])) {
            return false;
        }
    }
#ifdef AFATFS_USE_FREEFILE
    if (!afatfs_jfileQuiescent(&afatfs.freeFile)) {
        return false;
    }
#endif
#ifdef AFATFS_USE_INTROSPECTIVE_LOGGING
    if (!afatfs_jfileQuiescent(&afatfs.introSpecLog)) {
        return false;
    }
#endif
    return true;
}
/**
 * @brief Test whether journal work has crossed the required durable commit boundary.
 * @return True when no journal is active or the journal has reached its idle committed boundary.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_journalCommitted(void)
{
    return !afatfs.journal.ready || (!afatfs.journal.active && afatfs.journal.phase == AFATFS_JOURNAL_IDLE);
}
/**
 * @brief Report a journal error that prevents further safe writable progress.
 * @return True when the condition or completion described above holds; false otherwise.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static bool afatfs_journalUnresolvedError(void)
{
    return afatfs.journal.ready && afatfs.journal.phase == AFATFS_JOURNAL_ERROR;
}
// Select alternating headers using scalar state, avoiding a second sector buffer.
/**
 * @brief Select the validated alternating header used to resume journal processing.
 * @param slot Alternating journal-header slot selected for processing.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static void afatfs_jselectHeader(uint32_t slot)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (afatfs_jvalidHeader() && (!state->validHeader || (int32_t)(afatfs_jget(state->io + 8) - state->sequence) > 0)) {
        state->sequence = afatfs_jget(state->io + 8);
        state->epoch = afatfs_jget(state->io + 12);
        state->count = afatfs_jget(state->io + 16);
        state->active = afatfs_jget(state->io + 20) != 0;
        state->owned = afatfs_jget(state->io + 24) != 0;
        state->headerSlot = slot;
        state->validHeader = true;
    }
}
/**
 * @brief Capture and durably log a metadata page's before-image before allowing modification.
 * @param index Descriptor index of the metadata page whose before-image must be durable.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static afatfsOperationStatus_e afatfs_journalBeforeWrite(int index)
{
    afatfsJournal_t *state = &afatfs.journal;
    afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
    if (!state->ready) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (state->phase == AFATFS_JOURNAL_ERROR) {
        return AFATFS_OPERATION_FAILURE;
    }
    if (state->active && entry->journalEpoch == state->epoch) {
        return AFATFS_OPERATION_SUCCESS;
    }
    if (state->phase != AFATFS_JOURNAL_IDLE || entry->state == AFATFS_CACHE_STATE_WRITING) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }
    if (!state->active) {
        if (++state->epoch == 0) {
            afatfs_jfail();
            return AFATFS_OPERATION_FAILURE;
        }
        state->active = true;
        state->count = 0;
        state->phase = AFATFS_JOURNAL_START;
    } else {
        state->phase = AFATFS_JOURNAL_INDEX_READ;
    }
    state->work.capture.pendingCache = index;
    state->work.capture.pendingSector = entry->sectorIndex;
    state->work.capture.record.scan = 0;
    entry->retainCount++;
    return AFATFS_OPERATION_IN_PROGRESS;
}
/**
 * @brief Advance capture, commit and checkpoint writes while honoring backend acknowledgements.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static void afatfs_journalPoll(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    if (!state->ready || state->ioPending) {
        return;
    }
    switch (state->phase) {
        case AFATFS_JOURNAL_IDLE:
            if (state->active && afatfs_jquiescent()) {
                state->active = false;
                state->count = 0;
                state->phase = AFATFS_JOURNAL_COMMIT;
            }
            break;
        case AFATFS_JOURNAL_START:
            if (afatfs_jwriteHeader()) {
                state->phase = AFATFS_JOURNAL_INDEX_READ;
            }
            break;
        case AFATFS_JOURNAL_INDEX_READ:
            if (state->work.capture.record.scan == state->count) {
                if (state->count == state->capacity) {
                    afatfs_jfail();
                    break;
                }
                memcpy(state->io, afatfs_cacheSectorGetMemory(state->work.capture.pendingCache), 512);
                state->work.capture.record.imageCRC = afatfs_jcrc(state->io, 512);
                state->phase = AFATFS_JOURNAL_IMAGE_WRITE;
            } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * state->work.capture.record.scan))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                if (afatfs_jget(state->io + 12) == state->work.capture.pendingSector) {
                    state->phase = AFATFS_JOURNAL_CAPTURE_DONE;
                } else {
                    state->work.capture.record.scan++;
                }
            }
            break;
        case AFATFS_JOURNAL_IMAGE_WRITE:
            if (afatfs_jio(true, afatfs_jsector(3 + 2 * state->count))) {
                memset(state->io, 0, 512);
                memcpy(state->io, afatfs_jmagic(true), 8);
                afatfs_jput(state->io + 8, state->epoch);
                afatfs_jput(state->io + 12, state->work.capture.pendingSector);
                afatfs_jput(state->io + 16, state->work.capture.record.imageCRC);
                afatfs_jput(state->io + 508, afatfs_jcrc(state->io, 508));
                state->phase = AFATFS_JOURNAL_RECORD_WRITE;
            }
            break;
        case AFATFS_JOURNAL_RECORD_WRITE:
            if (afatfs_jio(true, afatfs_jsector(2 + 2 * state->count))) {
                state->count++;
                state->phase = AFATFS_JOURNAL_RECORD_HEADER;
            }
            break;
        case AFATFS_JOURNAL_RECORD_HEADER:
            if (afatfs_jwriteHeader()) {
                state->phase = AFATFS_JOURNAL_CAPTURE_DONE;
            }
            break;
        case AFATFS_JOURNAL_CAPTURE_DONE:
            afatfs.cacheDescriptor[state->work.capture.pendingCache].journalEpoch = state->epoch;
            afatfs.cacheDescriptor[state->work.capture.pendingCache].retainCount--;
            state->work.capture.pendingCache = -1;
            state->phase = AFATFS_JOURNAL_IDLE;
            break;
        case AFATFS_JOURNAL_COMMIT:
            if (afatfs_jwriteHeader()) {
                state->blockAllocation = false;
                state->phase = AFATFS_JOURNAL_IDLE;
            }
            break;
        default:
            break;
    }
}

// Validate every published image before the first undo write. Keep the active
// header unchanged throughout replay so another power loss can repeat it safely.
/**
 * @brief Replay validated undo records and restore the journal to a committed boundary.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when AFATFS_ENABLE_JOURNAL is enabled.
 */
static afatfsOperationStatus_e afatfs_journalRecover(void)
{
    afatfsJournal_t *state = &afatfs.journal;
    switch (state->mount.recoveryPhase) {
        case AFATFS_JOURNAL_RECOVERY_PREPARE:
            state->work.recovery.progress = state->active ? state->count : 0;
            state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_VALIDATE_RECORD;
            break;
        case AFATFS_JOURNAL_RECOVERY_VALIDATE_RECORD:
            if (!state->work.recovery.progress) {
                state->work.recovery.progress = state->active ? state->count : 0;
                state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_REPLAY_READ_RECORD;
            } else if (afatfs_jio(false, afatfs_jsector(2 + 2 * (state->work.recovery.progress - 1)))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                state->work.recovery.imageCRC = afatfs_jget(state->io + 16);
                state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_VALIDATE_IMAGE;
            }
            break;
        case AFATFS_JOURNAL_RECOVERY_VALIDATE_IMAGE:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (state->work.recovery.progress - 1)))) {
                if (afatfs_jcrc(state->io, 512) != state->work.recovery.imageCRC) {
                    afatfs_jfail();
                } else {
                    state->work.recovery.progress--;
                    state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_VALIDATE_RECORD;
                }
            }
            break;
        case AFATFS_JOURNAL_RECOVERY_REPLAY_READ_RECORD:
            if (!state->work.recovery.progress) {
                state->active = false;
                state->count = 0;
                return AFATFS_OPERATION_SUCCESS;
            }
            if (afatfs_jio(false, afatfs_jsector(2 + 2 * (state->work.recovery.progress - 1)))) {
                if (!afatfs_jrecord()) {
                    afatfs_jfail();
                    break;
                }
                state->work.recovery.target = afatfs_jget(state->io + 12);
                state->work.recovery.imageCRC = afatfs_jget(state->io + 16);
                state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_REPLAY_READ_IMAGE;
            }
            break;
        case AFATFS_JOURNAL_RECOVERY_REPLAY_READ_IMAGE:
            if (afatfs_jio(false, afatfs_jsector(3 + 2 * (state->work.recovery.progress - 1)))) {
                if (afatfs_jcrc(state->io, 512) != state->work.recovery.imageCRC) {
                    afatfs_jfail();
                } else {
                    state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_REPLAY_WRITE_IMAGE;
                }
            }
            break;
        case AFATFS_JOURNAL_RECOVERY_REPLAY_WRITE_IMAGE:
            if (afatfs_jio(true, state->work.recovery.target)) {
                state->work.recovery.progress--;
                state->mount.recoveryPhase = AFATFS_JOURNAL_RECOVERY_REPLAY_READ_RECORD;
            }
            break;
        default:
            afatfs_jfail();
            break;
    }
    return state->phase == AFATFS_JOURNAL_ERROR ? AFATFS_OPERATION_FAILURE : AFATFS_OPERATION_IN_PROGRESS;
}
#endif

#ifdef AFATFS_ENABLE_FAT_JOURNAL
/**
 * @brief Reject ordinary FAT32 writes that target the owned journal extent.
 * @param physicalSectorIndex Physical sector requested by the cache.
 * @param sectorFlags Cache access flags, including metadata and write intent.
 * @return False after recording denied journal writes; true for other sector requests.
 */
static AFATFS_INLINE bool afatfs_fatJournalAllowsSector(LBA_t physicalSectorIndex, uint8_t sectorFlags)
{
    if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.journal.ready &&
        (sectorFlags & AFATFS_CACHE_WRITE)) {
        uint32_t journalStart = afatfs_fileClusterToPhysical(afatfs.journal.firstCluster, 0);
        if (physicalSectorIndex >= journalStart && physicalSectorIndex - journalStart < afatfs.journal.sectors) {
            afatfs.lastError = AFATFS_ERROR_DENIED;
            return false;
        }
    }
    return true;
}

/* ===== Optional fatjournal implementation; guarded by the enclosing feature condition. ===== */
/* FAT32 journal discovery, FAT-chain provisioning and clean markers. */
/**
 * @brief Update and mirror the FAT32 clean-shutdown marker.
 * @param dirty True to publish the dirty state; false to publish the clean-shutdown state.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when AFATFS_ENABLE_FAT_JOURNAL is enabled.
 */
static afatfsOperationStatus_e afatfs_fjDirty(bool dirty)
{
    afatfsFATSector_t primary;
    uint8_t *mirror;
    afatfsOperationStatus_e status =
        afatfs_FATWriteSector(
            0, &primary, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, AFATFS_FAT_MIRROR_STATE(NULL) 0);
    if (status == AFATFS_OPERATION_SUCCESS) {
        primary.fat32[1] = dirty ? primary.fat32[1] & ~0x08000000U : primary.fat32[1] | 0x08000000U;
        memcpy(mirror, primary.bytes, 512);
    }
    return status;
}

/**
 * @brief Advance FAT32 journal discovery, provisioning and recovery.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when AFATFS_ENABLE_FAT_JOURNAL is enabled.
 */
static afatfsOperationStatus_e afatfs_fatJournalMount(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsFilePtr_t file = &afatfs.openFiles[0];
    uint8_t *buffer;
    uint32_t value;
    afatfsOperationStatus_e status;
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        afatfs_findLast(&afatfs.currentDirectory);
        afatfs_initFileHandle(file);
        afatfs_chdir(NULL);
        return AFATFS_OPERATION_SUCCESS;
    }
    switch (journalState->lifecycle.initPhase) {
        case AFATFS_JOURNAL_INIT_PREPARE:
            status = afatfs_cacheSector(afatfs.partitionStartSector, &buffer, AFATFS_CACHE_READ, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->serial = ((fatVolumeID_t *)buffer)->fat.fatDescriptor.fat32.volumeID;
            uint64_t capacity = (uint64_t)2 * afatfs.fatSectors + 64;
            uint64_t sectors =
                (2 + 2 * capacity + afatfs.sectorsPerCluster - 1) / afatfs.sectorsPerCluster * afatfs.sectorsPerCluster;
            if (sectors * 512 > UINT32_MAX || sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            journalState->capacity = (uint32_t)capacity;
            journalState->sectors = (uint32_t)sectors;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS;
            break;
        case AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS:
        case AFATFS_JOURNAL_INIT_FAT_READ_MIRROR_STATUS:
            status = afatfs_FATGetNextCluster(
                journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS ? 0 : 1, 1, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->startupDirty |= !(value & 0x08000000U);
            if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_READ_MIRROR_STATUS;
            } else {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_FIND_FILE;
                afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
            }
            break;
        case AFATFS_JOURNAL_INIT_FAT_FIND_FILE: {
            fatDirectoryEntry_t *entry;
            status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (entry && !fat_isDirectoryEntryEmpty(entry) && !memcmp(entry->fileName, "AFATFS  JNL", 11)) {
                afatfs_fileLoadDirectoryEntry(file, entry);
                if (file->type != AFATFS_FILE_TYPE_NORMAL || file->logicalSize != journalState->sectors * 512 ||
                    (file->attrib & 7) != 7 || file->firstCluster < 2 ||
                    file->firstCluster - 2 + journalState->sectors / afatfs.sectorsPerCluster > afatfs.numClusters) {
                    afatfs_jfail();
                    break;
                }
                journalState->firstCluster = file->firstCluster;
                journalState->found = true;
                afatfs_findLast(&afatfs.currentDirectory);
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN;
            } else if (!entry || fat_isDirectoryEntryTerminator(entry)) {
                afatfs_findLast(&afatfs.currentDirectory);
                if (journalState->startupDirty) {
                    afatfs_jfail();
                } else {
                    journalState->provision = true;
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY;
                }
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN:
        case AFATFS_JOURNAL_INIT_FAT_VALIDATE_MIRROR_CHAIN:
            status = afatfs_FATGetNextCluster(
                journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN ? 0 : 1,
                journalState->firstCluster + journalState->work.setup.progress, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            if (journalState->work.setup.progress + 1 == journalState->sectors / afatfs.sectorsPerCluster
                    ? !afatfs_FATIsEndOfChainMarker(value)
                    : value != journalState->firstCluster + journalState->work.setup.progress + 1) {
                afatfs_jfail();
                break;
            }
            if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_MIRROR_CHAIN;
            } else {
                journalState->work.setup.progress++;
                if (journalState->work.setup.progress == journalState->sectors / afatfs.sectorsPerCluster) {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER;
                } else {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER:
        case AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER:
            if (afatfs_jio(false,
                    afatfs_jsector(
                        journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1))) {
                afatfs_jselectHeader(
                    journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1);
                if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER) {
                    if (!journalState->validHeader || (journalState->startupDirty && !journalState->owned) ||
                        (!journalState->startupDirty && journalState->active)) {
                        afatfs_jfail();
                    } else {
                        journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVER;
                    }
                } else {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVER:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT;
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT:
            if (afatfs_jwriteHeader()) {
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_MARK_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_MARK_DIRTY:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FLUSH_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_FLUSH_DIRTY:
            if (afatfs_flushCache()) {
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP;
            }
            break;
        case AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP:
            if (afatfs_jwriteHeader()) {
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY:
            if (afatfs_fjDirty(true) == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(
                file, AFATFS_JOURNAL_NAME, 7, AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(file);
            if (file->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (file->type != AFATFS_FILE_TYPE_NORMAL || file->firstCluster) {
                afatfs_jfail();
                break;
            }
            journalState->work.setup.candidate = 2;
            journalState->work.setup.runLength = 0;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT:
            if (journalState->work.setup.candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_FATGetNextCluster(0, journalState->work.setup.candidate, &value);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.runLength = value ? 0 : journalState->work.setup.runLength + 1;
            journalState->work.setup.candidate++;
            if (journalState->work.setup.runLength == journalState->sectors / afatfs.sectorsPerCluster) {
                journalState->firstCluster = journalState->work.setup.candidate - journalState->work.setup.runLength;
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors) {
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT;
            } else {
                if (!journalState->ioPending && !journalState->ioDone) {
                    memset(journalState->io, 0, 512);
                }
                if (afatfs_jio(true, afatfs_jsector(journalState->work.setup.progress))) {
                    journalState->work.setup.progress++;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors / afatfs.sectorsPerCluster) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION;
            } else {
                value = journalState->firstCluster + journalState->work.setup.progress;
                status = afatfs_FATSetNextCluster(value,
                    journalState->work.setup.progress + 1 == journalState->sectors / afatfs.sectorsPerCluster
                        ? UINT32_MAX
                        : value + 1);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                journalState->work.setup.progress++;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION:
            if (afatfs_flushCache()) {
                file->firstCluster = journalState->firstCluster;
                file->physicalSize = journalState->sectors * 512;
                file->logicalSize = file->physicalSize;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE:
            status = afatfs_saveDirectoryEntry(file, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE:
            if (afatfs_flushCache()) {
                journalState->epoch = 1;
                journalState->owned = true;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER:
            if (afatfs_jwriteHeader()) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER:
            if (afatfs_jwriteHeader()) {
                afatfs_initFileHandle(file);
                afatfs_chdir(NULL);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        default:
            afatfs_jfail();
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * @brief Drain the FAT32 journal and publish its clean shutdown state.
 * @return True when the journal and clean markers have completed their shutdown barriers.
 *
 * @note Available when AFATFS_ENABLE_FAT_JOURNAL is enabled.
 */
static bool afatfs_fatJournalShutdown(void)
{
    afatfsJournal_t *journalState = &afatfs.journal;
    if (afatfs.readOnly || !journalState->firstCluster) {
        return true;
    }
    switch (journalState->lifecycle.shutdownPhase) {
        case AFATFS_JOURNAL_SHUTDOWN_WAIT_COMMIT:
            if (!afatfs_journalCommitted()) {
                return false;
            }
            journalState->ready = false;
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_MARK_CLEAN;
            // Fall through
        case AFATFS_JOURNAL_SHUTDOWN_MARK_CLEAN:
            if (afatfs_fjDirty(false) != AFATFS_OPERATION_SUCCESS) {
                return false;
            }
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_FLUSH_CLEAN;
            return false;
        case AFATFS_JOURNAL_SHUTDOWN_FLUSH_CLEAN:
            if (!afatfs_flushCache()) {
                return false;
            }
            journalState->owned = false;
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_RELEASE_OWNERSHIP;
            // Fall through
        case AFATFS_JOURNAL_SHUTDOWN_RELEASE_OWNERSHIP:
            if (!afatfs_jwriteHeader()) {
                return false;
            }
            journalState->lifecycle.shutdownPhase = AFATFS_JOURNAL_SHUTDOWN_COMPLETE;
            return true;
        case AFATFS_JOURNAL_SHUTDOWN_COMPLETE:
        default:
            return true;
    }
}
#endif

#ifdef ASYNCFS_ENABLE_EXFAT
/* ===== Optional exfatjournal implementation; guarded by the enclosing feature condition. ===== */
// Mount/recovery/provisioning temporarily borrows the first public handle.
// No application operation is accepted until this routine has completed.
/**
 * @brief Advance discovery, validation or provisioning of the exFAT undo journal.
 * @return AFATFS_OPERATION_SUCCESS when this step completes, AFATFS_OPERATION_IN_PROGRESS while work is pending,
 * or AFATFS_OPERATION_FAILURE on error.
 *
 * @note Available when ASYNCFS_ENABLE_EXFAT is enabled.
 */
static afatfsOperationStatus_e afatfs_journalMount(void)
{
    // GPT write protection forbids provisioning and recovery writes, including dirty-flag changes.
    if (afatfs.partitionType == AFATFS_PARTITION_EXFAT_READ_ONLY) {
        afatfs_initFileHandle(&afatfs.openFiles[0]);
        return AFATFS_OPERATION_SUCCESS;
    }
    afatfsJournal_t *journalState = &afatfs.journal;
    afatfsFile_t *fileHandle = &afatfs.openFiles[0];
    afatfsOperationStatus_e status;
    uint8_t *sector;
    if (journalState->phase == AFATFS_JOURNAL_ERROR) {
        afatfs_initFileHandle(fileHandle);
        return AFATFS_OPERATION_SUCCESS; // protected read-only mount
    }
    switch (journalState->lifecycle.initPhase) {
        case AFATFS_JOURNAL_INIT_PREPARE: {
            uint64_t capacity = (uint64_t)afatfs.fatSectors + ((uint64_t)afatfs.bitmapLength + 511) / 512 + 64;
            uint64_t bytes = (2 + 2 * capacity) * 512;
            bytes = (bytes + afatfs_clusterSize() - 1) / afatfs_clusterSize() * afatfs_clusterSize();
            if (bytes > UINT32_MAX) {
                afatfs_jfail();
                break;
            }
            journalState->capacity = (uint32_t)capacity;
            journalState->sectors = (uint32_t)(bytes / 512);
            if (journalState->found) {
                fileHandle->exfatEntryCount = 3;
                if (afatfs_exfatLoadEntrySet(fileHandle, AFATFS_JOURNAL_NAME) != 1 || !fileHandle->exfatNoFatChain ||
                    fileHandle->logicalSize != bytes || fileHandle->validDataLength != bytes ||
                    (fileHandle->attrib & FAT_FILE_ATTRIBUTE_DIRECTORY)) {
                    afatfs_jfail();
                    break;
                }
                journalState->firstCluster = fileHandle->firstCluster;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER;
            } else if (journalState->startupDirty) {
                afatfs_jfail();
                break;
            } else {
                journalState->provision = true;
                journalState->work.setup.candidate = 2;
                journalState->work.setup.runLength = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY;
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER:
        case AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER:
            if (afatfs_jio(false,
                    afatfs_jsector(
                        journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1))) {
                afatfs_jselectHeader(
                    journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER ? 0 : 1);
                if (journalState->lifecycle.initPhase == AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER) {
                    if (!journalState->validHeader || (journalState->startupDirty && !journalState->owned) ||
                        (!journalState->startupDirty && journalState->active)) {
                        afatfs_jfail();
                    } else {
                        journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVER;
                    }
                } else {
                    journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER;
                }
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT:
            if (!journalState->startupDirty || afatfs_jwriteHeader() == 1) {
                // Cached root/FAT pages may describe the interrupted state.
                memset(afatfs.cacheDescriptor, 0, sizeof(afatfs.cacheDescriptor));
                afatfs.cacheDirtyEntries = 0;
                afatfs.cacheFlushInProgress = false;
                afatfs.bitmapCursorCluster = 0;
                afatfs.bitmapCursorIndex = 0;
                afatfs.readOnly = false;
                afatfs.lastError = AFATFS_ERROR_NONE;
                afatfs_chdir(NULL);
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_MARK_DIRTY;
            }
            break;
        case AFATFS_JOURNAL_INIT_MARK_DIRTY: // Mark ownership only after the dirty boot marker is durable.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_FLUSH_DIRTY;
            break;
        case AFATFS_JOURNAL_INIT_FLUSH_DIRTY:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->owned = true;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP;
            break;
        case AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP:
            if (afatfs_jwriteHeader() == 1) {
                afatfs_initFileHandle(fileHandle);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY: // First mount of a clean volume: establish the dirty guard before bootstrap.
            status =
                afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE, 0);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            sector[106] |= 2;
            sector[112] = 0xFF;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            afatfs_createFile(fileHandle, AFATFS_JOURNAL_NAME,
                FAT_FILE_ATTRIBUTE_HIDDEN | FAT_FILE_ATTRIBUTE_SYSTEM | FAT_FILE_ATTRIBUTE_READ_ONLY,
                AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ, NULL, NULL);
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE:
            afatfs_fileOperationContinue(&afatfs.currentDirectory);
            afatfs_fileOperationContinue(fileHandle);
            if (fileHandle->operation.active != AFATFS_FILE_OPERATION_NONE) {
                break;
            }
            if (fileHandle->type != AFATFS_FILE_TYPE_NORMAL || fileHandle->firstCluster) {
                afatfs_jfail();
                break;
            }
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT: {
            // Locate a contiguous extent. The journal locator never needs the FAT at recovery.
            bool allocated;
            if (journalState->work.setup.candidate - 2 >= afatfs.numClusters) {
                afatfs_jfail();
                break;
            }
            status = afatfs_exfatBitmap(journalState->work.setup.candidate, false, false, &allocated);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.runLength = allocated ? 0 : journalState->work.setup.runLength + 1;
            journalState->work.setup.candidate++;
            if (journalState->work.setup.runLength == (journalState->sectors / afatfs.sectorsPerCluster)) {
                journalState->firstCluster =
                    journalState->work.setup.candidate - (journalState->sectors / afatfs.sectorsPerCluster);
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT;
            }
            break;
        }
        case AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT:
            if (journalState->work.setup.progress == journalState->sectors) {
                journalState->work.setup.progress = 0;
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT;
                break;
            }
            if (!journalState->ioPending && !journalState->ioDone) {
                memset(journalState->io, 0, 512);
            }
            if (afatfs_jio(true, afatfs_jsector(journalState->work.setup.progress)) == 1) {
                journalState->work.setup.progress++;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT:
            if (journalState->work.setup.progress == (journalState->sectors / afatfs.sectorsPerCluster)) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION;
                break;
            }
            status =
                afatfs_exfatBitmap(journalState->firstCluster + journalState->work.setup.progress, true, true, NULL);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->work.setup.progress++;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION:
            if (!afatfs_flushCache()) {
                break;
            }
            fileHandle->firstCluster = journalState->firstCluster;
            fileHandle->exfatNoFatChain = true;
            fileHandle->physicalSize = journalState->sectors * 512;
            fileHandle->logicalSize = fileHandle->physicalSize;
            fileHandle->validDataLength = fileHandle->physicalSize;
            fileHandle->committedDataLength = fileHandle->physicalSize;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE:
            status = afatfs_saveDirectoryEntry(fileHandle, AFATFS_SAVE_DIRECTORY_FOR_CLOSE);
            if (status != AFATFS_OPERATION_SUCCESS) {
                return status;
            }
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE:
            if (!afatfs_flushCache()) {
                break;
            }
            journalState->epoch = 1;
            journalState->owned = true;
            journalState->active = false;
            journalState->count = 0;
            journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER;
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER:
            if (afatfs_jwriteHeader() == 1) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER;
            }
            break;
        case AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER:
            if (afatfs_jwriteHeader() == 1) {
                afatfs_initFileHandle(fileHandle);
                journalState->work.capture.pendingCache = -1;
                journalState->ready = true;
                journalState->provision = false;
                journalState->phase = AFATFS_JOURNAL_IDLE;
                afatfs_chdir(NULL);
                return AFATFS_OPERATION_SUCCESS;
            }
            break;
        case AFATFS_JOURNAL_INIT_RECOVER:
            if (afatfs_journalRecover() == AFATFS_OPERATION_SUCCESS) {
                journalState->lifecycle.initPhase = AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT;
            }
            break;
    }
    return AFATFS_OPERATION_IN_PROGRESS;
}
#endif
