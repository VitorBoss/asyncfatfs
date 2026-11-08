"""Prepare isolated, deliberately experimental cross-cluster prefetch copies."""

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
from pathlib import Path


PREFETCH = r'''void afatfs_fprefetch(afatfsFilePtr_t file)
{
    if (!afatfs_mediaSessionActive() || !file || afatfs.fileSystemType != FAT_FILESYSTEM_TYPE_FAT32 ||
        file->type != AFATFS_FILE_TYPE_NORMAL || !(file->mode & AFATFS_FILE_MODE_READ) ||
        afatfs_fileIsBusy(file) || afatfs_isEndOfAllocatedFile(file)) {
        return;
    }
    uint32_t wanted = file->cursorOffset;
    uint32_t cluster = file->cursorCluster;
    uint32_t sectorIndex = afatfs_byteIndexInCluster(wanted) / AFATFS_SECTOR_SIZE;
    uint32_t inSector = wanted % AFATFS_SECTOR_SIZE;
    if (inSector) {
        if (file->logicalSize - wanted <= AFATFS_SECTOR_SIZE - inSector) {
            return;
        }
        wanted += AFATFS_SECTOR_SIZE - inSector;
        sectorIndex++;
    }
    for (unsigned ahead = 0; ahead < AFATFS_PREFETCH_WINDOW_SIZE && wanted < file->logicalSize; ahead++) {
        if (sectorIndex >= afatfs.sectorsPerCluster) {
            uint32_t nextCluster;
            if (afatfs_fileGetNextCluster(file, cluster, &nextCluster) != AFATFS_OPERATION_SUCCESS ||
                nextCluster < FAT_SMALLEST_LEGAL_CLUSTER_NUMBER || nextCluster >= afatfs.numClusters + 2) {
                return;
            }
            cluster = nextCluster;
            sectorIndex = 0;
        }
        uint8_t *buffer;
        if (afatfs_cacheSector(afatfs_fileClusterToPhysical(cluster, sectorIndex), &buffer,
                AFATFS_CACHE_READ, 0) != AFATFS_OPERATION_SUCCESS) {
            return;
        }
        if (file->logicalSize - wanted <= AFATFS_SECTOR_SIZE) {
            return;
        }
        wanted += AFATFS_SECTOR_SIZE;
        sectorIndex++;
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory.resolve()
    root.mkdir(parents=True, exist_ok=True)
    for variant in ("current", "window"):
        shutil.copytree("lib", root / variant / "lib", dirs_exist_ok=True)
    target = root / "window/lib/asyncfatfs.c"
    source = target.read_text()
    start = source.index("void afatfs_fprefetch(afatfsFilePtr_t file)")
    end = source.index("\n/**", start)
    source = source[:start] + PREFETCH + source[end:]
    previous = """        if (cursorOffsetInSector + bytesToReadThisSector == AFATFS_SECTOR_SIZE) {
            afatfs_fprefetch(file);
        }"""
    assert source.count(previous) == 1
    source = source.replace(previous, "        afatfs_fprefetch(file);")
    target.write_text(source)
    commit = subprocess.check_output(["git", "rev-parse", "upstream/master"], text=True).strip()
    archive = subprocess.check_output(["git", "archive", commit, "lib"])
    with tarfile.open(fileobj=io.BytesIO(archive)) as bundle:
        for member in bundle.getmembers():
            if member.isfile() and member.name.startswith("lib/") and ".." not in Path(member.name).parts:
                target = root / "upstream" / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(bundle.extractfile(member).read())
    (root / "portability.h").write_text('#define __attribute__(attributes)\n#pragma pack(push, 1)\n#include "fat_standard.h"\n#pragma pack(pop)\n')
    (root / "config").mkdir(exist_ok=True)
    (root / "config/afatfs_config.h").write_text(
        '#define AFATFS_NUM_CACHE_SECTORS 4\n#define AFATFS_MAX_OPEN_FILES 3\n'
        '#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2\n#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100\n'
        '#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"\n#define AFATFS_INTROSPEC_LOG_FILENAME "ASYNCFAT.LOG"\n')
    hashes = {variant: {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted((root / variant / "lib").iterdir()) if path.is_file()}
              for variant in ("current", "window", "upstream")}
    (root / "manifest.json").write_text(json.dumps(dict(upstream_commit=commit, source_hashes=hashes), indent=2))
    print(f"Prepared isolated experiment: {root}")


if __name__ == "__main__":
    main()
