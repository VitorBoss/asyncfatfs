// The minimal build must never advertise uncached FAT sectors for pre-erase.
#define main metadata_hint_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

int main(void)
{
    exerciseFat(true, 1);
    mount();
    afatfsFATSector_t primary;
    uint8_t *mirror;
    afatfsOperationStatus_e status;
    for (unsigned retry = 0;; retry++) {
        assert(retry < 10000);
        status = afatfs_FATWriteSector(
            400, &primary, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE,
            AFATFS_FAT_MIRROR_STATE(NULL) 3);
        if (status == AFATFS_OPERATION_SUCCESS) {
            break;
        }
        assert(status == AFATFS_OPERATION_IN_PROGRESS);
        pump();
    }
    assert(mirror && mirror != primary.bytes);
    assert(afatfs_getCacheDescriptorForBuffer(primary.bytes)->consecutiveEraseBlockCount == 0);
    assert(afatfs_getCacheDescriptorForBuffer(mirror)->consecutiveEraseBlockCount == 0);
    assert(afatfs_getCacheDescriptorForBuffer(primary.bytes)->retainCount == 0);
    memcpy(mirror, primary.bytes, SECTOR);
    unmount();
    puts("FAT metadata: separate pages, balanced retention and zero future pre-erase hints passed.");
    return 0;
}
