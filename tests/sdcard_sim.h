#include "sdcard.h"

bool sdcard_sim_init(const char *filename);
void sdcard_sim_destroy();
bool sdcard_sim_isReady();

#ifdef SDCARD_SIM_TRACK_BATCHES
typedef struct sdcardSimStats_t {
    uint32_t reads;
    uint32_t writes;
    uint32_t multiSessions;
    uint32_t multiBlocks;
    uint32_t readInterrupts;
    uint32_t addressInterrupts;
} sdcardSimStats_t;

void sdcard_sim_getStats(sdcardSimStats_t *stats);
#endif
