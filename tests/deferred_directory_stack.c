#define main original_filesystem_regression_main
#include "test_exfat.c"
#undef main
#include "../lib/asyncfatfs.c"

static bool extensionCompleted;

static void extensionComplete(afatfsFilePtr_t directory, void *user)
{
    assert(directory == &afatfs.currentDirectory);
    assert(user == &extensionCompleted);
    assert(directory->operation.active == AFATFS_FILE_OPERATION_NONE);
    extensionCompleted = true;
}

int main(void)
{
    exerciseFat(true, 1);
    mount();
    afatfsFilePtr_t directory = &afatfs.currentDirectory;
    afatfsFinder_t finder;
    fatDirectoryEntry_t *entry;
    afatfs_findFirst(directory, &finder);
    for (unsigned attempts = 0;; attempts++) {
        assert(attempts < 200000);
        afatfsOperationStatus_e status = afatfs_findNext(directory, &finder, &entry);
        assert(status != AFATFS_OPERATION_FAILURE);
        if (status == AFATFS_OPERATION_SUCCESS && !entry) {
            break;
        }
        pump();
    }
    afatfs_findLast(directory);
    while (!afatfs_flush()) {
        pump();
    }
    assert(!io.pending);
    unsigned savedReads = reads;
    unsigned savedWrites = submittedWrites;
    uint32_t originalSize = directory->physicalSize;
    assert(afatfs_extendSubdirectory(directory, NULL, extensionComplete, &extensionCompleted) ==
        AFATFS_OPERATION_IN_PROGRESS);
    assert(directory->operation.active == AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY);
    assert(directory->operation.state.extendSubdirectory.phase == AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL);
    assert(directory->physicalSize == originalSize);
    assert(reads == savedReads && submittedWrites == savedWrites && !io.pending && !extensionCompleted);
    assert(afatfs_extendSubdirectory(directory, NULL, NULL, NULL) == AFATFS_OPERATION_FAILURE);
    for (unsigned attempts = 0; !extensionCompleted; attempts++) {
        assert(attempts < 200000);
        pump();
    }
    assert(directory->physicalSize == originalSize + SECTOR);
    uint32_t physicalSector = afatfs_fileGetCursorPhysicalSector(directory);
    unmount();
    for (unsigned index = 0; index < SECTOR; index++) {
        assert(media[physicalSector * SECTOR + index] == 0);
    }
    mount();
    unmount();
    puts("Deferred directory growth: no inline allocation, busy rejection, poll completion, zero-fill and remount passed.");
    return 0;
}
