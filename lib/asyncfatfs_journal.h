#pragma once

#define AFATFS_JOURNAL_NAME "AFATFS.JNL"

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
// Runtime state only. Disk headers/records are serialized explicitly.
typedef struct afatfsJournal_t {
    AFATFS_ALIGNED uint8_t io[512];
    uint32_t serial;
    uint32_t firstCluster;
    uint32_t sectors;
    uint32_t capacity;
    uint32_t sequence;
    uint32_t epoch;
    uint32_t count;
    uint32_t headerSlot;
    uint32_t pendingSector;
    uint32_t scan;
    uint32_t progress;
    uint32_t target;
    uint32_t imageCRC;
    uint32_t candidate;
    uint32_t runLength;
    int pendingCache;
    uint8_t phase;
    uint8_t initPhase;
    uint8_t shutdownPhase;
    union {
        uint8_t foundEntries;
        uint8_t recoveryPhase;
    } mount;
    bool ready : 1;
    bool active : 1;
    bool owned : 1;
    bool found : 1;
    bool validHeader : 1;
    bool headerPrepared : 1;
    bool ioPending : 1;
    bool ioDone : 1;
    bool ioFailed : 1;
    bool startupDirty : 1;
    bool provision : 1;
    bool blockAllocation : 1;
} afatfsJournal_t;
