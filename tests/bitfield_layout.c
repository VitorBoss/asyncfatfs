// Standalone layout/flag probe: compile this TU instead of a separate asyncfatfs.c.
#include "../lib/asyncfatfs.c"
#include <stddef.h>

// C99 compile-time checks; do not require _Static_assert or _Alignof from C11.
#define LAYOUT_ASSERT(name, condition) typedef char name[(condition) ? 1 : -1]
#ifdef _MSC_VER
#define TYPE_ALIGNMENT(valueType) __alignof(valueType)
#else
#define TYPE_ALIGNMENT(valueType) __alignof__(valueType)
#endif
#define MEMBER_ALIGNED(recordType, member, valueType) (offsetof(recordType, member) % TYPE_ALIGNMENT(valueType) == 0)

LAYOUT_ASSERT(check_cache_offset, offsetof(afatfs_t, cache) % 4 == 0);
LAYOUT_ASSERT(check_cache_alignment, TYPE_ALIGNMENT(afatfs_t) >= 4);
LAYOUT_ASSERT(check_file_array, MEMBER_ALIGNED(afatfs_t, openFiles, afatfsFile_t));
LAYOUT_ASSERT(check_current_directory, MEMBER_ALIGNED(afatfs_t, currentDirectory, afatfsFile_t));
LAYOUT_ASSERT(check_file_operation, MEMBER_ALIGNED(afatfsFile_t, operation, afatfsFileOperation_t));
LAYOUT_ASSERT(check_truncate_size, MEMBER_ALIGNED(afatfsTruncateFile_t, targetSize, uint32_t));
LAYOUT_ASSERT(check_request_values, MEMBER_ALIGNED(afatfsRequest_t, value, uint32_t));
LAYOUT_ASSERT(check_request_scratch, MEMBER_ALIGNED(afatfsRequest_t, scratch, afatfsPathFrame_t));
LAYOUT_ASSERT(check_request_callback, MEMBER_ALIGNED(afatfsRequest_t, callback, afatfsResultCallback_t));
LAYOUT_ASSERT(check_directory_pointer, MEMBER_ALIGNED(afatfsDirectory_t, next, afatfsDirectory_t *));
LAYOUT_ASSERT(check_card_serial, MEMBER_ALIGNED(sdcardMetadata_t, productSerial, uint32_t));
LAYOUT_ASSERT(check_card_blocks, MEMBER_ALIGNED(sdcardMetadata_t, numBlocks, uint32_t));
LAYOUT_ASSERT(check_card_oem, MEMBER_ALIGNED(sdcardMetadata_t, oemID, uint16_t));
LAYOUT_ASSERT(check_mbr_size, sizeof(mbrPartitionEntry_t) == 16);
LAYOUT_ASSERT(check_fat_entry_size, sizeof(fatDirectoryEntry_t) == 32);
LAYOUT_ASSERT(check_exfat_entry_size, sizeof(exFatFileEntry_t) == 32);
LAYOUT_ASSERT(check_exfat_stream_size, sizeof(exFatStreamEntry_t) == 32);
LAYOUT_ASSERT(check_exfat_cluster_offset, offsetof(exFatStreamEntry_t, firstCluster) == 20);
LAYOUT_ASSERT(check_boot_shift_offset, offsetof(fatVolumeID_t, exFat.bytesPerSectorShift) == 108);
LAYOUT_ASSERT(check_extend_prefix, offsetof(afatfsExtendSubdirectory_t, appendFreeCluster) == 0);
LAYOUT_ASSERT(check_unlink_prefix, offsetof(afatfsUnlinkFile_t, truncateFile) == 0);
LAYOUT_ASSERT(check_truncate_traversal, MEMBER_ALIGNED(afatfsTruncateFile_t, traversal, uint32_t));
LAYOUT_ASSERT(check_truncate_workspace, sizeof(((afatfsTruncateFile_t *)0)->traversal) == sizeof(uint32_t));
LAYOUT_ASSERT(check_append_progress, MEMBER_ALIGNED(afatfsAppendFreeCluster_t, progress, uint32_t));
LAYOUT_ASSERT(check_append_workspace, sizeof(((afatfsAppendFreeCluster_t *)0)->progress) == sizeof(uint32_t));
#ifdef ASYNCFS_ENABLE_LFN
LAYOUT_ASSERT(check_lfn_cluster, MEMBER_ALIGNED(afatfsFile_t, lfnCluster, uint32_t));
LAYOUT_ASSERT(check_lfn_offset, MEMBER_ALIGNED(afatfsFile_t, lfnOffset, uint32_t));
LAYOUT_ASSERT(check_lfn_output, MEMBER_ALIGNED(afatfsFile_t, lfnOutput, afatfsFileInfo_t *));
LAYOUT_ASSERT(check_lfn_alias, MEMBER_ALIGNED(afatfsCreateFile_t, location, uint32_t));
LAYOUT_ASSERT(check_lfn_scan, sizeof(((afatfsCreateFile_t *)0)->scan) == sizeof(afatfsLongNameState_t));
LAYOUT_ASSERT(check_lfn_entry_size, sizeof(fatLfnDirectoryEntry_t) == 32);
LAYOUT_ASSERT(check_lfn_cluster_offset, offsetof(fatLfnDirectoryEntry_t, lfnAlwaysZero) == 26);
#endif
#ifdef __XC16__
LAYOUT_ASSERT(check_16bit_int, sizeof(int) == 2);
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
LAYOUT_ASSERT(check_valid_length, MEMBER_ALIGNED(afatfsFile_t, validDataLength, uint32_t));
LAYOUT_ASSERT(check_mount_state, MEMBER_ALIGNED(afatfs_t, initState.exfat, afatfsExfatMount_t));
LAYOUT_ASSERT(check_mount_finder, MEMBER_ALIGNED(afatfsExfatMount_t, progress.directory.finder, afatfsFinder_t));
LAYOUT_ASSERT(check_mount_progress, MEMBER_ALIGNED(afatfsExfatMount_t, progress, uint32_t));
LAYOUT_ASSERT(check_mount_workspace, sizeof(afatfsExfatMount_t) == 8 * sizeof(uint32_t));
#endif
#ifdef AFATFS_ENABLE_JOURNAL
LAYOUT_ASSERT(check_journal_init_zero, AFATFS_JOURNAL_INIT_PREPARE == 0);
LAYOUT_ASSERT(check_journal_init_range, AFATFS_JOURNAL_INIT_COUNT <= UINT8_MAX + 1U);
LAYOUT_ASSERT(check_journal_init_storage, sizeof(((afatfsJournal_t *)0)->lifecycle.initPhase) == 1);
LAYOUT_ASSERT(check_journal_shutdown_zero, AFATFS_JOURNAL_SHUTDOWN_WAIT_COMMIT == 0);
LAYOUT_ASSERT(check_journal_shutdown_range, AFATFS_JOURNAL_SHUTDOWN_COUNT <= UINT8_MAX + 1U);
LAYOUT_ASSERT(check_journal_shutdown_storage, sizeof(((afatfsJournal_t *)0)->lifecycle.shutdownPhase) == 1);
LAYOUT_ASSERT(check_journal_recovery_zero, AFATFS_JOURNAL_RECOVERY_PREPARE == 0);
LAYOUT_ASSERT(check_journal_recovery_range, AFATFS_JOURNAL_RECOVERY_COUNT <= UINT8_MAX + 1U);
LAYOUT_ASSERT(check_journal_recovery_storage, sizeof(((afatfsJournal_t *)0)->mount.recoveryPhase) == 1);
LAYOUT_ASSERT(check_journal_member, MEMBER_ALIGNED(afatfs_t, journal, afatfsJournal_t));
LAYOUT_ASSERT(check_journal_alignment, TYPE_ALIGNMENT(afatfsJournal_t) >= 4);
LAYOUT_ASSERT(check_journal_io, offsetof(afatfsJournal_t, io) % 4 == 0);
LAYOUT_ASSERT(check_journal_cluster, MEMBER_ALIGNED(afatfsJournal_t, firstCluster, uint32_t));
LAYOUT_ASSERT(check_journal_work, MEMBER_ALIGNED(afatfsJournal_t, work, uint32_t));
LAYOUT_ASSERT(check_journal_capture_index, MEMBER_ALIGNED(afatfsJournal_t, work.capture.pendingCache, int));
LAYOUT_ASSERT(check_journal_capture_record, MEMBER_ALIGNED(afatfsJournal_t, work.capture.record, uint32_t));
LAYOUT_ASSERT(check_journal_work_size, sizeof(((afatfsJournal_t *)0)->work) == 3 * sizeof(uint32_t));
LAYOUT_ASSERT(check_journal_lifecycle_size, sizeof(((afatfsJournal_t *)0)->lifecycle) == 1);
LAYOUT_ASSERT(check_journal_lifecycle_overlap,
    offsetof(afatfsJournal_t, lifecycle.initPhase) == offsetof(afatfsJournal_t, lifecycle.shutdownPhase));
#endif

#define DIRECTORY_FLAGS(APPLY)                                                                                         \
    APPLY(registered)                                                                                                  \
    APPLY(reading)
#define REQUEST_FLAGS(APPLY)                                                                                           \
    APPLY(complete)                                                                                                    \
    APPLY(waiting)                                                                                                     \
    APPLY(checkpoint)                                                                                                  \
    APPLY(driving)
#define TRUNCATE_FLAGS(APPLY)                                                                                          \
    APPLY(nextClusterValid)                                                                                            \
    APPLY(fatCleared)                                                                                                  \
    APPLY(linear)
#ifdef ASYNCFS_ENABLE_EXFAT
#define CREATE_FLAGS(APPLY)                                                                                            \
    APPLY(directory)                                                                                                   \
    APPLY(directoryEnd)
#define FILE_FLAGS(APPLY)                                                                                              \
    APPLY(modified)                                                                                                    \
    APPLY(exfatNoFatChain)                                                                                             \
    APPLY(dataDirty)
#define GPT_FLAGS(APPLY)                                                                                               \
    APPLY(readOnly)                                                                                                    \
    APPLY(backupKnown)
#define FILESYSTEM_FLAGS(APPLY)                                                                                        \
    APPLY(cacheFlushInProgress)                                                                                        \
    APPLY(filesystemFull)                                                                                              \
    APPLY(readOnly)                                                                                                    \
    APPLY(fsInfoDirty)                                                                                                 \
    APPLY(upcaseSkip)                                                                                                  \
    APPLY(exfatClean)
#else
#define CREATE_FLAGS(APPLY) APPLY(directory)
#define FILE_FLAGS(APPLY) APPLY(modified)
#define FILESYSTEM_FLAGS(APPLY)                                                                                        \
    APPLY(cacheFlushInProgress)                                                                                        \
    APPLY(filesystemFull)                                                                                              \
    APPLY(readOnly)                                                                                                    \
    APPLY(fsInfoDirty)
#endif

#ifdef ASYNCFS_ENABLE_LFN
#undef CREATE_FLAGS
#define CREATE_FLAGS(APPLY)                                                                                            \
    APPLY(directory)                                                                                                   \
    APPLY(directoryEnd)
#undef FILE_FLAGS
#ifdef ASYNCFS_ENABLE_EXFAT
#define FILE_FLAGS(APPLY)                                                                                              \
    APPLY(modified)                                                                                                    \
    APPLY(lfnAscii)                                                                                                    \
    APPLY(exfatNoFatChain)                                                                                             \
    APPLY(dataDirty)
#else
#define FILE_FLAGS(APPLY)                                                                                              \
    APPLY(modified)                                                                                                    \
    APPLY(lfnAscii)
#endif
#define LONGNAME_FLAGS(APPLY)                                                                                          \
    APPLY(matched)                                                                                                     \
    APPLY(ascii)
#endif

#define COUNT_FLAG(member) +1
#define STORE_FLAG(member)                                                                                             \
    do {                                                                                                               \
        object.member = (pattern & (UINT32_C(1) << flagIndex)) ? nonzero : 0;                                          \
        flagIndex++;                                                                                                   \
    } while (0);
#define VERIFY_FLAG(member)                                                                                            \
    do {                                                                                                               \
        if (object.member != ((pattern >> flagIndex) & 1)) {                                                           \
            return __LINE__;                                                                                           \
        }                                                                                                              \
        flagIndex++;                                                                                                   \
    } while (0);
#define DEFINE_FLAG_TEST(functionName, objectType, fields)                                                             \
    static int functionName(void)                                                                                      \
    {                                                                                                                  \
        objectType object;                                                                                             \
        memset(&object, 0, sizeof(object));                                                                            \
        enum {                                                                                                         \
            flagCount = 0 fields(COUNT_FLAG)                                                                           \
        };                                                                                                             \
        volatile uint32_t nonzero = 2;                                                                                 \
        for (unsigned pass = 0; pass < 2; pass++) {                                                                    \
            nonzero = pass ? UINT32_C(0x80000000) : 2;                                                                 \
            for (uint32_t pattern = 0; pattern < (UINT32_C(1) << flagCount); pattern++) {                              \
                unsigned flagIndex = 0;                                                                                \
                fields(STORE_FLAG) /* Finish all stores before checking neighboring flags. */                          \
                    flagIndex = 0;                                                                                     \
                fields(VERIFY_FLAG)                                                                                    \
            }                                                                                                          \
        }                                                                                                              \
        return 0;                                                                                                      \
    }

DEFINE_FLAG_TEST(check_directory_flags, afatfsDirectory_t, DIRECTORY_FLAGS)
DEFINE_FLAG_TEST(check_request_flags, afatfsRequest_t, REQUEST_FLAGS)
DEFINE_FLAG_TEST(check_truncate_flags, afatfsTruncateFile_t, TRUNCATE_FLAGS)
DEFINE_FLAG_TEST(check_create_flags, afatfsCreateFile_t, CREATE_FLAGS)
DEFINE_FLAG_TEST(check_file_flags, afatfsFile_t, FILE_FLAGS)
DEFINE_FLAG_TEST(check_filesystem_flags, afatfs_t, FILESYSTEM_FLAGS)
#ifdef AFATFS_ENABLE_JOURNAL
#define JOURNAL_FLAGS(APPLY)                                                                                           \
    APPLY(ready)                                                                                                       \
    APPLY(active)                                                                                                      \
    APPLY(owned)                                                                                                       \
    APPLY(found)                                                                                                       \
    APPLY(validHeader)                                                                                                 \
    APPLY(headerPrepared)                                                                                              \
    APPLY(ioPending)                                                                                                   \
    APPLY(ioDone)                                                                                                      \
    APPLY(ioFailed)                                                                                                    \
    APPLY(startupDirty)                                                                                                \
    APPLY(provision)                                                                                                   \
    APPLY(blockAllocation)                                                                                             \
    APPLY(headerSlot)
DEFINE_FLAG_TEST(check_shared_journal_flags, afatfsJournal_t, JOURNAL_FLAGS)
#endif
#ifdef ASYNCFS_ENABLE_LFN
DEFINE_FLAG_TEST(check_longname_flags, afatfsLongNameState_t, LONGNAME_FLAGS)
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
DEFINE_FLAG_TEST(check_gpt_flags, afatfsGpt_t, GPT_FLAGS)
#endif

// XC16 -S exposes these compile-time values as .word records, without hardware I/O.
const uint16_t afatfs_layout_sizes[] = {
    sizeof(int),
    sizeof(void *),
    sizeof(afatfs_t),
    sizeof(afatfsFile_t),
    sizeof(afatfsRequest_t),
    sizeof(afatfsDirectory_t),
    sizeof(afatfsTruncateFile_t),
#ifdef AFATFS_ENABLE_JOURNAL
    sizeof(afatfsJournal_t),
    offsetof(afatfsJournal_t, io),
    0, // Removed before-image buffer; keep the historical metric columns.
    0, // Removed header buffer.
#endif
};

const uint16_t afatfs_operation_layout_sizes[] = {
    sizeof(afatfsCreateFile_t),
    sizeof(afatfsAppendFreeCluster_t),
    sizeof(afatfsExtendSubdirectory_t),
    sizeof(afatfsFinder_t),
};

int main(void)
{
    int result = 0;
    result |= check_directory_flags();
    result |= check_request_flags();
    result |= check_truncate_flags();
    result |= check_create_flags();
    result |= check_file_flags();
    result |= check_filesystem_flags();
#ifdef AFATFS_ENABLE_JOURNAL
    result |= check_shared_journal_flags();
#endif
#ifdef ASYNCFS_ENABLE_LFN
    result |= check_longname_flags();
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
    result |= check_gpt_flags();
#endif
    if (result) {
        fprintf(stderr, "Boolean conversion or flag isolation failed at line %d\n", result);
        return 1;
    }
    for (unsigned index = 0; index < sizeof(afatfs_layout_sizes) / sizeof(afatfs_layout_sizes[0]); index++) {
        printf("%u%s", (unsigned)afatfs_layout_sizes[index],
            index + 1 == sizeof(afatfs_layout_sizes) / sizeof(afatfs_layout_sizes[0]) ? "\n" : ",");
    }
    printf("OPERATIONS,%u,%u,%u,%u\n", (unsigned)afatfs_operation_layout_sizes[0],
        (unsigned)afatfs_operation_layout_sizes[1], (unsigned)afatfs_operation_layout_sizes[2],
        (unsigned)afatfs_operation_layout_sizes[3]);
    puts("PASS C99 Boolean conversions, all flag combinations and natural/sector alignment");
    return 0;
}
