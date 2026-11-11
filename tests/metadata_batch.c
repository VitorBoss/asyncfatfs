// Deterministic I/O benchmark with the same delayed backend used by regression.
#define main metadata_regression_main
#include "test_exfat.c"
#undef main

static void benchmarkFormatFat(void)
{
    memset(media, 0, sizeof(media));
    memset(&io, 0, sizeof(io));
    const unsigned count = 65530;
    const unsigned fatSectors = ((count + 2) * 4 + 511) / 512;
    mbrPartitionEntry_t *part = (mbrPartitionEntry_t *)(media + 446);
    part->type = MBR_PARTITION_TYPE_FAT32;
    part->lbaBegin = PARTITION;
    part->numSectors = 1 + fatSectors * 2 + count;
    media[510] = 0x55;
    media[511] = 0xAA;
    fatVolumeID_t *boot = (fatVolumeID_t *)(media + PARTITION * SECTOR);
    boot->jmpBoot[0] = 0xEB;
    boot->jmpBoot[1] = 0x3C;
    boot->jmpBoot[2] = 0x90;
    boot->fat.bytesPerSector = SECTOR;
    boot->fat.sectorsPerCluster = 1;
    boot->fat.reservedSectorCount = 1;
    boot->fat.numFATs = 2;
    boot->fat.totalSectors32 = part->numSectors;
    boot->fat.fatDescriptor.fat32.FATSize32 = fatSectors;
    boot->fat.fatDescriptor.fat32.rootCluster = 2;
    media[PARTITION * SECTOR + 510] = 0x55;
    media[PARTITION * SECTOR + 511] = 0xAA;
    for (unsigned index = 0; index < 2; index++) {
        uint8_t *table = media + (PARTITION + 1 + index * fatSectors) * SECTOR;
        put32(table, 0x0FFFFFF8);
        put32(table + 4, 0x0FFFFFFF);
        put32(table + 8, 0x0FFFFFFF);
    }
}

static void benchmarkRun(bool exfat, bool concurrent)
{
    if (exfat) {
        format(false);
    } else {
        benchmarkFormatFat();
    }
    unsigned setupReads = reads;
    unsigned setupWrites = writes;
    mount();
    setupReads = reads - setupReads;
    setupWrites = writes - setupWrites;
    afatfsFilePtr_t first = openFile("BENCHA.BIN", "w");
    afatfsFilePtr_t second = concurrent ? openFile("BENCHB.BIN", "w") : NULL;
    assert(first && (!concurrent || second));
    for (unsigned retry = 0; !afatfs_flush(); retry++) {
        assert(retry < 200000);
        pump();
    }
    unsigned startReads = reads;
    unsigned startWrites = writes;
    uint8_t payload[257];
    memset(payload, 0x59, sizeof(payload));
    for (unsigned offset = 0; offset < 65536;) {
        unsigned amount = sizeof(payload) < 65536 - offset ? (unsigned)sizeof(payload) : 65536 - offset;
        writeBytes(first, payload, amount);
        if (second) {
            writeBytes(second, payload, amount);
        }
        offset += amount;
    }
    closeFile(first);
    if (second) {
        closeFile(second);
    }
    unmount();
    unsigned measuredReads = reads - startReads;
    unsigned measuredWrites = writes - startWrites;
    mount();
    const char *names[] = {"BENCHA.BIN", "BENCHB.BIN"};
    for (unsigned index = 0; index < (concurrent ? 2U : 1U); index++) {
        afatfsFilePtr_t file = openFile(names[index], "r");
        assert(file);
        uint32_t size;
        assert(afatfs_fGetSize(file, &size) && size == 65536);
        uint8_t actual[512];
        for (unsigned offset = 0; offset < 65536; offset += sizeof(actual)) {
            readBytes(file, actual, sizeof(actual));
            for (unsigned position = 0; position < sizeof(actual); position++) {
                assert(actual[position] == 0x59);
            }
        }
        closeFile(file);
    }
    unmount();
    printf("BENCH,%s,%u,%u,%u,%u,%u,%u\n", exfat ? "exfat" : "fat32", concurrent ? 2U : 1U,
        concurrent ? 131072U : 65536U, measuredReads, measuredWrites, setupReads, setupWrites);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned mode = 0; mode < 2; mode++) {
        benchmarkRun(false, mode != 0);
#ifndef TEST_FAT_ONLY
        benchmarkRun(true, mode != 0);
#endif
    }
    return 0;
}
