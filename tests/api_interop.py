"""Independently verify images emitted by AFATFS_API_IMAGE_DIR tests.

Only regular image files are accessed. FATtools supplies an independent parser
and chain reader; exfatprogs additionally checks exFAT allocation/entry integrity.
"""
import argparse
import json
import struct
from pathlib import Path

from exfat_interop import (COMMAND_LOG, Chain, check_volume, close_volume,
                           extract, pattern, volume)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", type=Path, default=Path("tests/build/api-images"))
    parser.add_argument("--exfatprogs", type=Path, help="Also run fsck.exfat; omit for FATtools-only verification")
    args = parser.parse_args()
    results = []
    for kind, scheme in ((2, "mbr"), (3, "mbr"), (4, "mbr"), (4, "gpt")):
        disk = args.images / f"api-fat{kind}-{scheme}.img"
        partition = args.images / f"volume-fat{kind}-{scheme}.img"
        extract(disk, partition)
        root = volume(partition)
        try:
            entry = root.find("VERIFY.BIN")
            assert entry is not None, disk
            if kind == 4:
                size = entry.u64DataLength
                modified = entry.dwMTime
                data = Chain(root.boot, root.fat, entry.Start(), size, nofat=entry.IsContig()).read()
            else:
                size = entry.dwFileSize
                modified = (entry.wMDate << 16) | entry.wMTime
                data = Chain(root.boot, root.fat, entry.Start(), size).read()
            assert size == 2003 and bytes(data) == pattern(2003, 55), disk
            assert modified == 0x58221883, (disk, hex(modified))
            assert root.find("TCLIP.BIN") is None and root.find("RENAMED.BIN") is None
        finally:
            close_volume(root)
        if kind == 4 and args.exfatprogs:
            check_volume(args.exfatprogs, partition)
        elif kind == 3:
            with partition.open("rb") as image:
                for sector in (1, 7):
                    image.seek(sector * 512 + 488)
                    assert struct.unpack("<II", image.read(8)) == (0xFFFFFFFF, 0xFFFFFFFF)
        results.append({"type": kind, "scheme": scheme, "bytes": size,
                        "modified": hex(modified), "verified": True})
        print(f"Verified FAT type {kind}/{scheme}: payload, size and timestamp")
    report = {"results": results, "fsck_requested": bool(args.exfatprogs), "commands": COMMAND_LOG}
    (args.images / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
