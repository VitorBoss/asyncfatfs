#pragma once

#include "asyncfatfs.h"

bool writeLogTestEntries(afatfsFilePtr_t file, uint32_t *entryIndex, uint32_t targetEntries);
bool validateLogTestEntries(afatfsFilePtr_t file, uint32_t *entryIndex, uint32_t targetEntries);

void testAssert(bool condition, const char *errorMessage);
// Poll from one test stage until the close callback confirms synchronization.
bool closeFileAndWait(afatfsFilePtr_t file);

#define TEST_LOG_ENTRY_SIZE 16
