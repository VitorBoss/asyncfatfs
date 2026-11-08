// Run the library against the actual file-backed simulator, not a mock backend.
#include <assert.h>
#include "asyncfatfs.h"
#include "sdcard_sim.c"

#define INTEGRATION_BYTES (32U * 1024U)
static afatfsFilePtr_t openedFile;
static bool finished;
static bool sawReadSession;

static void pollIntegration(void)
{
    afatfs_poll();
    assert(afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_FATAL);
    sawReadSession |= sdcard.state == SDCARD_STATE_READING_MULTIPLE_BLOCKS ||
        (sdcard.state == SDCARD_STATE_READING && sdcard.multiBlocksRemain != 0);
}

static void openedIntegration(afatfsFilePtr_t file, void *context)
{
    (void)context;
    assert(file);
    openedFile = file;
    finished = true;
}

static void closedIntegration(void *context)
{
    (void)context;
    finished = true;
}

static afatfsFilePtr_t openIntegration(const char *name, const char *mode)
{
    finished = false;
    assert(afatfs_fopen(name, mode, openedIntegration, NULL));
    for (unsigned retry = 0; !finished; retry++) {
        assert(retry < 1000000);
        pollIntegration();
    }
    return openedFile;
}

static void closeIntegration(afatfsFilePtr_t file)
{
    finished = false;
    assert(afatfs_fclose(file, closedIntegration, NULL));
    for (unsigned retry = 0; !finished; retry++) {
        assert(retry < 1000000);
        pollIntegration();
    }
    assert(sdcard.state != SDCARD_STATE_READING_MULTIPLE_BLOCKS);
}

static void mountIntegration(void)
{
    afatfs_init();
    for (unsigned retry = 0; afatfs_getFilesystemState() != AFATFS_FILESYSTEM_STATE_READY; retry++) {
        assert(retry < 1000000);
        pollIntegration();
    }
}

static void unmountIntegration(void)
{
    for (unsigned retry = 0; !afatfs_destroy(false); retry++) {
        assert(retry < 1000000);
        pollIntegration();
    }
    assert(afatfs_canPowerOff() && !sdcard.multiBlocksRemain);
}

static void store16(uint8_t *buffer, unsigned offset, uint16_t value)
{
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1] = (uint8_t)(value >> 8);
}

static void store32(uint8_t *buffer, unsigned offset, uint32_t value)
{
    for (unsigned index = 0; index < 4; index++) {
        buffer[offset + index] = (uint8_t)(value >> (index * 8));
    }
}

static void formatIntegration(const char *path)
{
    FILE *image = fopen(path, "wb");
    assert(image);
    uint8_t buffer[512] = {0};
    for (unsigned index = 0; index < 66558; index++) {
        assert(fwrite(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
    }
    buffer[450] = 0x0B;
    store32(buffer, 454, 1);
    store32(buffer, 458, 66557);
    buffer[510] = 0x55;
    buffer[511] = 0xAA;
    assert(!fseek(image, 0, SEEK_SET));
    assert(fwrite(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0xEB;
    buffer[1] = 0x58;
    buffer[2] = 0x90;
    store16(buffer, 11, 512);
    buffer[13] = 1;
    store16(buffer, 14, 1);
    buffer[16] = 2;
    buffer[21] = 0xF8;
    store32(buffer, 32, 66557);
    store32(buffer, 36, 513);
    store32(buffer, 44, 2);
    store16(buffer, 48, 0xFFFF);
    store16(buffer, 50, 0xFFFF);
    buffer[510] = 0x55;
    buffer[511] = 0xAA;
    assert(fwrite(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
    memset(buffer, 0, sizeof(buffer));
    store32(buffer, 0, 0x0FFFFFF8);
    store32(buffer, 4, 0x0FFFFFFF);
    store32(buffer, 8, 0x0FFFFFFF);
    for (unsigned copy = 0; copy < 2; copy++) {
        assert(!fseek(image, (2 + copy * 513) * 512, SEEK_SET));
        assert(fwrite(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
    }
    assert(!fclose(image));
}

static void verifyIntegration(afatfsFilePtr_t file, uint32_t *offset, unsigned salt)
{
    uint8_t buffer[257];
    unsigned count = INTEGRATION_BYTES - *offset < sizeof(buffer) ? INTEGRATION_BYTES - *offset : sizeof(buffer);
    unsigned received = afatfs_fread(file, buffer, count);
    for (unsigned index = 0; index < received; index++) {
        assert(buffer[index] == (uint8_t)(*offset + index + salt));
    }
    *offset += received;
}

int main(int count, char **arguments)
{
    assert(count == 2);
    formatIntegration(arguments[1]);
    assert(sdcard_sim_init(arguments[1]));
    mountIntegration();
    const char *names[] = {"SIMONE.BIN", "SIMTWO.BIN"};
    uint8_t buffer[512];
    for (unsigned index = 0; index < 2; index++) {
        afatfsFilePtr_t file = openIntegration(names[index], "w");
        for (unsigned offset = 0; offset < INTEGRATION_BYTES;) {
            for (unsigned position = 0; position < sizeof(buffer); position++) {
                buffer[position] = (uint8_t)(offset + position + index * 37);
            }
            offset += afatfs_fwrite(file, buffer, sizeof(buffer));
            pollIntegration();
        }
        closeIntegration(file);
    }
    unmountIntegration();
    sdcard_sim_destroy();
    assert(sdcard_sim_init(arguments[1]));
    mountIntegration();
    afatfsFilePtr_t first = openIntegration(names[0], "r");
    uint32_t firstOffset = 0;
    sawReadSession = false;
    for (unsigned retry = 0; firstOffset < INTEGRATION_BYTES; retry++) {
        assert(retry < 1000000);
        verifyIntegration(first, &firstOffset, 0);
        pollIntegration();
    }
    assert(sawReadSession);
    closeIntegration(first);
    first = openIntegration(names[0], "r");
    afatfsFilePtr_t second = openIntegration(names[1], "r");
    firstOffset = 0;
    uint32_t secondOffset = 0;
    sawReadSession = false;
    for (unsigned retry = 0; firstOffset < INTEGRATION_BYTES || secondOffset < INTEGRATION_BYTES; retry++) {
        assert(retry < 1000000);
        verifyIntegration(first, &firstOffset, 0);
        verifyIntegration(second, &secondOffset, 37);
        pollIntegration();
    }
    assert(!sawReadSession);
    closeIntegration(first);
    closeIntegration(second);
    unmountIntegration();
    sdcard_sim_destroy();
    FILE *image = fopen(arguments[1], "rb");
    assert(image);
    uint8_t mirror[512];
    for (unsigned sector = 0; sector < 513; sector++) {
        assert(!fseek(image, (2 + sector) * 512, SEEK_SET));
        assert(fread(buffer, 1, sizeof(buffer), image) == sizeof(buffer));
        assert(!fseek(image, (515 + sector) * 512, SEEK_SET));
        assert(fread(mirror, 1, sizeof(mirror), image) == sizeof(mirror));
        assert(!memcmp(buffer, mirror, sizeof(buffer)));
    }
    assert(!fclose(image));
    puts(
        "Library + shipped simulator: write/remount, one-reader sessions, two-reader fallback, exact 257-byte payload, equal FATs and clean power-off passed.");
    return 0;
}
