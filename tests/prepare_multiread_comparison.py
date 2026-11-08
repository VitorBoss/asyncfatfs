"""Snapshot the current implementation and a local upstream reference."""
import argparse
import io
import json
import shutil
import subprocess
import tarfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory.resolve()
    assert (root / "before/lib/asyncfatfs.c").exists(), "Pre-change snapshot missing"
    shutil.copytree("lib", root / "after/lib", dirs_exist_ok=True)
    (root / "after/tests").mkdir(exist_ok=True)
    for name in ("sdcard_sim.c", "sdcard_sim.h"):
        shutil.copyfile(Path("tests") / name, root / "after/tests" / name)
    commit = subprocess.check_output(["git", "rev-parse", "upstream/master"], text=True).strip()
    with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(["git", "archive", commit, "lib"]))) as bundle:
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
    for variant in ("before", "after", "default"):
        version = "before" if variant == "before" else "after"
        source = (root / version / "lib/asyncfatfs.c").as_posix()
        (root / f"{variant}-layout.c").write_text(f'#include "{source}"\nconst uint16_t comparison_layout[] = {{sizeof(afatfs_t), sizeof(afatfsFile_t), sizeof(afatfsCacheBlockDescriptor_t)}};\n')
    for variant in ("before", "after"):
        source = (root / variant / "tests/sdcard_sim.c").as_posix()
        (root / f"{variant}-backend-layout.c").write_text(f'#include "{source}"\nconst uint16_t comparison_layout[] = {{sizeof(sdcard), sizeof(sdcard.currentOperation), sizeof(sdcard.state)}};\n')
    (root / "manifest.json").write_text(json.dumps(dict(upstream_commit=commit), indent=2))


if __name__ == "__main__":
    main()
