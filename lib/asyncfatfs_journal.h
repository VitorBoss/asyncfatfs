#pragma once

// Internal exFAT undo journal. All storage is statically allocated, like the sector cache.
#define AFATFS_JOURNAL_NAME "AFATFS.JNL"
#define AFATFS_JOURNAL_HEADER_MAGIC "AFJNL002"
#define AFATFS_JOURNAL_RECORD_MAGIC "AFUNDO02"

typedef enum {
    AFATFS_JOURNAL_IDLE,
    AFATFS_JOURNAL_START,
    AFATFS_JOURNAL_INDEX_READ,
    AFATFS_JOURNAL_IMAGE_WRITE,
    AFATFS_JOURNAL_RECORD_WRITE,
    AFATFS_JOURNAL_RECORD_HEADER,
    AFATFS_JOURNAL_CAPTURE_DONE,
    AFATFS_JOURNAL_COMMIT,
    AFATFS_JOURNAL_ERROR
} afatfsJournalPhase_e;

typedef struct afatfsJournal_t {
    bool ready, provision, found, owned, active;
    bool ioPending, ioDone, ioFailed, headerPrepared;
    bool startupDirty, blockAllocation;
    uint8_t initPhase, foundEntries;
    uint32_t firstCluster, clusters, sectors, capacity;
    uint32_t sequence, epoch, count, headerSlot;
    uint32_t pendingSector;
    uint32_t progress, candidate, runLength;
    uint32_t scan, target, imageCRC;
    int pendingCache;
    afatfsJournalPhase_e phase;
    uint8_t io[512];
    uint8_t before[512];
    uint8_t header[512];
} afatfsJournal_t;
