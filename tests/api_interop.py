"""Independently verify images emitted by AFATFS_API_IMAGE_DIR tests.

Only regular image files are accessed. FATtools supplies an independent parser
and chain reader; exfatprogs additionally checks exFAT allocation/entry integrity.
"""
import argparse
import hashlib
from importlib.metadata import version
import json
import struct
from pathlib import Path

from exfat_interop import (ROOT, COMMAND_LOG, Chain, check_volume, close_volume,
                           extract, pattern, volume)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", type=Path, default=Path("tests/build/api-images"))
    parser.add_argument("--exfatprogs", type=Path, help="Also run fsck.exfat; omit for FATtools-only verification")
    args = parser.parse_args()
    report = {"status": "incomplete", "results": [], "fsck_requested": bool(args.exfatprogs),
              "fattools_version": version("FATtools"), "commands": COMMAND_LOG,
              "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in sorted((ROOT / "lib").glob("*"))
                                if path.suffix in (".c", ".h", ".inc")}}
    if args.exfatprogs:
        report["exfatprogs_sha256"] = hashlib.sha256(args.exfatprogs.read_bytes()).hexdigest()
    try:
        verify_images(args, report)
        report["status"] = "passed"
    finally:
        # Failed checks must replace older success reports and retain the tool's exit code.
        (args.images / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def verify_images(args, report):
    for kind, scheme in ((2, "mbr"), (3, "mbr"), (4, "mbr"), (4, "gpt")):
        disk = args.images / f"api-fat{kind}-{scheme}.img"
        partition = args.images / f"volume-fat{kind}-{scheme}.img"
        result = {"type": kind, "scheme": scheme, "verified": False,
                  "image_sha256": hashlib.sha256(disk.read_bytes()).hexdigest()}
        report["results"].append(result)
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
            result["fsck"] = check_volume(args.exfatprogs, partition).strip()
        elif kind == 3:
            with partition.open("rb") as image:
                for sector in (1, 7):
                    image.seek(sector * 512 + 488)
                    assert struct.unpack("<II", image.read(8)) == (0xFFFFFFFF, 0xFFFFFFFF)
        result.update(bytes=size, modified=hex(modified), verified=True)
        print(f"Verified FAT type {kind}/{scheme}: payload, size and timestamp")


if __name__ == "__main__":
    main()
