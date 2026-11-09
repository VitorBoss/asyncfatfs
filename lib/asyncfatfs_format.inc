/* Explicit-range quick format/partition operations, driven one sector at a time.
 * Only available while unmounted; cache[0] is the DMA buffer, never an extra sector array. */
static void afatfs_formatPut32(uint8_t *buffer, uint32_t value)
{
    for (unsigned n = 0; n < 4; n++) {
        buffer[n] = (uint8_t)(value >> (8 * n));
    }
}

static bool afatfs_formatBegin(
    afatfsRequest_t *request, unsigned operation, const void *options, afatfsResultCallback_t callback, void *user)
{
    if (!request || !options || afatfs_request ||
        (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_STOPPED &&
            afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_UNKNOWN)) {
        afatfs.lastError = AFATFS_ERROR_BUSY;
        return false;
    }
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_INITIALIZATION);
    memset(request, 0, sizeof(*request));
    request->operation = (uint8_t)operation;
    request->output = (void *)options;
    request->callback = callback;
    request->user = user;
    request->checkpoint = true;
    afatfs_request = request;
    return true;
}

bool afatfs_mkfs(
    afatfsRequest_t *request, const afatfsFormatOptions_t *options, afatfsResultCallback_t callback, void *user)
{
    if (!options || !options->startSector || options->sectorCount < 128 ||
        options->sectorCount > UINT32_MAX - options->startSector ||
        (options->type != FAT_FILESYSTEM_TYPE_FAT16 && options->type != FAT_FILESYSTEM_TYPE_FAT32
#ifdef ASYNCFS_ENABLE_EXFAT
            && options->type != FAT_FILESYSTEM_TYPE_EXFAT
#endif
            )) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        return false;
    }
    uint32_t spc = options->sectorsPerCluster ? options->sectorsPerCluster : 1;
    uint32_t fatSectors = 0;
    uint32_t clusters = 0;
    uint32_t heap = 0;
    for (;;) {
        if (!isPowerOfTwo(spc) || spc > (options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 65536U : 128U)) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        uint32_t reserved = options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 24
            : options->type == FAT_FILESYSTEM_TYPE_FAT32               ? 32
                                                                       : 1;
        uint32_t fats = options->type == FAT_FILESYSTEM_TYPE_EXFAT ? 1 : 2;
        uint32_t root = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 32 : 0;
        fatSectors = (uint32_t)((((uint64_t)options->sectorCount / spc + 2) *
                                        (options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4) +
                                    511) /
            512);
        uint64_t start = (uint64_t)reserved + (uint64_t)fats * fatSectors + root;
        if (options->type == FAT_FILESYSTEM_TYPE_EXFAT) {
            start = (start + spc - 1) / spc * spc;
        }
        if (start >= options->sectorCount) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        heap = (uint32_t)start;
        clusters = (options->sectorCount - heap) / spc;
        uint32_t maximum = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT16_MAX_CLUSTERS
            : options->type == FAT_FILESYSTEM_TYPE_FAT32              ? 0x0FFFFFF5U
                                                                      : 0xFFFFFFF5U;
        uint32_t minimum = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT12_MAX_CLUSTERS + 1
            : options->type == FAT_FILESYSTEM_TYPE_FAT32              ? FAT16_MAX_CLUSTERS + 1
                                                                      : 256;
        if (clusters <= maximum && clusters >= minimum) {
            break;
        }
        if (options->sectorsPerCluster || clusters < minimum) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        spc *= 2;
    }
    if (!afatfs_formatBegin(request, AFATFS_API_MKFS, options, callback, user)) {
        return false;
    }
    request->value[0] = options->sectorCount;
    request->value[1] = spc;
    request->value[2] = fatSectors;
    request->value[3] = heap;
    request->value[4] = clusters;
    request->value[12] = options->type;
    if (options->type == FAT_FILESYSTEM_TYPE_EXFAT) {
        request->value[5] = (uint32_t)(((uint64_t)clusters + 7) / 8 + (uint64_t)spc * 512 - 1) / (spc * 512);
        request->value[6] = 2 + request->value[5];
        request->value[7] = request->value[6] + 1;
        request->value[11] = request->value[5] + 2;
    } else {
        request->value[7] = options->type == FAT_FILESYSTEM_TYPE_FAT32 ? 2 : 0;
    }
    return true;
}

bool afatfs_fdisk(
    afatfsRequest_t *request, const afatfsPartitionOptions_t *options, afatfsResultCallback_t callback, void *user)
{
    if (!options || options->sectorCount < (options->gpt ? 68U : 2U)) {
        afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
        return false;
    }
#ifndef ASYNCFS_ENABLE_EXFAT
    if (options->gpt) {
        afatfs.lastError = AFATFS_ERROR_UNSUPPORTED;
        return false;
    }
#endif
    if (options->gpt) {
        bool nonzero = false;
        for (unsigned n = 0; n < 16; n++) {
            nonzero |= options->diskGuid[n] != 0;
        }
        if (!nonzero) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
    }
    for (unsigned n = 0; n < 4; n++) {
        if (!options->length[n]) {
            continue;
        }
        uint64_t end = (uint64_t)options->start[n] + options->length[n];
        if (options->start[n] < (options->gpt ? 34U : 1U) || end > options->sectorCount - (options->gpt ? 33U : 0U) ||
            (!options->gpt && !options->type[n])) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
            return false;
        }
        for (unsigned m = 0; m < n; m++) {
            if (options->length[m] && options->start[n] < (uint64_t)options->start[m] + options->length[m] &&
                options->start[m] < end) {
                afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER;
                return false;
            }
        }
    }
    return afatfs_formatBegin(request, AFATFS_API_FDISK, options, callback, user);
}

static void afatfs_formatUpcase(uint8_t *buffer)
{
    memset(buffer, 0, 512);
    for (unsigned n = 0; n < 128; n++) {
        buffer[n * 2] = (uint8_t)afatfs_apiUpper(n);
    }
    buffer[256] = buffer[257] = 0xFF;
    buffer[258] = 0x7F;
    buffer[259] = 0xFF;
    buffer[260] = buffer[261] = 0xFF;
}

static void afatfs_formatVolumeSector(afatfsRequest_t *request, uint32_t relative, uint8_t *buffer)
{
    const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
    uint32_t spc = request->value[1];
    uint32_t fatSectors = request->value[2];
    uint32_t heap = request->value[3];
    uint32_t count = request->value[4];
    bool exfat = options->type == FAT_FILESYSTEM_TYPE_EXFAT;
    bool fat32 = options->type == FAT_FILESYSTEM_TYPE_FAT32;
    memset(buffer, 0, 512);
    if ((exfat && relative < 24) || relative == 0 || (fat32 && relative == 6)) {
        uint32_t boot = exfat ? relative % 12 : 0;
        if (boot == 0) {
            fatVolumeID_t *volume = (fatVolumeID_t *)buffer;
            volume->jmpBoot[0] = 0xEB;
            volume->jmpBoot[1] = exfat ? 0x76 : fat32 ? 0x58 : 0x3C;
            volume->jmpBoot[2] = 0x90;
            memcpy(volume->oemName, exfat ? "EXFAT   " : "ASYNCFS ", 8);
            if (exfat) {
                volume->exFat.partitionOffset = options->startSector;
                volume->exFat.volumeLength = options->sectorCount;
                volume->exFat.fatOffset = 24;
                volume->exFat.fatLength = fatSectors;
                volume->exFat.clusterHeapOffset = heap;
                volume->exFat.clusterCount = count;
                volume->exFat.rootDirectoryCluster = request->value[7];
                volume->exFat.volumeSerialNumber = options->serial;
                volume->exFat.fileSystemRevision = 0x100;
                volume->exFat.bytesPerSectorShift = 9;
                for (uint32_t n = spc; n > 1; n >>= 1) {
                    volume->exFat.sectorsPerClusterShift++;
                }
                volume->exFat.numFATs = 1;
                volume->exFat.driveSelect = 0x80;
                volume->exFat.percentInUse = (uint8_t)((uint64_t)request->value[11] * 100 / count);
            } else {
                volume->fat.bytesPerSector = 512;
                volume->fat.sectorsPerCluster = (uint8_t)spc;
                volume->fat.reservedSectorCount = fat32 ? 32 : 1;
                volume->fat.numFATs = 2;
                volume->fat.rootEntryCount = fat32 ? 0 : 512;
                volume->fat.media = 0xF8;
                volume->fat.hiddenSectors = options->startSector;
                volume->fat.sectorsPerTrack = 32;
                volume->fat.numHeads = 64;
                if (options->sectorCount < 65536) {
                    volume->fat.totalSectors16 = (uint16_t)options->sectorCount;
                } else {
                    volume->fat.totalSectors32 = options->sectorCount;
                }
                if (fat32) {
                    fat32Descriptor_t *descriptor = &volume->fat.fatDescriptor.fat32;
                    descriptor->FATSize32 = fatSectors;
                    descriptor->rootCluster = 2;
                    descriptor->fsInfo = 1;
                    descriptor->backupBootSector = 6;
                    descriptor->driveNumber = 0x80;
                    descriptor->bootSignature = 0x29;
                    descriptor->volumeID = options->serial;
                    memcpy(descriptor->volumeLabel, "NO NAME    ", 11);
                    memcpy(descriptor->fileSystemType, "FAT32   ", 8);
                } else {
                    fat16Descriptor_t *descriptor = &volume->fat.fatDescriptor.fat16;
                    volume->fat.FATSize16 = (uint16_t)fatSectors;
                    descriptor->driveNumber = 0x80;
                    descriptor->bootSignature = 0x29;
                    descriptor->volumeID = options->serial;
                    memcpy(descriptor->volumeLabel, "NO NAME    ", 11);
                    memcpy(descriptor->fileSystemType, "FAT16   ", 8);
                }
            }
            buffer[510] = 0x55;
            buffer[511] = 0xAA;
        } else if (boot <= 8) {
            afatfs_formatPut32(buffer + 508, 0xAA550000);
        } else if (boot == 11) {
            for (unsigned n = 0; n < 512; n += 4) {
                afatfs_formatPut32(buffer + n, request->value[9]);
            }
        }
        return;
    }
    if (fat32 && (relative == 1 || relative == 7)) {
        afatfs_formatPut32(buffer, 0x41615252);
        afatfs_formatPut32(buffer + 484, 0x61417272);
        afatfs_formatPut32(buffer + 488, count - 1);
        afatfs_formatPut32(buffer + 492, 3);
        afatfs_formatPut32(buffer + 508, 0xAA550000);
        return;
    }
    uint32_t fatStart = exfat ? 24 : fat32 ? 32 : 1;
    if (relative >= fatStart && relative - fatStart < fatSectors * (exfat ? 1U : 2U)) {
        uint32_t width = options->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4;
        uint32_t base = (relative - fatStart) % fatSectors * (512 / width);
        for (unsigned n = 0; n < 512 / width; n++) {
            uint32_t cluster = base + n;
            uint32_t value = 0;
            if (cluster == 0) {
                value = width == 2 ? 0xFFF8 : 0xFFFFFFF8;
            } else if (cluster == 1 || (fat32 && cluster == 2)) {
                value = 0xFFFFFFFF;
            } else if (exfat && cluster >= 2 && cluster < 2 + request->value[5]) {
                value = cluster + 1 == 2 + request->value[5] ? 0xFFFFFFFF : cluster + 1;
            } else if (exfat && (cluster == request->value[6] || cluster == request->value[7])) {
                value = 0xFFFFFFFF;
            }
            if (width == 4) {
                afatfs_formatPut32(buffer + n * 4, value);
            } else {
                buffer[n * 2] = (uint8_t)value;
                buffer[n * 2 + 1] = (uint8_t)(value >> 8);
            }
        }
        return;
    }
    if (!exfat || relative < heap) {
        return;
    }
    uint32_t dataSector = relative - heap;
    if (dataSector < request->value[5] * spc) {
        uint32_t byteBase = dataSector * 512;
        for (unsigned n = 0; n < 512; n++) {
            uint64_t bit = (uint64_t)(byteBase + n) * 8;
            for (unsigned b = 0; b < 8; b++) {
                if (bit + b < request->value[11] || bit + b >= count) {
                    buffer[n] |= (uint8_t)(1U << b);
                }
            }
        }
    } else if (dataSector == (request->value[6] - 2) * spc) {
        afatfs_formatUpcase(buffer);
    } else if (dataSector == (request->value[7] - 2) * spc) {
        exFatUpTableEntry_t *bitmap = (exFatUpTableEntry_t *)buffer;
        exFatUpTableEntry_t *upcase = bitmap + 1;
        bitmap->type = EXFAT_DIR_BITMAP;
        bitmap->firstCluster = 2;
        bitmap->dataSize = ((uint64_t)count + 7) / 8;
        upcase->type = EXFAT_DIR_UPCASE;
        upcase->firstCluster = request->value[6];
        upcase->dataSize = 262;
        upcase->tableChecksum = request->value[10];
    }
}

static void afatfs_formatPartitionSector(afatfsRequest_t *request, uint32_t sector, uint8_t *buffer)
{
    const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
    memset(buffer, 0, 512);
    if (!sector) {
        mbrPartitionEntry_t *entries = (mbrPartitionEntry_t *)(buffer + 446);
        if (options->gpt) {
            entries[0].type = MBR_PARTITION_TYPE_GPT;
            entries[0].lbaBegin = 1;
            entries[0].numSectors = options->sectorCount - 1;
            entries[0].chsBegin[1] = 2;
            memset(entries[0].chsEnd, 0xFF, 3);
        } else {
            for (unsigned n = 0; n < 4; n++) {
                if (options->length[n]) {
                    entries[n].type = options->type[n];
                    entries[n].lbaBegin = options->start[n];
                    entries[n].numSectors = options->length[n];
                    memset(entries[n].chsBegin, 0xFF, 3);
                    memset(entries[n].chsEnd, 0xFF, 3);
                }
            }
        }
        buffer[510] = 0x55;
        buffer[511] = 0xAA;
        return;
    }
    if (!options->gpt) {
        return;
    }
#ifdef ASYNCFS_ENABLE_EXFAT
    if (sector == 1 || sector == options->sectorCount - 1) {
        memcpy(buffer, "EFI PART", 8);
        afatfs_formatPut32(buffer + 8, 0x10000);
        afatfs_formatPut32(buffer + 12, 92);
        afatfs_formatPut32(buffer + 24, sector);
        afatfs_formatPut32(buffer + 32, sector == 1 ? options->sectorCount - 1 : 1);
        afatfs_formatPut32(buffer + 40, 34);
        afatfs_formatPut32(buffer + 48, options->sectorCount - 34);
        memcpy(buffer + 56, options->diskGuid, 16);
        afatfs_formatPut32(buffer + 72, sector == 1 ? 2 : options->sectorCount - 33);
        afatfs_formatPut32(buffer + 80, 128);
        afatfs_formatPut32(buffer + 84, 128);
        afatfs_formatPut32(buffer + 88, request->value[9]);
        afatfs_formatPut32(buffer + 16, ~afatfs_crc32Update(UINT32_MAX, buffer, 92));
    } else if (sector == 2 || sector == options->sectorCount - 33) {
        static const uint8_t basic[16] = {
            0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
        for (unsigned n = 0; n < 4; n++) {
            if (options->length[n]) {
                uint8_t *entry = buffer + n * 128;
                memcpy(entry, basic, 16);
                memcpy(entry + 16, options->diskGuid, 16);
                entry[31] ^= (uint8_t)(n + 1);
                entry[16] |= 1;
                afatfs_formatPut32(entry + 32, options->start[n]);
                afatfs_formatPut32(entry + 40, options->start[n] + options->length[n] - 1);
            }
        }
    }
#else
    (void)request;
#endif
}

static void afatfs_formatComplete(
    sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    (void)operation;
    (void)sector;
    if (generation != afatfs_mediaGeneration || !afatfs_request) {
        return;
    }
    afatfs_request->waiting = false;
    afatfs_request->value[15] = buffer ? 1 : 2;
    if (!buffer) {
        afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
    }
}
static void afatfs_apiFormat(afatfsRequest_t *request)
{
    if (request->waiting) {
        return;
    }
    uint8_t *buffer = afatfs.cache;
    bool disk = request->operation == AFATFS_API_FDISK;
    if (!request->phase) {
        if (disk) {
            const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (options->gpt) {
                if (!request->value[13]) {
                    request->value[9] = UINT32_MAX;
                }
                afatfs_formatPartitionSector(request, 2 + request->value[13], buffer);
                request->value[9] = afatfs_crc32Update(request->value[9], buffer, 512);
                if (++request->value[13] < 32) {
                    return;
                }
                request->value[9] = ~request->value[9];
            }
#else
            (void)options;
#endif
        } else if (request->value[12] == FAT_FILESYSTEM_TYPE_EXFAT) {
            afatfs_formatUpcase(buffer);
            for (unsigned n = 0; n < 262; n++) {
                request->value[10] = (request->value[10] >> 1 | request->value[10] << 31) + buffer[n];
            }
            for (unsigned page = 0; page < 11; page++) {
                afatfs_formatVolumeSector(request, page, buffer);
                for (unsigned n = 0; n < 512; n++) {
                    if (page || (n != 106 && n != 107 && n != 112)) {
                        request->value[9] = (request->value[9] >> 1 | request->value[9] << 31) + buffer[n];
                    }
                }
            }
        }
        request->phase = 1;
    }
    uint32_t last;
    uint32_t sector;
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        last = options->gpt ? 67 : 3;
        sector = request->value[8] < 34 ? request->value[8] : options->sectorCount - 33 + request->value[8] - 34;
    } else {
        const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
        last = request->value[3] +
            (options->type == FAT_FILESYSTEM_TYPE_EXFAT          ? request->value[11] * request->value[1]
                    : options->type == FAT_FILESYSTEM_TYPE_FAT32 ? request->value[1]
                                                                 : 0);
        sector = options->startSector + request->value[8];
    }
    if (request->value[15] == 1) {
        request->value[8]++;
        request->value[15] = 0;
    }
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        // Clear stale GPT signatures when switching to MBR, without touching any new partition's data.
        while (!options->gpt && request->value[8] > 0 && request->value[8] < last) {
            uint32_t probe = request->value[8] == 1 ? 1 : options->sectorCount - 1;
            bool occupied = request->value[8] == 2 && options->sectorCount == 2;
            for (unsigned n = 0; n < 4; n++) {
                occupied |= options->length[n] && probe >= options->start[n] &&
                    (uint64_t)probe < (uint64_t)options->start[n] + options->length[n];
            }
            if (!occupied) {
                break;
            }
            request->value[8]++;
        }
    }
    if (request->value[8] >= last) {
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
        sdcardOperationStatus_e stopped = sdcard_endWriteBlocks();
        if (stopped != SDCARD_OPERATION_SUCCESS) {
            if (stopped == SDCARD_OPERATION_FAILURE) {
                afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            }
            return;
        }
#endif
        if (!sdcard_poll()) {
            return;
        }
        afatfsResultCallback_t callback = request->callback;
        void *user = request->user;
        afatfs_request = NULL;
        afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_STOPPED);
        request->complete = true;
        request->result = AFATFS_ERROR_NONE;
        request->driving = false;
        if (callback) {
            callback(AFATFS_ERROR_NONE, user);
        }
        return;
    }
    // Recompute after advancing an acknowledged sector. The buffer remains untouched while DMA owns it.
    if (disk) {
        const afatfsPartitionOptions_t *options = (const afatfsPartitionOptions_t *)request->output;
        sector = options->gpt
            ? (request->value[8] < 34 ? request->value[8] : options->sectorCount - 33 + request->value[8] - 34)
            : request->value[8] == 2 ? options->sectorCount - 1
                                     : request->value[8];
        afatfs_formatPartitionSector(request, sector, buffer);
    } else {
        const afatfsFormatOptions_t *options = (const afatfsFormatOptions_t *)request->output;
        sector = options->startSector + request->value[8];
        afatfs_formatVolumeSector(request, request->value[8], buffer);
    }
    request->value[15] = 0;
    switch (sdcard_writeBlock(sector, buffer, afatfs_formatComplete, afatfs_mediaGeneration)) {
        case SDCARD_OPERATION_IN_PROGRESS:
            request->waiting = true;
            break;
        case SDCARD_OPERATION_SUCCESS:
            request->value[15] = 1;
            break;
        case SDCARD_OPERATION_FAILURE:
            afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            break;
        default:
            break;
    }
}
