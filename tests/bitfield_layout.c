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
#ifdef __XC16__
LAYOUT_ASSERT(check_16bit_int, sizeof(int) == 2);
#endif
#ifdef ASYNCFS_ENABLE_EXFAT
LAYOUT_ASSERT(check_valid_length, MEMBER_ALIGNED(afatfsFile_t, validDataLength, uint32_t));
LAYOUT_ASSERT(check_journal_member, MEMBER_ALIGNED(afatfs_t, journal, afatfsJournal_t));
LAYOUT_ASSERT(check_journal_alignment, TYPE_ALIGNMENT(afatfsJournal_t) >= 4);
LAYOUT_ASSERT(check_journal_io, offsetof(afatfsJournal_t, io) % 4 == 0);
LAYOUT_ASSERT(check_journal_before, offsetof(afatfsJournal_t, before) % 4 == 0);
LAYOUT_ASSERT(check_journal_header, offsetof(afatfsJournal_t, header) % 4 == 0);
LAYOUT_ASSERT(check_journal_cluster, MEMBER_ALIGNED(afatfsJournal_t, firstCluster, uint32_t));
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
#define JOURNAL_FLAGS(APPLY)                                                                                           \
    APPLY(ready)                                                                                                       \
    APPLY(provision)                                                                                                   \
    APPLY(found)                                                                                                       \
    APPLY(owned)                                                                                                       \
    APPLY(active)                                                                                                      \
    APPLY(ioPending)                                                                                                   \
    APPLY(ioDone)                                                                                                      \
    APPLY(ioFailed)                                                                                                    \
    APPLY(headerPrepared)                                                                                              \
    APPLY(startupDirty)                                                                                                \
    APPLY(blockAllocation)
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
#ifdef ASYNCFS_ENABLE_EXFAT
DEFINE_FLAG_TEST(check_gpt_flags, afatfsGpt_t, GPT_FLAGS)
DEFINE_FLAG_TEST(check_journal_flags, afatfsJournal_t, JOURNAL_FLAGS)
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
#ifdef ASYNCFS_ENABLE_EXFAT
    sizeof(afatfsJournal_t),
    offsetof(afatfsJournal_t, io),
    offsetof(afatfsJournal_t, before),
    offsetof(afatfsJournal_t, header),
#endif
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
#ifdef ASYNCFS_ENABLE_EXFAT
    result |= check_gpt_flags();
    result |= check_journal_flags();
#endif
    if (result) {
        fprintf(stderr, "Boolean conversion or flag isolation failed at line %d\n", result);
        return 1;
    }
    for (unsigned index = 0; index < sizeof(afatfs_layout_sizes) / sizeof(afatfs_layout_sizes[0]); index++) {
        printf("%u%s", (unsigned)afatfs_layout_sizes[index],
            index + 1 == sizeof(afatfs_layout_sizes) / sizeof(afatfs_layout_sizes[0]) ? "\n" : ",");
    }
    puts("PASS C99 Boolean conversions, all flag combinations and natural/sector alignment");
    return 0;
}
