"""Reject untrusted FAT32 journals before issuing any recovery writes."""
import argparse
import json
import shutil
import struct
import zlib
from pathlib import Path

from fat_lfn_powercut import invoke, records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"status": "running", "cases": []}
    try:
        for cluster in (512, 4096):
            folder = args.matrix / f"{cluster}-rename"
            base = folder / "base.img"
            writes = records(folder / "writes.trace")
            candidates = [(index + 1, struct.unpack_from("<I", data, 16)[0], sector, data)
                          for index, (sector, data) in enumerate(writes)
                          if data[:8] == b"AFJFT001" and struct.unpack_from("<I", data, 20)[0]
                          and struct.unpack_from("<I", data, 16)[0]]
            cut, count, header_sector, header = max(candidates, key=lambda item: item[1])
            head = struct.unpack_from("<I", header, 32)[0]
            journal = struct.unpack_from("<I", header, 52)[0] + (head - 2) * (cluster // 512)
            partition = struct.unpack_from("<I", header, 44)[0]
            fat_start = struct.unpack_from("<I", header, 48)[0]
            with base.open("rb") as original:
                original.seek(partition * 512)
                boot = original.read(512)
            fat_sectors = struct.unpack_from("<I", boot, 36)[0]
            root = struct.unpack_from("<I", boot, 44)[0]
            root_start = (struct.unpack_from("<I", header, 52)[0] + (root - 2) * (cluster // 512)) * 512
            variants = ("headers", "record", "image", "record-range", "record-self", "record-epoch",
                        "header-flags", "clean-active", "serial", "primary-chain", "mirror-chain",
                        "missing", "attributes", "external-dirty")
            for variant in variants:
                image = args.output / f"{cluster}-{variant}.img"
                shutil.copyfile(base, image)
                with image.open("r+b") as media:
                    if variant in ("record", "image", "record-range", "record-self", "record-epoch", "clean-active"):
                        for sector, data in writes[:cut]:
                            media.seek(sector * 512)
                            media.write(data)
                    if variant == "headers":
                        for slot in (0, 1):
                            media.seek((journal + slot) * 512)
                            media.write(b"INVALID!")
                    elif variant in ("record-range", "record-self", "record-epoch"):
                        sector = journal + 2 + 2 * (count - 1)
                        media.seek(sector * 512)
                        descriptor = bytearray(media.read(512))
                        if variant == "record-epoch":
                            struct.pack_into("<I", descriptor, 8, struct.unpack_from("<I", descriptor, 8)[0] + 1)
                        else:
                            struct.pack_into("<I", descriptor, 12, journal if variant == "record-self" else partition)
                        struct.pack_into("<I", descriptor, 508, zlib.crc32(descriptor[:508]))
                        media.seek(sector * 512)
                        media.write(descriptor)
                    elif variant == "header-flags":
                        for slot in (0, 1):
                            media.seek((journal + slot) * 512)
                            invalid = bytearray(media.read(512))
                            struct.pack_into("<I", invalid, 20, 2)
                            struct.pack_into("<I", invalid, 508, zlib.crc32(invalid[:508]))
                            media.seek((journal + slot) * 512)
                            media.write(invalid)
                    elif variant in ("record", "image"):
                        # Damage the last published record, proving the whole log is
                        # validated before even its first otherwise-valid undo write.
                        sector = journal + 2 + 2 * (count - 1) + (variant == "image")
                        media.seek(sector * 512 + 100)
                        previous = media.read(1)
                        media.seek(-1, 1)
                        media.write(bytes([previous[0] ^ 0x80]))
                    elif variant == "serial":
                        media.seek(partition * 512 + 67)
                        previous = media.read(1)
                        media.seek(-1, 1)
                        media.write(bytes([previous[0] ^ 1]))
                    elif variant.endswith("chain"):
                        start = fat_start + (fat_sectors if variant == "mirror-chain" else 0)
                        media.seek(start * 512 + head * 4)
                        media.write(struct.pack("<I", 0x0FFFFFFF))
                    elif variant in ("missing", "attributes"):
                        media.seek(root_start)
                        entries = media.read(cluster)
                        offset = next(index for index in range(0, len(entries), 32)
                                      if entries[index:index + 11] == b"AFATFS  JNL")
                        media.seek(root_start + offset + (11 if variant == "attributes" else 0))
                        media.write(b"\x20" if variant == "attributes" else b"\xE5")
                    if variant in ("missing", "external-dirty", "clean-active"):
                        for table in (0, 1):
                            media.seek((fat_start + table * fat_sectors) * 512 + 4)
                            value = struct.unpack("<I", media.read(4))[0]
                            media.seek(-4, 1)
                            media.write(struct.pack("<I", value | 0x08000000 if variant == "clean-active" else value & ~0x08000000))
                result_path = image.with_suffix(".json")
                trace = image.with_suffix(".trace")
                probe = invoke(args.binary, image, "guard", "rename", trace, result_path)
                result = json.loads(result_path.read_text()) if probe["code"] == 0 else {}
                passed = probe["code"] == 0 and result.get("readOnly") and not records(trace)
                report["cases"].append({"cluster": cluster, "variant": variant, "passed": passed,
                                        "probe": probe, "result": result})
                assert passed, report["cases"][-1]
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "error"
        report["error"] = repr(error)
        raise
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
