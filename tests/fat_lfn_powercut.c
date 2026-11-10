// Each replay/remount is a fresh process. Reuse the original deterministic tracing backend.
#define AFATFS_TRACE_BACKEND_ONLY
#include "test_exfat_powercut.c"

static const char *cutSource = "Source long filename with important data.bin";
static char cutDestination[256];
static const char *cutCreated = "Created long filename spanning directory sectors.bin";

static void cutWaitRequest(void)
{
    waitNativeApi(AFATFS_ERROR_NONE);
}

static void cutRun(const char *scenario)
{
    if (!strcmp(scenario, "create") || !strcmp(scenario, "create-data")) {
        afatfsFilePtr_t file = openFile(cutCreated, "x+");
        CHECK(file);
        if (!strcmp(scenario, "create-data")) {
            writePattern(file, 1703, 73);
        }
        closeFile(file);
    } else {
        completed = false;
        if (!strncmp(scenario, "rename", 6)) {
            CHECK(afatfs_rename(&nativeRequest, cutSource, cutDestination, nativeApiDone, &context));
        } else {
            CHECK(!strcmp(scenario, "delete") || !strcmp(scenario, "delete-empty"));
            CHECK(afatfs_unlink(&nativeRequest, cutSource, nativeApiDone, &context));
        }
        cutWaitRequest();
    }
}

static int cutExists(const char *name)
{
    afatfsFilePtr_t file = openFile(name, "r");
    if (!file) {
        return -1;
    }
    uint32_t size;
    CHECK(afatfs_fGetSize(file, &size));
    if (!strcmp(name, cutSource) || !strcmp(name, cutDestination)) {
        readPattern(file, size, 31);
    }
    closeFile(file);
    return (int)size;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(argc == 6);
    if (strstr(argv[3], "same")) {
        strcpy(cutDestination, "Destination renamed long filename with important data.bin");
    } else {
        memset(cutDestination, 'R', 251);
        memcpy(cutDestination + 251, ".bin", 5);
    }
    CHECK(sdcard_sim_init(argv[1]));
    traceFile = fopen(argv[4], "wb");
    CHECK(traceFile);
    mountImage();
    unsigned mountWrites = completedWrites;
    afatfsError_e mountError = afatfs_getLastError();
    verifyFile("KEEP.BIN", 4096, 9);
    verifyFile("SURVIVE.BIN", 1701, 67);
    if (!strcmp(argv[2], "run")) {
        cutRun(argv[3]);
    } else {
        CHECK(!strcmp(argv[2], "probe") || !strcmp(argv[2], "prepare") || !strcmp(argv[2], "guard"));
    }
    unsigned operationWrites = completedWrites;
    int source = -1;
    int destination = -1;
    int created = -1;
    if (strcmp(argv[2], "guard")) {
        source = cutExists(cutSource);
        destination = cutExists(cutDestination);
        created = cutExists(cutCreated);
    }
    bool readOnly = afatfs_isReadOnly();
    drain();
    unmountImage();
    CHECK(afatfs_canPowerOff());
    sdcard_sim_destroy();
    CHECK(!fclose(traceFile));
    FILE *output = fopen(argv[5], "w");
    CHECK(output);
    fprintf(output,
        "{\"writes\":%u,\"mountWrites\":%u,\"mountError\":%u,\"operation\":%u,\"source\":%d,\"destination\":%d,\"created\":%d,\"readOnly\":%s}\n",
        completedWrites, mountWrites, (unsigned)mountError, operationWrites, source, destination, created,
        readOnly ? "true" : "false");
    CHECK(!fclose(output));
    return 0;
}
