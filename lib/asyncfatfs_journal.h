#pragma once

#define AFATFS_JOURNAL_NAME "AFATFS.JNL"

// Common mount/provisioning steps. FAT32 adds discovery and chain validation;
// both adapters use the same steps for headers, recovery and extent allocation.
typedef enum {
    AFATFS_JOURNAL_INIT_PREPARE = 0,
    AFATFS_JOURNAL_INIT_READ_PRIMARY_HEADER,
    AFATFS_JOURNAL_INIT_READ_SECONDARY_HEADER,
    AFATFS_JOURNAL_INIT_RECOVER,
    AFATFS_JOURNAL_INIT_RECOVERY_CHECKPOINT,
    AFATFS_JOURNAL_INIT_MARK_DIRTY,
    AFATFS_JOURNAL_INIT_FLUSH_DIRTY,
    AFATFS_JOURNAL_INIT_CLAIM_OWNERSHIP,
    AFATFS_JOURNAL_INIT_PROVISION_MARK_DIRTY,
    AFATFS_JOURNAL_INIT_PROVISION_CREATE_FILE,
    AFATFS_JOURNAL_INIT_PROVISION_WAIT_FILE,
    AFATFS_JOURNAL_INIT_PROVISION_FIND_EXTENT,
    AFATFS_JOURNAL_INIT_PROVISION_ZERO_EXTENT,
    AFATFS_JOURNAL_INIT_PROVISION_ALLOCATE_EXTENT,
    AFATFS_JOURNAL_INIT_PROVISION_FLUSH_ALLOCATION,
    AFATFS_JOURNAL_INIT_PROVISION_PUBLISH_FILE,
    AFATFS_JOURNAL_INIT_PROVISION_FLUSH_FILE,
    AFATFS_JOURNAL_INIT_PROVISION_FIRST_HEADER,
    AFATFS_JOURNAL_INIT_PROVISION_SECOND_HEADER,
    AFATFS_JOURNAL_INIT_FAT_READ_PRIMARY_STATUS,
    AFATFS_JOURNAL_INIT_FAT_READ_MIRROR_STATUS,
    AFATFS_JOURNAL_INIT_FAT_FIND_FILE,
    AFATFS_JOURNAL_INIT_FAT_VALIDATE_PRIMARY_CHAIN,
    AFATFS_JOURNAL_INIT_FAT_VALIDATE_MIRROR_CHAIN,
    AFATFS_JOURNAL_INIT_COUNT
} afatfsJournalInitPhase_e;

typedef enum {
    AFATFS_JOURNAL_SHUTDOWN_WAIT_COMMIT = 0,
    AFATFS_JOURNAL_SHUTDOWN_MARK_CLEAN,
    AFATFS_JOURNAL_SHUTDOWN_FLUSH_CLEAN,
    AFATFS_JOURNAL_SHUTDOWN_RELEASE_OWNERSHIP,
    AFATFS_JOURNAL_SHUTDOWN_COMPLETE,
    AFATFS_JOURNAL_SHUTDOWN_COUNT
} afatfsJournalShutdownPhase_e;

typedef enum {
    AFATFS_JOURNAL_RECOVERY_PREPARE = 0,
    AFATFS_JOURNAL_RECOVERY_VALIDATE_RECORD,
    AFATFS_JOURNAL_RECOVERY_VALIDATE_IMAGE,
    AFATFS_JOURNAL_RECOVERY_REPLAY_READ_RECORD,
    AFATFS_JOURNAL_RECOVERY_REPLAY_READ_IMAGE,
    AFATFS_JOURNAL_RECOVERY_REPLAY_WRITE_IMAGE,
    AFATFS_JOURNAL_RECOVERY_COUNT
} afatfsJournalRecoveryPhase_e;

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
    // Setup ends before capture starts. Replay needs its own target, counter
    // and CRC simultaneously; capture only needs scan OR CRC at each step.
    union {
        struct {
            uint32_t candidate;
            uint32_t runLength;
            uint32_t progress;
        } setup;
        struct {
            uint32_t target;
            uint32_t progress;
            uint32_t imageCRC;
        } recovery;
        struct {
            uint32_t pendingSector;
            union {
                uint32_t scan;
                uint32_t imageCRC;
            } record;
            int pendingCache;
        } capture;
    } work;
    // phase still coexists with mount errors and with shutdown's commit wait.
    uint8_t phase;
    union {
        uint8_t initPhase;     // afatfsJournalInitPhase_e.
        uint8_t shutdownPhase; // afatfsJournalShutdownPhase_e; reset before READY.
    } lifecycle;
    union {
        uint8_t foundEntries;
        uint8_t recoveryPhase; // afatfsJournalRecoveryPhase_e, stored as one byte on every ABI.
    } mount;
    bool headerSlot : 1; // Only two header sectors: index 0 or 1, never serialized.
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
