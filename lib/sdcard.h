#pragma once

#include <stdint.h>
#include <stdbool.h>
// Card Identification (CID) register.
typedef struct sdcard_cid_t {
    uint8_t manufacturerID; /* byte 0 - Manufacturer ID */
    uint16_t oemID;         /* byte 1-2 - OEM/Application ID.*/
    char prodName[5];       /* byte 3-7 - Product name.*/
    uint8_t prodRevMinor:4; /* byte 8 - Product revision - n.m two 4-bit nibbles. */
    uint8_t prodRevMajor:4; /* byte 8 - Product revision - n.m two 4-bit nibbles. */
    uint32_t prodSerial;    /* byte 9-12 - Product serial 32-bit number Big Endian format. */
    uint16_t prodYear;      /* byte 13-14 - Manufacturing date big endian - four nibbles RYYM Reserved Year Month. */
    uint8_t crc;            /* byte 15 - CRC7 bits 1-7 checksum, bit 0 always 1. */
} sdcardCID_t;

typedef struct sdcard_metadata_t {
    sdcardCID_t cid;
    uint32_t numBlocks; /* Card capacity in 512-byte blocks*/
} sdcardMetadata_t;

typedef enum {
    SDCARD_BLOCK_OPERATION_READ,
    SDCARD_BLOCK_OPERATION_WRITE,
    SDCARD_BLOCK_OPERATION_ERASE,
} sdcardBlockOperation_e;

typedef enum {
    SDCARD_OPERATION_IN_PROGRESS,
    SDCARD_OPERATION_BUSY,
    SDCARD_OPERATION_SUCCESS,
    SDCARD_OPERATION_FAILURE
} sdcardOperationStatus_e;

typedef void (*sdcard_operationCompleteCallback_c)(
    sdcardBlockOperation_e operation, uint32_t blockIndex, uint8_t *buffer, uint32_t callbackData);

typedef void (*sdcard_profilerCallback_c)(sdcardBlockOperation_e operation, uint32_t blockIndex, uint32_t duration);
// Synchronously cancel queued/in-flight I/O (including DMA and read/write sessions).
// After return no old request may access a buffer or write to the medium, and reset
// must not start new I/O. Late software completion notifications may still be delivered;
// preserve their original callbackData so the retired mount can reject them.
// Called by init, forced/clean teardown and media-removal notification.
void sdcard_reset(void);
/**
 * Optional when AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT is enabled in the library.
 * Begin a bounded, consecutive read session. No buffer is owned until a normal
 * readBlock request is accepted; deliver exactly one callback for each accepted
 * request, preserving its buffer and callbackData. Never read beyond blockCount.
 * Repeating begin at the session's next block must preserve the existing session.
 * Nonconsecutive reads and all writes must safely stop a read session first.
 * A read failure ends the session; no subsequent buffer may be reported valid.
 * reset synchronously cancels both read/write sessions and in-flight DMA.
 * SUCCESS means ready, BUSY/IN_PROGRESS means retry, FAILURE means no session
 * was accepted. Never pre-erase or write any sector when starting/stopping reads.
 */
sdcardOperationStatus_e sdcard_beginReadBlocks(uint32_t blockIndex, uint32_t blockCount);
/**
 * Stop an optional read session, including early termination. May return BUSY
 * while a buffer or stop command is in flight. SUCCESS releases all read-session
 * ownership; with no active read session this is an idempotent success.
 */
sdcardOperationStatus_e sdcard_endReadBlocks(void);
/**
 * Optional when AFATFS_READ_SESSION_QUERY is enabled. Pure query: true only if
 * the ready backend can accept this exact next block within the still-active,
 * previously bounded read session. Never extend/restart a session, transfer a
 * block, acquire a buffer or change counters. Return false while busy/stopping,
 * after any read failure, or after reset/removal. Existing backends need not
 * implement this function unless the library explicitly enables the query.
 */
bool sdcard_isReadBlockInSequence(uint32_t blockIndex);
/**
 * Read the 512-byte block with the given index into the given 512-byte buffer.
 *
 * When the read completes, your callback will be called. If the read was successful, the buffer pointer will be the
 * same buffer you originally passed in, otherwise the buffer will be set to NULL.
 *
 * You must keep the pointer to the buffer valid until the operation completes!
 *
 * Returns:
 *     true - The operation was successfully queued for later completion, your callback will be called later
 *     false - The operation could not be started due to the card being busy (try again later).
 */
bool sdcard_readBlock(
    uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData);

/**
 * Write the 512-byte block from the given buffer into the block with the given index.
 *
 * If the write does not complete immediately, your callback will be called later. If the write was successful, the
 * buffer pointer will be the same buffer you originally passed in, otherwise the buffer will be set to NULL.
 *
 * Returns:
 *     SDCARD_OPERATION_IN_PROGRESS - Your buffer is currently being transmitted to the card and your callback will be
 *                                    called later to report the completion. The buffer pointer must remain valid until
 *                                    that time.
 *     SDCARD_OPERATION_SUCCESS     - Your buffer has been transmitted to the card now.
 *     SDCARD_OPERATION_BUSY        - The card is already busy and cannot accept your write
 *     SDCARD_OPERATION_FAILURE     - Your write was rejected by the card, card will be reset
 */
sdcardOperationStatus_e sdcard_writeBlock(
    uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData);

/**
 * Will be called by afatfs_poll() periodically for the SD card to perform in-progress transfers.
 *
 * Returns true if the card is ready to accept new commands.
 */
bool sdcard_poll();

/**
 * Begin writing a series of consecutive blocks beginning at the given block index. This will allow (but not require)
 * the SD card to pre-erase the number of blocks you specifiy, which can allow the writes to complete faster.
 *
 * Afterwards, just call sdcard_writeBlock() as normal to write those blocks consecutively.
 *
 * The multi-block write will be aborted automatically when writing to a non-consecutive address, or by performing a
 * read. You can abort it manually by calling sdcard_endWriteBlocks().
 *
 * If you don't want to implement this feature, remove the define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT in asyncfatfs.c.
 *
 * Returns:
 *     SDCARD_OPERATION_SUCCESS     - Multi-block write has been queued
 *     SDCARD_OPERATION_BUSY        - The card is already busy and cannot accept your write
 *     SDCARD_OPERATION_FAILURE     - A fatal error occured, card will be reset
 */
sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t blockIndex, uint32_t blockCount);

/**
 * Abort a multiple-block write early (before all the `blockCount` blocks had been written).
 *
 * If you don't want to implement this feature, remove the define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT in asyncfatfs.c.
 *
 * Returns:
 *     SDCARD_OPERATION_SUCCESS     - Multi-block write has been cancelled, or no multi-block write was in progress.
 *     SDCARD_OPERATION_BUSY        - The card is busy with another operation and could not cancel the multi-block write.
 */
sdcardOperationStatus_e sdcard_endWriteBlocks();

/**
 * Only required to be provided when using AFATFS_USE_INTROSPECTIVE_LOGGING.
 */
void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback);
