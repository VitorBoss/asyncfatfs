"""Cut power during undo replay itself, and check damaged logs fail closed."""
import argparse
import hashlib
import json
import shutil
import struct
import zlib
from pathlib import Path

import exfat_powercut as cuts


def replay(source, destination, records, count, torn=None):
    shutil.copyfile(source, destination)
    with destination.open("r+b") as disk:
        for n in range(count):
            block = struct.unpack_from("<I", records, n * 516)[0]
            disk.seek(block * 512)
            disk.write(records[n * 516 + 4:(n + 1) * 516])
        if torn:
            block = struct.unpack_from("<I", records, count * 516)[0]
            start, end = (0, 256) if torn == "prefix256" else (256, 512)
            disk.seek(block * 512 + start)
            disk.write(records[count * 516 + 4 + start:count * 516 + 4 + end])


def header(image, journal_sector):
    with image.open("rb") as disk:
        disk.seek(journal_sector * 512)
        candidates = [disk.read(512), disk.read(512)]
    valid = [h for h in candidates if h[:8] == b"AFJNL002" and zlib.crc32(h[:508]) == struct.unpack_from("<I", h, 508)[0]]
    return max(valid, key=lambda h: struct.unpack_from("<I", h, 8)[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--exfatprogs", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True, help="Completed --journal powercut report directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    command = cuts.command
    tools = cuts.tools
    report = {"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "cuts": [], "guards": []}
    work = args.output / "work.img"
    crash = args.output / "crash.img"
    pending = args.output / "pending.img"
    result = args.output / "probe.json"
    trace = args.output / "recovery.trace"
    raw = args.output / "checked-volume.img"
    try:
        for cluster in (512, 4096):
            base = args.input / f"append-{cluster}-base.img"
            records = (args.input / f"append-{cluster}.trace").read_bytes()
            points = json.loads((args.input / f"append-{cluster}-checkpoints.json").read_text())
            root = tools.volume(base)
            first = root.find("AFATFS.JNL").dwStartCluster
            tools.close_volume(root)
            with base.open("rb") as disk:
                disk.seek(2048 * 512)
                boot = disk.read(512)
            journal_sector = 2048 + struct.unpack_from("<I", boot, 88)[0] + (first - 2) * (cluster // 512)
            chosen = None
            # Use a cut with several protected sectors and uncommitted metadata.
            for count in range(points["closed"] - 1, points["mount"], -1):
                replay(base, crash, records, count)
                h = header(crash, journal_sector)
                if struct.unpack_from("<I", h, 20)[0] and struct.unpack_from("<I", h, 16)[0] >= 2:
                    chosen = count
                    break
            assert chosen is not None
            shutil.copyfile(crash, work)
            recovered = command([args.binary, work, "recover", "append", trace, result])
            assert recovered["returncode"] == 0, recovered
            recovered_info = json.loads(result.read_text())
            assert not recovered_info["readOnly"] and recovered_info["targetPrefix"]
            recovery_records = trace.read_bytes()
            for count in range(recovered_info["mountWrites"] + 1):
                modes = [None, "prefix256", "suffix256"] if count < recovered_info["mountWrites"] else [None]
                for mode in modes:
                    replay(crash, pending, recovery_records, count, mode)
                    checked = command([args.binary, pending, "recover", "append", "unused", result])
                    info = json.loads(result.read_text()) if checked["returncode"] == 0 else None
                    tools.extract(pending, raw)
                    fsck = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", raw])
                    okay = info and not info["readOnly"] and info["keep"] and info["targetPrefix"] and fsck["returncode"] == 0
                    report["cuts"].append({"cluster": cluster, "write": count, "mode": mode or "atomic",
                                           "probe": info, "fsck": fsck, "passed": bool(okay)})
                    assert okay, report["cuts"][-1]
            # Both superblocks or either part of an in-use record corrupted must
            # produce zero writes, even when another record is otherwise replayable.
            for corruption in ("headers", "descriptor", "before-image", "external-dirty"):
                shutil.copyfile(base if corruption == "external-dirty" else crash, pending)
                with pending.open("r+b") as disk:
                    if corruption == "external-dirty":
                        disk.seek(2048 * 512 + 106); dirty = disk.read(1)[0] | 2
                        disk.seek(2048 * 512 + 106); disk.write(bytes([dirty]))
                    else:
                        offsets = [0, 1] if corruption == "headers" else [2 if corruption == "descriptor" else 3]
                        for sector in offsets:
                            disk.seek((journal_sector + sector) * 512 + 200)
                            value = disk.read(1)[0]
                            disk.seek((journal_sector + sector) * 512 + 200); disk.write(bytes([value ^ 0x5A]))
                guarded = command([args.binary, pending, "recover", "append", "unused", result])
                info = json.loads(result.read_text()) if guarded["returncode"] == 0 else None
                okay = info and info["readOnly"] and info["writes"] == 0 and info["error"] == 4 and info["keep"]
                report["guards"].append({"cluster": cluster, "corruption": corruption, "probe": info, "passed": bool(okay)})
                assert okay, report["guards"][-1]
            print(f"recovery-{cluster}: replay cuts and damaged-log guards passed", flush=True)
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {len(report['cuts'])} recovery cuts, {len(report['guards'])} corruption/ownership guards", flush=True)


if __name__ == "__main__":
    main()
