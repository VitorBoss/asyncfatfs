/* Explicit-range quick format/partition operations, driven one sector at a time.
 * Only available while unmounted; cache[0] is the DMA buffer, never an extra sector array. */
static void afatfs_formatPut32(uint8_t *p, uint32_t v)
{ for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(v >> (8 * n)); }

static bool afatfs_formatBegin(afatfsRequest_t *r, unsigned op, const void *options, afatfsResultCallback_t cb, void *user)
{
    if (!r || !options || afatfs_request || (afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_STOPPED
        && afatfs.fileSystemState != AFATFS_FILESYSTEM_STATE_UNKNOWN)) { afatfs.lastError = AFATFS_ERROR_BUSY; return false; }
    afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_INITIALIZATION);
    memset(r, 0, sizeof(*r)); r->operation = (uint8_t)op; r->output = (void *)options;
    r->callback = cb; r->user = user; r->checkpoint = true; afatfs_request = r;
    return true;
}

bool afatfs_mkfs(afatfsRequest_t *r, const afatfsFormatOptions_t *o, afatfsResultCallback_t cb, void *user)
{
    if (!o || !o->startSector || o->sectorCount < 128 || o->sectorCount > UINT32_MAX - o->startSector
        || (o->type != FAT_FILESYSTEM_TYPE_FAT16 && o->type != FAT_FILESYSTEM_TYPE_FAT32
#ifdef ASYNCFS_ENABLE_EXFAT
            && o->type != FAT_FILESYSTEM_TYPE_EXFAT
#endif
        )) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
    uint32_t spc = o->sectorsPerCluster ? o->sectorsPerCluster : 1;
    uint32_t fatSectors = 0, clusters = 0, heap = 0;
    for (;;) {
        if (!isPowerOfTwo(spc) || spc > (o->type == FAT_FILESYSTEM_TYPE_EXFAT ? 65536U : 128U)) {
            afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false;
        }
        uint32_t reserved = o->type == FAT_FILESYSTEM_TYPE_EXFAT ? 24 : o->type == FAT_FILESYSTEM_TYPE_FAT32 ? 32 : 1;
        uint32_t fats = o->type == FAT_FILESYSTEM_TYPE_EXFAT ? 1 : 2;
        uint32_t root = o->type == FAT_FILESYSTEM_TYPE_FAT16 ? 32 : 0;
        fatSectors = (uint32_t)((((uint64_t)o->sectorCount / spc + 2) * (o->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4) + 511) / 512);
        uint64_t start = (uint64_t)reserved + (uint64_t)fats * fatSectors + root;
        if (o->type == FAT_FILESYSTEM_TYPE_EXFAT) start = (start + spc - 1) / spc * spc;
        if (start >= o->sectorCount) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
        heap = (uint32_t)start; clusters = (o->sectorCount - heap) / spc;
        uint32_t maximum = o->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT16_MAX_CLUSTERS : o->type == FAT_FILESYSTEM_TYPE_FAT32 ? 0x0FFFFFF5U : 0xFFFFFFF5U;
        uint32_t minimum = o->type == FAT_FILESYSTEM_TYPE_FAT16 ? FAT12_MAX_CLUSTERS + 1 : o->type == FAT_FILESYSTEM_TYPE_FAT32 ? FAT16_MAX_CLUSTERS + 1 : 256;
        if (clusters <= maximum && clusters >= minimum) break;
        if (o->sectorsPerCluster || clusters < minimum) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
        spc *= 2;
    }
    if (!afatfs_formatBegin(r, AFATFS_API_MKFS, o, cb, user)) return false;
    r->value[0] = o->sectorCount; r->value[1] = spc; r->value[2] = fatSectors;
    r->value[3] = heap; r->value[4] = clusters; r->value[12] = o->type;
    if (o->type == FAT_FILESYSTEM_TYPE_EXFAT) {
        r->value[5] = (uint32_t)(((uint64_t)clusters + 7) / 8 + (uint64_t)spc * 512 - 1) / (spc * 512);
        r->value[6] = 2 + r->value[5]; r->value[7] = r->value[6] + 1; r->value[11] = r->value[5] + 2;
    } else { r->value[7] = o->type == FAT_FILESYSTEM_TYPE_FAT32 ? 2 : 0; }
    return true;
}

bool afatfs_fdisk(afatfsRequest_t *r, const afatfsPartitionOptions_t *o, afatfsResultCallback_t cb, void *user)
{
    if (!o || o->sectorCount < (o->gpt ? 68U : 2U)) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
#ifndef ASYNCFS_ENABLE_EXFAT
    if (o->gpt) { afatfs.lastError = AFATFS_ERROR_UNSUPPORTED; return false; }
#endif
    if (o->gpt) {
        bool nonzero = false; for (unsigned n = 0; n < 16; n++) nonzero |= o->diskGuid[n] != 0;
        if (!nonzero) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
    }
    for (unsigned n = 0; n < 4; n++) {
        if (!o->length[n]) continue;
        uint64_t end = (uint64_t)o->start[n] + o->length[n];
        if (o->start[n] < (o->gpt ? 34U : 1U) || end > o->sectorCount - (o->gpt ? 33U : 0U)
            || (!o->gpt && !o->type[n])) { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
        for (unsigned m = 0; m < n; m++) if (o->length[m] && o->start[n] < (uint64_t)o->start[m] + o->length[m] && o->start[m] < end)
            { afatfs.lastError = AFATFS_ERROR_INVALID_PARAMETER; return false; }
    }
    return afatfs_formatBegin(r, AFATFS_API_FDISK, o, cb, user);
}

static void afatfs_formatUpcase(uint8_t *buffer)
{
    memset(buffer, 0, 512);
    for (unsigned n = 0; n < 128; n++) buffer[n * 2] = (uint8_t)afatfs_apiUpper(n);
    buffer[256] = buffer[257] = 0xFF; buffer[258] = 0x7F; buffer[259] = 0xFF;
    buffer[260] = buffer[261] = 0xFF;
}

static void afatfs_formatVolumeSector(afatfsRequest_t *r, uint32_t relative, uint8_t *buffer)
{
    const afatfsFormatOptions_t *o = (const afatfsFormatOptions_t *)r->output;
    uint32_t spc = r->value[1], fatSectors = r->value[2], heap = r->value[3], count = r->value[4];
    bool exfat = o->type == FAT_FILESYSTEM_TYPE_EXFAT, fat32 = o->type == FAT_FILESYSTEM_TYPE_FAT32;
    memset(buffer, 0, 512);
    if ((exfat && relative < 24) || relative == 0 || (fat32 && relative == 6)) {
        uint32_t boot = exfat ? relative % 12 : 0;
        if (boot == 0) {
            fatVolumeID_t *v = (fatVolumeID_t *)buffer;
            v->jmpBoot[0] = 0xEB; v->jmpBoot[1] = exfat ? 0x76 : fat32 ? 0x58 : 0x3C; v->jmpBoot[2] = 0x90;
            memcpy(v->oemName, exfat ? "EXFAT   " : "ASYNCFS ", 8);
            if (exfat) {
                v->exFat.partitionOffset = o->startSector; v->exFat.volumeLength = o->sectorCount;
                v->exFat.fatOffset = 24; v->exFat.fatLength = fatSectors; v->exFat.clusterHeapOffset = heap;
                v->exFat.clusterCount = count; v->exFat.rootDirectoryCluster = r->value[7];
                v->exFat.volumeSerialNumber = o->serial; v->exFat.fileSystemRevision = 0x100;
                v->exFat.bytesPerSectorShift = 9;
                for (uint32_t n = spc; n > 1; n >>= 1) v->exFat.sectorsPerClusterShift++;
                v->exFat.numFATs = 1; v->exFat.driveSelect = 0x80;
                v->exFat.percentInUse = (uint8_t)((uint64_t)r->value[11] * 100 / count);
            } else {
                v->fat.bytesPerSector = 512; v->fat.sectorsPerCluster = (uint8_t)spc;
                v->fat.reservedSectorCount = fat32 ? 32 : 1; v->fat.numFATs = 2;
                v->fat.rootEntryCount = fat32 ? 0 : 512; v->fat.media = 0xF8;
                v->fat.hiddenSectors = o->startSector; v->fat.sectorsPerTrack = 32; v->fat.numHeads = 64;
                if (o->sectorCount < 65536) v->fat.totalSectors16 = (uint16_t)o->sectorCount;
                else v->fat.totalSectors32 = o->sectorCount;
                if (fat32) {
                    fat32Descriptor_t *d = &v->fat.fatDescriptor.fat32;
                    d->FATSize32 = fatSectors; d->rootCluster = 2; d->fsInfo = 1; d->backupBootSector = 6;
                    d->driveNumber = 0x80; d->bootSignature = 0x29; d->volumeID = o->serial;
                    memcpy(d->volumeLabel, "NO NAME    ", 11); memcpy(d->fileSystemType, "FAT32   ", 8);
                } else {
                    fat16Descriptor_t *d = &v->fat.fatDescriptor.fat16;
                    v->fat.FATSize16 = (uint16_t)fatSectors; d->driveNumber = 0x80; d->bootSignature = 0x29; d->volumeID = o->serial;
                    memcpy(d->volumeLabel, "NO NAME    ", 11); memcpy(d->fileSystemType, "FAT16   ", 8);
                }
            }
            buffer[510] = 0x55; buffer[511] = 0xAA;
        } else if (boot <= 8) afatfs_formatPut32(buffer + 508, 0xAA550000);
        else if (boot == 11) for (unsigned n = 0; n < 512; n += 4) afatfs_formatPut32(buffer + n, r->value[9]);
        return;
    }
    if (fat32 && (relative == 1 || relative == 7)) {
        afatfs_formatPut32(buffer, 0x41615252); afatfs_formatPut32(buffer + 484, 0x61417272);
        afatfs_formatPut32(buffer + 488, count - 1); afatfs_formatPut32(buffer + 492, 3);
        afatfs_formatPut32(buffer + 508, 0xAA550000); return;
    }
    uint32_t fatStart = exfat ? 24 : fat32 ? 32 : 1;
    if (relative >= fatStart && relative - fatStart < fatSectors * (exfat ? 1U : 2U)) {
        uint32_t width = o->type == FAT_FILESYSTEM_TYPE_FAT16 ? 2 : 4;
        uint32_t base = (relative - fatStart) % fatSectors * (512 / width);
        for (unsigned n = 0; n < 512 / width; n++) {
            uint32_t cluster = base + n, value = 0;
            if (cluster == 0) value = width == 2 ? 0xFFF8 : 0xFFFFFFF8;
            else if (cluster == 1 || (fat32 && cluster == 2)) value = 0xFFFFFFFF;
            else if (exfat && cluster >= 2 && cluster < 2 + r->value[5]) value = cluster + 1 == 2 + r->value[5] ? 0xFFFFFFFF : cluster + 1;
            else if (exfat && (cluster == r->value[6] || cluster == r->value[7])) value = 0xFFFFFFFF;
            if (width == 4) afatfs_formatPut32(buffer + n * 4, value);
            else { buffer[n * 2] = (uint8_t)value; buffer[n * 2 + 1] = (uint8_t)(value >> 8); }
        }
        return;
    }
    if (!exfat || relative < heap) return;
    uint32_t dataSector = relative - heap;
    if (dataSector < r->value[5] * spc) {
        uint32_t byteBase = dataSector * 512;
        for (unsigned n = 0; n < 512; n++) {
            uint64_t bit = (uint64_t)(byteBase + n) * 8;
            for (unsigned b = 0; b < 8; b++) if (bit + b < r->value[11] || bit + b >= count) buffer[n] |= (uint8_t)(1U << b);
        }
    } else if (dataSector == (r->value[6] - 2) * spc) afatfs_formatUpcase(buffer);
    else if (dataSector == (r->value[7] - 2) * spc) {
        exFatUpTableEntry_t *bitmap = (exFatUpTableEntry_t *)buffer, *upcase = bitmap + 1;
        bitmap->type = EXFAT_DIR_BITMAP; bitmap->firstCluster = 2; bitmap->dataSize = ((uint64_t)count + 7) / 8;
        upcase->type = EXFAT_DIR_UPCASE; upcase->firstCluster = r->value[6]; upcase->dataSize = 262; upcase->tableChecksum = r->value[10];
    }
}

static void afatfs_formatPartitionSector(afatfsRequest_t *r, uint32_t sector, uint8_t *buffer)
{
    const afatfsPartitionOptions_t *o = (const afatfsPartitionOptions_t *)r->output;
    memset(buffer, 0, 512);
    if (!sector) {
        mbrPartitionEntry_t *entries = (mbrPartitionEntry_t *)(buffer + 446);
        if (o->gpt) {
            entries[0].type = MBR_PARTITION_TYPE_GPT; entries[0].lbaBegin = 1; entries[0].numSectors = o->sectorCount - 1;
            entries[0].chsBegin[1] = 2; memset(entries[0].chsEnd, 0xFF, 3);
        }
        else for (unsigned n = 0; n < 4; n++) if (o->length[n]) {
            entries[n].type = o->type[n]; entries[n].lbaBegin = o->start[n]; entries[n].numSectors = o->length[n];
            memset(entries[n].chsBegin, 0xFF, 3); memset(entries[n].chsEnd, 0xFF, 3);
        }
        buffer[510] = 0x55; buffer[511] = 0xAA; return;
    }
    if (!o->gpt) return;
#ifdef ASYNCFS_ENABLE_EXFAT
    if (sector == 1 || sector == o->sectorCount - 1) {
        memcpy(buffer, "EFI PART", 8); afatfs_formatPut32(buffer + 8, 0x10000); afatfs_formatPut32(buffer + 12, 92);
        afatfs_formatPut32(buffer + 24, sector); afatfs_formatPut32(buffer + 32, sector == 1 ? o->sectorCount - 1 : 1);
        afatfs_formatPut32(buffer + 40, 34); afatfs_formatPut32(buffer + 48, o->sectorCount - 34);
        memcpy(buffer + 56, o->diskGuid, 16); afatfs_formatPut32(buffer + 72, sector == 1 ? 2 : o->sectorCount - 33);
        afatfs_formatPut32(buffer + 80, 128); afatfs_formatPut32(buffer + 84, 128); afatfs_formatPut32(buffer + 88, r->value[9]);
        afatfs_formatPut32(buffer + 16, ~afatfs_crc32Update(UINT32_MAX, buffer, 92));
    } else if (sector == 2 || sector == o->sectorCount - 33) {
        static const uint8_t basic[16] = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
        for (unsigned n = 0; n < 4; n++) if (o->length[n]) {
            uint8_t *entry = buffer + n * 128;
            memcpy(entry, basic, 16); memcpy(entry + 16, o->diskGuid, 16); entry[31] ^= (uint8_t)(n + 1); entry[16] |= 1;
            afatfs_formatPut32(entry + 32, o->start[n]); afatfs_formatPut32(entry + 40, o->start[n] + o->length[n] - 1);
        }
    }
#else
    (void)r;
#endif
}

static void afatfs_formatComplete(sdcardBlockOperation_e operation, uint32_t sector, uint8_t *buffer, uint32_t generation)
{
    (void)operation; (void)sector;
    if (generation != afatfs_mediaGeneration || !afatfs_request) return;
    afatfs_request->waiting = false; afatfs_request->value[15] = buffer ? 1 : 2;
    if (!buffer) afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
}
static void afatfs_apiFormat(afatfsRequest_t *r)
{
    if (r->waiting) return;
    uint8_t *buffer = afatfs.cache;
    bool disk = r->operation == AFATFS_API_FDISK;
    if (!r->phase) {
        if (disk) {
            const afatfsPartitionOptions_t *o = (const afatfsPartitionOptions_t *)r->output;
#ifdef ASYNCFS_ENABLE_EXFAT
            if (o->gpt) {
                if (!r->value[13]) r->value[9] = UINT32_MAX;
                afatfs_formatPartitionSector(r, 2 + r->value[13], buffer);
                r->value[9] = afatfs_crc32Update(r->value[9], buffer, 512);
                if (++r->value[13] < 32) return;
                r->value[9] = ~r->value[9];
            }
#else
            (void)o;
#endif
        } else if (r->value[12] == FAT_FILESYSTEM_TYPE_EXFAT) {
            afatfs_formatUpcase(buffer);
            for (unsigned n = 0; n < 262; n++) r->value[10] = (r->value[10] >> 1 | r->value[10] << 31) + buffer[n];
            for (unsigned page = 0; page < 11; page++) {
                afatfs_formatVolumeSector(r, page, buffer);
                for (unsigned n = 0; n < 512; n++) if (page || (n != 106 && n != 107 && n != 112))
                    r->value[9] = (r->value[9] >> 1 | r->value[9] << 31) + buffer[n];
            }
        }
        r->phase = 1;
    }
    uint32_t last, sector;
    if (disk) {
        const afatfsPartitionOptions_t *o = (const afatfsPartitionOptions_t *)r->output;
        last = o->gpt ? 67 : 3;
        sector = r->value[8] < 34 ? r->value[8] : o->sectorCount - 33 + r->value[8] - 34;
    } else {
        const afatfsFormatOptions_t *o = (const afatfsFormatOptions_t *)r->output;
        last = r->value[3] + (o->type == FAT_FILESYSTEM_TYPE_EXFAT ? r->value[11] * r->value[1]
            : o->type == FAT_FILESYSTEM_TYPE_FAT32 ? r->value[1] : 0);
        sector = o->startSector + r->value[8];
    }
    if (r->value[15] == 1) { r->value[8]++; r->value[15] = 0; }
    if (disk) {
        const afatfsPartitionOptions_t *o = (const afatfsPartitionOptions_t *)r->output;
        // Clear stale GPT signatures when switching to MBR, without touching any new partition's data.
        while (!o->gpt && r->value[8] > 0 && r->value[8] < last) {
            uint32_t probe = r->value[8] == 1 ? 1 : o->sectorCount - 1;
            bool occupied = r->value[8] == 2 && o->sectorCount == 2;
            for (unsigned n = 0; n < 4; n++)
                occupied |= o->length[n] && probe >= o->start[n] && (uint64_t)probe < (uint64_t)o->start[n] + o->length[n];
            if (!occupied) break;
            r->value[8]++;
        }
    }
    if (r->value[8] >= last) {
#ifdef AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT
        sdcardOperationStatus_e stopped = sdcard_endWriteBlocks();
        if (stopped != SDCARD_OPERATION_SUCCESS) {
            if (stopped == SDCARD_OPERATION_FAILURE) afatfs.lastError = AFATFS_ERROR_BACKEND_IO;
            return;
        }
#endif
        if (!sdcard_poll()) return;
        afatfsResultCallback_t cb = r->callback; void *user = r->user;
        afatfs_request = NULL; afatfs_resetMediaSession(AFATFS_FILESYSTEM_STATE_STOPPED);
        r->complete = true; r->result = AFATFS_ERROR_NONE; r->driving = false;
        if (cb) cb(AFATFS_ERROR_NONE, user);
        return;
    }
    // Recompute after advancing an acknowledged sector. The buffer remains untouched while DMA owns it.
    if (disk) {
        const afatfsPartitionOptions_t *o = (const afatfsPartitionOptions_t *)r->output;
        sector = o->gpt ? (r->value[8] < 34 ? r->value[8] : o->sectorCount - 33 + r->value[8] - 34)
            : r->value[8] == 2 ? o->sectorCount - 1 : r->value[8];
        afatfs_formatPartitionSector(r, sector, buffer);
    } else {
        const afatfsFormatOptions_t *o = (const afatfsFormatOptions_t *)r->output;
        sector = o->startSector + r->value[8]; afatfs_formatVolumeSector(r, r->value[8], buffer);
    }
    r->value[15] = 0;
    switch (sdcard_writeBlock(sector, buffer, afatfs_formatComplete, afatfs_mediaGeneration)) {
    case SDCARD_OPERATION_IN_PROGRESS: r->waiting = true; break;
    case SDCARD_OPERATION_SUCCESS: r->value[15] = 1; break;
    case SDCARD_OPERATION_FAILURE: afatfs.lastError = AFATFS_ERROR_BACKEND_IO; break;
    default: break;
    }
}
