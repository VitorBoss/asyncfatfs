// Exercise the shipped simulator's optional read-session contract directly.
#include <assert.h>
#include "sdcard_sim.c"

static unsigned completions;

static void readDone(sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    assert(operation == SDCARD_BLOCK_OPERATION_READ);
    assert(buffer && generation == 73);
    for (unsigned index = 0; index < SDCARD_SIM_BLOCK_SIZE; index++) {
        assert(buffer[index] == (uint8_t)sector);
    }
    completions++;
}

static void finishRead(uint32_t sector, uint8_t *buffer)
{
    assert(sdcard_readBlock(sector, buffer, readDone, 73));
    assert(!sdcard_isReadBlockInSequence(sector + 1));
    assert(sdcard_endReadBlocks() == SDCARD_OPERATION_BUSY);
    assert(sdcard_writeBlock(20, buffer, NULL, 0) == SDCARD_OPERATION_BUSY);
    while (!sdcard_poll()) {
    }
}

int main(int count, char **arguments)
{
    assert(count == 2);
    FILE *image = fopen(arguments[1], "wb");
    assert(image);
    uint8_t buffer[SDCARD_SIM_BLOCK_SIZE];
    for (unsigned sector = 0; sector < 32; sector++) {
        memset(buffer, sector, sizeof(buffer));
        assert(fwrite(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
    }
    assert(!fclose(image));
    assert(sdcard_sim_init(arguments[1]));
    assert(sdcard_beginReadBlocks(31, 2) == SDCARD_OPERATION_FAILURE);
    assert(sdcard_beginReadBlocks(2, 0) == SDCARD_OPERATION_FAILURE);
    assert(sdcard_beginReadBlocks(2, 3) == SDCARD_OPERATION_SUCCESS);
    assert(sdcard_isReadBlockInSequence(2));
    assert(sdcard_isReadBlockInSequence(2));
    assert(!sdcard_isReadBlockInSequence(3));
    assert(sdcard.multiNextBlock == 2 && sdcard.multiBlocksRemain == 3);
    finishRead(2, buffer);
    assert(sdcard.multiBlocksRemain == 2);
    assert(sdcard_isReadBlockInSequence(3));
    assert(sdcard_beginReadBlocks(3, 5) == SDCARD_OPERATION_SUCCESS);
    assert(sdcard.multiBlocksRemain == 2); // A retry does not extend the bounded run.
    finishRead(3, buffer);
    finishRead(4, buffer);
    assert(sdcard.state == SDCARD_STATE_READY && !sdcard.multiBlocksRemain);
    assert(!sdcard_isReadBlockInSequence(5));
    assert(completions == 3);

    assert(sdcard_beginReadBlocks(5, 3) == SDCARD_OPERATION_SUCCESS);
    finishRead(5, buffer);
    finishRead(9, buffer); // A nonconsecutive request must stop the old session.
    assert(sdcard.state == SDCARD_STATE_READY && !sdcard.multiBlocksRemain);
    assert(sdcard_beginReadBlocks(6, 3) == SDCARD_OPERATION_SUCCESS);
    finishRead(6, buffer);
    assert(sdcard_endReadBlocks() == SDCARD_OPERATION_SUCCESS);
    assert(sdcard_endReadBlocks() == SDCARD_OPERATION_SUCCESS);

    assert(sdcard_beginReadBlocks(7, 3) == SDCARD_OPERATION_SUCCESS);
    memset(buffer, 0xA6, sizeof(buffer));
    assert(sdcard_readBlock(7, buffer, readDone, 73));
    unsigned completedBefore = completions;
    sdcard_reset();
    assert(!sdcard_isReadBlockInSequence(7));
    assert(sdcard_poll() && completions == completedBefore);
    for (unsigned index = 0; index < sizeof(buffer); index++) {
        assert(buffer[index] == 0xA6);
    }
    // Read hints never pre-erase or write the announced range, even after reset.
    for (unsigned sector = 0; sector < 32; sector++) {
        finishRead(sector, buffer);
    }

    assert(sdcard_beginReadBlocks(10, 3) == SDCARD_OPERATION_SUCCESS);
    finishRead(10, buffer);
    memset(buffer, 0xD8, sizeof(buffer));
    assert(sdcard_writeBlock(20, buffer, NULL, 0) == SDCARD_OPERATION_IN_PROGRESS);
    while (!sdcard_poll()) {
    }
    assert(!sdcard.multiBlocksRemain && sdcard.state == SDCARD_STATE_READY);
    assert(sdcard_beginWriteBlocks(24, 2) == SDCARD_OPERATION_SUCCESS);
    assert(sdcard_beginReadBlocks(0, 2) == SDCARD_OPERATION_SUCCESS);
    finishRead(0, buffer);
    finishRead(1, buffer);
    assert(sdcard_endWriteBlocks() == SDCARD_OPERATION_SUCCESS);
    assert(sdcard_beginReadBlocks(2, 3) == SDCARD_OPERATION_SUCCESS);
    sdcard_sim_destroy();
    assert(sdcard_sim_init(arguments[1]));
    assert(!sdcard.multiBlocksRemain && sdcard.state == SDCARD_STATE_READY);
    finishRead(2, buffer);
    assert(sdcard.state == SDCARD_STATE_READY);
    sdcard_sim_destroy();
    puts(
        "Simulator multi-read: bounds, reuse, early stop, nonconsecutive IO, write transitions, reset ownership and no pre-erase passed.");
    return 0;
}
