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
    // Keep backend buffers word/DMA-aligned even when enums occupy one byte.
    AFATFS_ALIGNED uint8_t io[512];
    AFATFS_ALIGNED uint8_t before[512];
    AFATFS_ALIGNED uint8_t header[512];
    uint32_t firstCluster;
    uint32_t clusters;
    uint32_t sectors;
    uint32_t capacity;
    uint32_t sequence;
    uint32_t epoch;
    uint32_t count;
    uint32_t headerSlot;
    uint32_t pendingSector;
    uint32_t progress;
    uint32_t candidate;
    uint32_t runLength;
    uint32_t scan;
    uint32_t target;
    uint32_t imageCRC;
    int pendingCache;
    afatfsJournalPhase_e phase;
    uint8_t initPhase;
    uint8_t foundEntries;
    // C99 _Bool bit-fields: runtime state only, never copied to the on-disk format.
    bool ready : 1;
    bool provision : 1;
    bool found : 1;
    bool owned : 1;
    bool active : 1;
    bool ioPending : 1;
    bool ioDone : 1;
    bool ioFailed : 1;
    bool headerPrepared : 1;
    bool startupDirty : 1;
    bool blockAllocation : 1;
} afatfsJournal_t;
