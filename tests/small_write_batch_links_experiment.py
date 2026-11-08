"""Prepare a single-pair acquisition for already-durable in-page batch links."""
from pathlib import Path
import shutil

from full_upstream_report import fixed_clock

root = Path('tests/build/small-write-cpu')
source = Path('lib/asyncfatfs.c').read_text()
old = '''            while ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                uint32_t stride = (opState->progress.zeroSector >> 28) + 1;
                uint32_t last = (opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
                status = afatfs_FATSetNextCluster(last - stride, last);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                opState->progress.zeroSector -= stride;
            }'''
new = '''            if ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                afatfsFATSector_t sector;
                uint8_t *mirror;
                status = afatfs_FATWriteSector(opState->searchCluster / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR,
                    &sector, &mirror, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE | AFATFS_CACHE_METADATA,
                    AFATFS_FAT_MIRROR_STATE(NULL) 0);
                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }
                // The requested window stays in one FAT page. Its destination
                // EOCs are durable in both copies before any link is prepared.
                while ((opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) > opState->searchCluster + 1) {
                    uint32_t stride = (opState->progress.zeroSector >> 28) + 1;
                    uint32_t last = (opState->progress.zeroSector & AFATFS_CLUSTER_HINT_MASK) - 1;
                    uint32_t offset = (last - stride) % AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;
                    sector.fat32[offset] = (sector.fat32[offset] & 0xF0000000U) | last;
                    opState->progress.zeroSector -= stride;
                }
                memcpy(mirror, sector.bytes, AFATFS_SECTOR_SIZE);
            }'''
assert source.count(old) == 1
links = source.replace(old, new)
old = '''                // The common writer continuation needs no generic opcode dispatch.
                afatfs_appendRegularFreeClusterContinue(file);'''
new = '''                if (file->operation.state.appendFreeCluster.phase == AFATFS_APPEND_FREE_CLUSTER_PHASE_COLLECT) {
                    if (afatfs.cacheFlushInProgress) {
                        continue;
                    }
                    if (file->operation.state.appendFreeCluster.collectIdle < 63) {
                        file->operation.state.appendFreeCluster.collectIdle++;
                        continue;
                    }
                }
                // The common writer continuation needs no generic opcode dispatch.
                afatfs_appendRegularFreeClusterContinue(file);'''
assert source.count(old) == 1
poll = source.replace(old, new)
for name, candidate in (('links', links), ('poll', poll)):
    shutil.copytree('lib', root / name / 'lib', dirs_exist_ok=True)
    (root / name / 'lib/asyncfatfs.c').write_text(fixed_clock(candidate))
    (root / f'{name}-production.c').write_text(candidate)
for schedule in ('serial', 'interleaved'):
    for name in ('before', 'loop', 'fast', 'upstream'):
        shutil.copytree(root / f'cpu-{schedule}-{name}', root / f'initial-cpu-{schedule}-{name}', dirs_exist_ok=True)
shutil.copyfile(root / 'summary.json', root / 'initial-summary.json')
runner = Path('tests/run_small_write_cpu.ps1').read_text()
runner = runner.replace("@('before', 'loop', 'fast', 'upstream')", "@('before', 'links', 'poll', 'upstream')")
runner = runner.replace('if (!$RunOnly) {', "if (!$RunOnly -and $variant -notin @('before', 'upstream')) {")
Path('tests/run_small_write_batch_links.ps1').write_text(runner)
