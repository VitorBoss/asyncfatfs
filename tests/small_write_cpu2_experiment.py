"""Prepare arithmetic and already-dirty FAT-pair experiments for small writes."""
import json
from pathlib import Path
import shutil

from full_upstream_report import digest, fixed_clock

root = Path('tests/build/small-write-cpu2')
root.mkdir(parents=True, exist_ok=True)
source = Path('lib/asyncfatfs.c').read_text(encoding='utf-8')
arithmetic = source.replace(
    '(last - state->searchCluster) / stride + 1 >= AFATFS_SMALL_WRITE_WINDOW_CLUSTERS',
    '(AFATFS_SMALL_WRITE_WINDOW_CLUSTERS <= 1 ||\n'
    '            last - state->searchCluster >= (AFATFS_SMALL_WRITE_WINDOW_CLUSTERS - 1U) * stride)')
old = '''                while ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                    uint32_t stride = (opState->progress.zeroSector >> 28) + 1;
                    uint32_t last = (opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
                    uint32_t offset = (last - stride) % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                    sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | last;
                    opState->progress.zeroSector -= stride;
                }
                memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);'''
new = '''                uint32_t stride = (opState->progress.zeroSector >> 28) + 1;
                uint32_t last = (opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
                while (last > opState->searchCluster) {
                    uint32_t offset = (last - stride) % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                    sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | last;
                    last -= stride;
                }
                memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
                opState->progress.zeroSector =
                    (opState->progress.zeroSector & ~AFATFS_CLUSTER_HINT_MASK) | (last + 1);'''
assert arithmetic.count(old) == 1
arithmetic = arithmetic.replace(old, new)
old = '''    uint32_t value;
    if (afatfs_FATGetNextCluster(0, candidate, &value) != AFATFS_OPERATION_SUCCESS) {
        return false;
    }
    if (!fat_isFreeSpace(value) || afatfs_clusterIsReserved(candidate)) {
        afatfs_sealCollections();
        return false;
    }
    afatfsFATSector_t sector;
    uint8_t *mirror;
    if (afatfs_FATWriteSector(candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR, &sector, &mirror,
            AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, AFATFS_FAT_MIRROR_STATE(NULL) 0) !=
        AFATFS_OPERATION_SUCCESS) {
        return false;
    }
    uint32_t offset = candidate % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
    sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | 0x0FFFFFFFU;
    memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);'''
new = '''    afatfsFATSector_t sector;
    uint8_t *mirror;
    uint32_t offset = candidate % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
    LBA_t primarySector = afatfs_fatSectorToPhysical(0, candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);
    int primaryIndex = -1;
    int mirrorIndex = -1;
    for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
        const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
        if (entry->state != AFATFS_CACHE_STATE_DIRTY) {
            continue;
        }
        if (entry->sectorIndex == primarySector) {
            primaryIndex = index;
        } else if (entry->sectorIndex == primarySector + afatfs.fatSectors) {
            mirrorIndex = index;
        }
    }
    if (primaryIndex >= 0 && mirrorIndex >= 0) {
        // An already-dirty native pair has identical writable images. Neither
        // page can be evicted or backend-owned. Preserve the generic path's
        // read, primary acquisition and mirror acquisition access accounting.
        sector.bytes = afatfs_cacheSectorGetMemory(primaryIndex);
        mirror = afatfs_cacheSectorGetMemory(mirrorIndex);
        afatfs.cacheDescriptor[primaryIndex].accessTimestamp = ++afatfs.cacheTimer;
        if (!fat_isFreeSpace(sector.fat32[offset] & 0x0FFFFFFFU) || afatfs_clusterIsReserved(candidate)) {
            afatfs_sealCollections();
            return false;
        }
        afatfs.cacheDescriptor[primaryIndex].accessTimestamp = ++afatfs.cacheTimer;
        afatfs.cacheDescriptor[mirrorIndex].accessTimestamp = ++afatfs.cacheTimer;
        afatfs.fsInfoDirty = true;
        sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | 0x0FFFFFFFU;
        memcpy(mirror + offset * sizeof(uint32_t), &sector.fat32[offset], sizeof(uint32_t));
    } else {
        uint32_t value;
        if (afatfs_FATGetNextCluster(0, candidate, &value) != AFATFS_OPERATION_SUCCESS) {
            return false;
        }
        if (!fat_isFreeSpace(value) || afatfs_clusterIsReserved(candidate)) {
            afatfs_sealCollections();
            return false;
        }
        if (afatfs_FATWriteSector(candidate / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR, &sector, &mirror,
                AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA, AFATFS_FAT_MIRROR_STATE(NULL) 0) !=
            AFATFS_OPERATION_SUCCESS) {
            return false;
        }
        sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | 0x0FFFFFFFU;
        memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
    }'''
assert source.count(old) == 1
hit = source.replace(old, new)
assert arithmetic.count(old) == 1
both = arithmetic.replace(old, new)
for name, candidate in (('before', source), ('arithmetic', arithmetic), ('hit', hit), ('both', both)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / f'{name}-production.c').write_text(candidate, encoding='utf-8')
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate), encoding='utf-8')
old_root = Path('tests/build/small-write-cpu/final')
shutil.copytree(old_root / 'upstream', root / 'upstream', dirs_exist_ok=True)
for name in ('harness.c', 'portability.h'):
    shutil.copyfile(old_root / name, root / name)
for kind in ('cpu', 'io'):
    for source_name, name in (('after', 'before'), ('upstream', 'upstream')):
        shutil.copytree(old_root / f'{kind}-interleaved-{source_name}', root / f'{kind}-interleaved-{name}', dirs_exist_ok=True)
runner = Path('tests/run_full_upstream_comparison.ps1').read_text()
runner = runner.replace("'tests/build/full-upstream-20261119'", "'tests/build/small-write-cpu2'")
runner = runner.replace("@('default', 'multiread', 'query', 'upstream', 'upstream_min2')",
    "@('before', 'arithmetic', 'hit', 'both', 'upstream')")
runner = runner.replace("        serial = ''\n", '')
runner = runner.replace('if (!$RunOnly) {', 'if (!$RunOnly -and !(Test-Path "$rootAbsolute/cpu-$schedule-$variant/benchmark.exe")) {')
runner = runner.replace("$sourceName = if ($variant.StartsWith('upstream')) { $variant } else { 'current' }", '$sourceName = $variant')
Path('tests/run_small_write_cpu2_experiment.ps1').write_text(runner)
(root / 'before-source.json').write_text(json.dumps({path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()}, indent=2))
print(root)
