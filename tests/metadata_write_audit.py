"""Prepare isolated metadata diagnostics; never edit production library sources."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


VARIANTS = ('base', 'no_reservation', 'no_group', 'data_pressure', 'reserve_wait', 'upstream')


def replace_once(source, old, new):
    assert source.count(old) == 1, old[:100]
    return source.replace(old, new, 1)


def block_end(source, opening):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: ' ' * len(match[0]), source, flags=re.S)
    depth = 0
    for position in range(opening, len(source)):
        if masked[position] == '{':
            depth += 1
        elif masked[position] == '}':
            depth -= 1
            if depth == 0:
                return position + 1
    raise AssertionError('Unclosed block')


def replace_body(source, name, body):
    match = re.search(r'(?m)^static[^\n]*\b' + name + r'\([^;{}]*\)\s*\{', source)
    assert match, name
    opening = match.end() - 1
    ending = block_end(source, opening)
    return source[:opening] + '{\n' + body + '\n}' + source[ending:]


TRACE_DECLARATIONS = r'''
#include <stdio.h>
// Diagnostic-only globals in this generated copy, never in the production library.
static bool comparisonFlushBackground;
static uint32_t comparisonFlushExcluded;
static uint32_t comparisonPressureSector;
static void comparisonMetadataFlush(int cacheIndex);
'''

TRACE_IMPLEMENTATION = r'''
/** @brief Name an active operation in a diagnostic snapshot. */
static const char *comparisonOperationName(const afatfsFile_t *file)
{
    switch (file->operation.active) {
        case AFATFS_FILE_OPERATION_NONE:
            return "NONE";
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            return "APPEND";
        case AFATFS_FILE_OPERATION_SEEK:
            return "SEEK";
        case AFATFS_FILE_OPERATION_CLOSE:
            return "CLOSE";
        case AFATFS_FILE_OPERATION_SYNC:
            return "SYNC";
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            return "CREATE";
        default:
            return "OTHER";
    }
}

/** @brief Record metadata submissions without moving any cache or file state. */
static void comparisonMetadataFlush(int cacheIndex)
{
    const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[cacheIndex];
    unsigned kind;
    if (entry->sectorIndex >= afatfs.fatStartSector &&
        entry->sectorIndex - afatfs.fatStartSector < afatfs.fatSectors) {
        kind = 1;
    } else if (entry->sectorIndex >= afatfs.fatStartSector + afatfs.fatSectors &&
        entry->sectorIndex - afatfs.fatStartSector < 2 * afatfs.fatSectors) {
        kind = 2;
    } else if (entry->sectorIndex == afatfs.clusterStartSector) {
        kind = 3;
    } else {
        return;
    }
    unsigned ownerMask = 0;
    unsigned eligibleMask = 0;
    unsigned phaseMask = 0;
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *file = &afatfs.openFiles[index];
        if (file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
            phaseMask |= 1U << file->operation.state.appendFreeCluster.phase;
        }
        if (file->type == AFATFS_FILE_TYPE_NORMAL && file->modified &&
            (file->mode & (AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_APPEND)) &&
            file->directoryEntryPos.sectorNumberPhysical == entry->sectorIndex) {
            ownerMask |= 1U << index;
            if (file->operation.active == AFATFS_FILE_OPERATION_NONE ||
                file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER) {
                eligibleMask |= 1U << index;
            }
        }
    }
    fprintf(stderr, "MSUB,%u,%u,%u,%u,%u,%u,%u,%u,%u", kind, entry->sectorIndex,
        (unsigned)comparisonFlushBackground, comparisonFlushExcluded, comparisonPressureSector,
        afatfs.cacheTimer - entry->accessTimestamp, ownerMask, eligibleMask, phaseMask);
    for (unsigned index = 0; index < AFATFS_MAX_OPEN_FILES; index++) {
        const afatfsFile_t *file = &afatfs.openFiles[index];
        int phase = file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER
            ? (int)file->operation.state.appendFreeCluster.phase : -1;
        fprintf(stderr, ",%s,%d,%u,%u", comparisonOperationName(file), phase,
            file->cursorOffset, file->physicalSize);
    }
    fputc('\n', stderr);
}
'''


def instrument(source):
    source = replace_once(source, '#include "sdcard.h"', '#include "sdcard.h"\n' + TRACE_DECLARATIONS)
    source = replace_once(source,
                          '        case SDCARD_OPERATION_IN_PROGRESS:\n'
                          '            // The card will call us back later when the buffer transmission finishes',
                          '        case SDCARD_OPERATION_IN_PROGRESS:\n'
                          '            comparisonMetadataFlush(cacheIndex);\n'
                          '            // The card will call us back later when the buffer transmission finishes')
    source = replace_once(source,
                          '        if (earliestSectorIndex > -1) {\n'
                          '            afatfs_cacheFlushSector(earliestSectorIndex);',
                          '        if (earliestSectorIndex > -1) {\n'
                          '            comparisonFlushBackground = background;\n'
                          '            comparisonFlushExcluded = excludedSector;\n'
                          '            afatfs_cacheFlushSector(earliestSectorIndex);')
    start = source.index('    if (cacheSectorIndex == -1) {')
    opening = source.index('{', start)
    ending = block_end(source, opening)
    body = source[opening + 1:ending - 1]
    expected_returns = 3 if 'afatfs_cacheFlushSector(dataIndex);' in body else 2
    assert body.count('return AFATFS_OPERATION_IN_PROGRESS;') == expected_returns
    body = body.replace('                afatfs_cacheFlushSector(dataIndex);',
                        '                comparisonFlushBackground = false;\n'
                        '                comparisonFlushExcluded = 0;\n'
                        '                afatfs_cacheFlushSector(dataIndex);')
    body = re.sub(r'(?m)^([ \t]*)return AFATFS_OPERATION_IN_PROGRESS;',
                  lambda match: match[1] + 'comparisonPressureSector = 0;\n' + match[0], body)
    source = source[:opening + 1] + '\n        comparisonPressureSector = physicalSectorIndex;' + body + source[ending - 1:]
    return source + '\n' + TRACE_IMPLEMENTATION


def diagnostic_harness(source):
    source = replace_once(source, 'static benchmarkStats_t statistics;',
                          'static benchmarkStats_t statistics;\nstatic uint64_t metadataChangedEntries[2][129];')
    source = replace_once(source, '        if (transferWrite) {', r'''        if (transferWrite) {
            if (transferSector >= 2 && transferSector < dataStart) {
                uint32_t copyIndex = (transferSector - 2) / ((dataStart - 2) / 2);
                uint32_t changed = 0;
                for (uint32_t index = 0; index < SECTOR_BYTES / 4; index++) {
                    changed += memcmp(sectorData + index * 4, transferBuffer + index * 4, 4) != 0;
                }
                metadataChangedEntries[copyIndex][changed]++;
            }''')
    source = replace_once(source, '#ifdef COMPARISON_CPU_WORK_AUDIT\n    fprintf(stderr, "WORK,', r'''
    for (unsigned copyIndex = 0; copyIndex < 2; copyIndex++) {
        for (unsigned changed = 0; changed <= SECTOR_BYTES / 4; changed++) {
            if (metadataChangedEntries[copyIndex][changed]) {
                fprintf(stderr, "FAT_HIST,%s,%u,%u,%u,%u,%u,%u,%u,%llu\n", phase, latencyProfile,
                    clusterSectors, chunkBytes, fileCount, repetition, copyIndex, changed,
                    (unsigned long long)metadataChangedEntries[copyIndex][changed]);
                metadataChangedEntries[copyIndex][changed] = 0;
            }
        }
    }
#ifdef COMPARISON_CPU_WORK_AUDIT
    fprintf(stderr, "WORK,''')
    marker = 'static void runCase(uint32_t clusterSectors, uint32_t chunkBytes, uint32_t fileCount, uint32_t repetition)\n{'
    source = replace_once(source, marker, marker + '\n    fprintf(stderr, "CASE,%u,%u,%u,%u\\n", latencyProfile, clusterSectors, chunkBytes, fileCount);')
    source = replace_once(source, '#ifdef COMPARISON_CPU_ONLY\n        if (latencyProfile != 0) {\n            break;\n        }\n#endif\n', '')
    return source


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    base_root = Path('tests/build/io-cpu-relation')
    base = (base_root / 'current/lib/asyncfatfs.c').read_text()
    assert hashlib.sha256(Path('lib/asyncfatfs.c').read_bytes()).hexdigest() == hashlib.sha256((base_root / 'current/lib/asyncfatfs.c').read_bytes()).hexdigest()
    sources = {'base': base,
               'no_reservation': replace_body(base, 'afatfs_reserveNextCluster', '    (void)file;'),
               'no_group': replace_body(replace_body(base, 'afatfs_groupableAllocation', '    (void)file;\n    return false;'),
                                        'afatfs_pendingAllocationGroup', '    return false;')}
    old = '''afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && physicalSectorIndex >= afatfs.fatStartSector &&
            physicalSectorIndex - afatfs.fatStartSector < 2 * afatfs.fatSectors &&'''
    new = '''afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&
            ((physicalSectorIndex >= afatfs.fatStartSector &&
                 physicalSectorIndex - afatfs.fatStartSector < 2 * afatfs.fatSectors) || afatfs.sectorsPerCluster == 1) &&'''
    sources['data_pressure'] = replace_once(base, old, new)
    sources['reserve_wait'] = replace_once(base,
        '                        afatfs_flushCache();\n                        return AFATFS_OPERATION_IN_PROGRESS;',
        '                        afatfs_flushAllocationCache(file);\n                        return AFATFS_OPERATION_IN_PROGRESS;')
    for variant in VARIANTS:
        version = 'upstream' if variant == 'upstream' else 'current'
        destination = root / variant / 'lib'
        shutil.copytree(base_root / version / 'lib', destination, dirs_exist_ok=True)
        if variant != 'upstream':
            (destination / 'asyncfatfs.c').write_text(instrument(sources[variant]), encoding='utf-8')
    shutil.copy2(base_root / 'portability.h', root / 'portability.h')
    harness = Path('tests/upstream_comparison.c').read_text()
    (root / 'harness.c').write_text(diagnostic_harness(harness), encoding='utf-8')
    manifest = dict(base_sha256=hashlib.sha256(Path('lib/asyncfatfs.c').read_bytes()).hexdigest(),
                    variants=list(VARIANTS), production_modified=False,
                    generated_source_sha256={variant: hashlib.sha256(
                        (root / variant / 'lib/asyncfatfs.c').read_bytes()).hexdigest() for variant in VARIANTS})
    (root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/metadata-write-audit'))
    args = parser.parse_args()
    prepare(args.directory)
