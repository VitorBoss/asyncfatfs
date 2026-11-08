"""Format externally, exercise AsyncFatFS, then verify with FATtools and fsck.exfat.

No physical disks are opened. All media are regular files under --output.
FATtools 1.1.23 is a test dependency; exfatprogs is supplied by the caller.
"""
import argparse
import hashlib
from importlib.metadata import version
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMMAND_LOG = []
sys.path.insert(0, str(ROOT / "tests/build/vendor"))
from FATtools.Volume import vopen  # noqa: E402
from FATtools.FAT import Chain  # noqa: E402
from FATtools.mkfat import exfat_mkfs  # noqa: E402
from FATtools.partutils import partition  # noqa: E402


def pattern(count, tag):
    return bytes((i * 17 + tag) & 255 for i in range(count))


def run(command):
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(ROOT / "tests/build/vendor") + os.pathsep + environment.get("PYTHONPATH", "")
    result = subprocess.run([str(arg) for arg in command], capture_output=True, text=True, timeout=180, env=environment)
    COMMAND_LOG.append({"argv": [str(arg) for arg in command], "returncode": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr})
    print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}")
    return result.stdout + result.stderr


def check_volume(tools, image):
    output = run([tools, "--unpin-program=fsck.exfat", "-n", image])
    assert ": clean." in output, "fsck must explicitly report a clean filesystem"
    return output


def volume(path, mode="rb"):
    root = vopen(str(path), mode)
    if not hasattr(root, "listdir"):
        raise RuntimeError(f"FATtools could not open {path}: {root}")
    return root


def create(root, name, data):
    handle = root.create(name)
    assert handle.IsValid, name
    handle.write(data)
    handle.close()


def close_volume(root):
    # Dirtable.close() alone leaves FATtools' disk-sector cache open until process exit.
    root.close()
    device = root.boot.stream
    if hasattr(device, "disk"):
        device = device.disk
    device.close()


def read(root, name):
    # FATtools 1.1.23's high-level open() rejects names >242 characters and has an entry-type
    # dispatch bug. Its directory parser and Chain reader independently support the full name.
    entry = root.find(name)
    assert entry is not None, name
    if entry.u64DataLength == 0:
        return b""
    chain = Chain(root.boot, root.fat, entry.Start(), entry.u64DataLength, nofat=entry.IsContig())
    return bytes(chain.read())


def seed(image, cluster_size):
    root = volume(image, "r+b")
    create(root, "external seed.bin", pattern(20003, 11))
    create(root, "external empty.bin", b"")
    create(root, "external truncate.bin", pattern(cluster_size + 99, 17))
    create(root, "external delete.bin", pattern(cluster_size + 7, 23))
    fragmented = root.create("external fragmented.bin", prealloc=1)
    blocker = root.create("external blocker.bin", prealloc=1)
    blocker.write(pattern(cluster_size, 41)); blocker.close()
    fragmented.write(pattern(cluster_size * 3, 37))
    fragmented.close()
    entry = root.find("external fragmented.bin")
    assert not entry.IsContig(), "fixture must contain an externally generated fragmented FAT chain"
    assert root.find("external seed.bin").IsContig(), "fixture must contain a NoFatChain file"
    directory = root.mkdir("External directory")
    create(directory, "external child.bin", pattern(91, 49))
    close_volume(root)


def wrap(volume_image, disk_image, scheme="mbr"):
    start = 2048
    size = volume_image.stat().st_size
    assert size % 512 == 0
    if scheme == "gpt":
        with disk_image.open("wb") as target:
            target.truncate(start * 512 + size + 33 * 512)
        device = vopen(str(disk_image), "r+b", "disk")
        partition(device, "gpt")  # Independent GPT writer; includes primary/backup arrays and CRCs.
        with disk_image.open("r+b") as target, volume_image.open("rb") as source:
            target.seek(start * 512)
            shutil.copyfileobj(source, target)
        return
    mbr = bytearray(512)
    mbr[450] = 7
    struct.pack_into("<II", mbr, 454, start, size // 512)
    mbr[510:512] = b"\x55\xaa"
    with disk_image.open("wb") as target, volume_image.open("rb") as source:
        target.write(mbr)
        target.seek(start * 512)
        shutil.copyfileobj(source, target)


def extract(disk_image, volume_image):
    with disk_image.open("rb") as source:
        source.seek(454)
        start, count = struct.unpack("<II", source.read(8))
        source.seek(450)
        if source.read(1) == b"\xee":
            source.seek(512 + 72)
            table = struct.unpack("<Q", source.read(8))[0]
            source.seek(table * 512 + 32)
            start, last = struct.unpack("<QQ", source.read(16))
            count = last - start + 1
        source.seek(start * 512)
        with volume_image.open("wb") as target:
            remaining = count * 512
            while remaining:
                block = source.read(min(remaining, 1024 * 1024))
                assert block
                target.write(block)
                remaining -= len(block)


def verify(image, cluster_size):
    root = volume(image)
    assert read(root, "external seed.bin") == pattern(20003, 11) + pattern(cluster_size * 2 + 77, 21)
    assert read(root, "external empty.bin") == b""
    assert read(root, "external fragmented.bin") == pattern(cluster_size * 3, 37)
    assert read(root, "external truncate.bin") == pattern(333, 17)
    assert root.find("external delete.bin") is None
    assert read(root, "library output.bin") == pattern(cluster_size * 3 + 123, 73)
    assert read(root, "n" * 251 + ".bin") == pattern(777, 55)
    assert read(root, "library empty.bin") == b""
    children = cluster_size // 96 + 20
    for name in ("External directory", "Library directory"):
        directory = root.opendir(name)
        assert directory
        entry = root.find(name)
        assert entry.u64DataLength > cluster_size, "directory must grow beyond its original allocation"
        assert entry.u64ValidDataLength == entry.u64DataLength
        for n in range(children):
            assert read(directory, f"child{n:04d}.bin") == pattern(37, n)
    close_volume(root)
    print("PASS FATtools independent byte-for-byte verification", flush=True)


def external_roundtrip(image, cluster_size):
    root = volume(image, "r+b")
    create(root, "external roundtrip.bin", pattern(cluster_size + 93, 91))
    handle = root.open("library output.bin")
    assert handle.IsValid
    # FATtools Chain.seek(..., 2) is relative to allocated size, not logical EOF.
    handle.seek(handle.Entry.u64DataLength)
    handle.write(pattern(cluster_size + 93, 91))
    handle.close()
    close_volume(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exfatprogs", type=Path, required=True, help="Multicall Windows port of exfatprogs")
    parser.add_argument("--binary", type=Path, default=ROOT / "tests/build/test_exfat_external.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "tests/build/external")
    parser.add_argument("--runtime-dir", type=Path, help="Directory containing the Windows AddressSanitizer runtime DLL")
    parser.add_argument("--partition-table", choices=["mbr", "gpt"], default="mbr")
    parser.add_argument("--case", action="append", choices=["mkfs-512", "mkfs-4096", "mkfs-32768", "mkfs-65536", "mkfs-packed-4096", "fattools-4096", "fattools-65536"])
    args = parser.parse_args()
    if args.runtime_dir:
        os.environ["PATH"] = str(args.runtime_dir.resolve()) + os.pathsep + os.environ.get("PATH", "")
    args.output.mkdir(parents=True, exist_ok=True)
    tools = args.exfatprogs.resolve()
    cases = args.case or ["mkfs-512", "mkfs-4096", "mkfs-32768", "mkfs-65536", "mkfs-packed-4096", "fattools-4096", "fattools-65536"]
    report = {"fattools_version": version("FATtools"), "partition_table": args.partition_table,
              "exfatprogs_binary_sha256": hashlib.sha256(tools.read_bytes()).hexdigest(),
              "test_binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              "source_sha256": {name: hashlib.sha256((ROOT / "lib" / name).read_bytes()).hexdigest()
                                for name in ("asyncfatfs.c", "asyncfatfs_journal.h", "asyncfatfs_journal.inc", "fat_standard.c", "fat_standard.h")},
              "cases": [], "commands": COMMAND_LOG}
    try:
        for case in cases:
            print(f"\n=== {case} ===", flush=True)
            cluster_size = int(case.rsplit("-", 1)[1])
            raw = args.output / f"{case}-volume.img"
            image = args.output / f"{case}-disk.img"
            with raw.open("wb") as handle:
                handle.truncate((128 if cluster_size == 65536 else 64) * 1024 * 1024)
            if case.startswith("mkfs"):
                command = [tools, "--unpin-program=mkfs.exfat", "-c", cluster_size, "-L", "ASYNCFATFS"]
                if "packed" in case:
                    command.append("--pack-bitmap")
                run(command + [raw])
                wrap(raw, image, args.partition_table)  # No bytes in the formatter's volume are patched.
            else:
                device = vopen(str(raw), "r+b", "disk")
                assert exfat_mkfs(device, device.size, params={"wanted_cluster": cluster_size, "show_info": 1}) == 0
                device.close()
                wrap(raw, image, args.partition_table)
            seed(image, cluster_size)
            extract(image, raw)
            baseline = check_volume(tools, raw)
            run([args.binary.resolve(), image, "exercise", cluster_size])
            extract(image, raw)
            after = check_volume(tools, raw)
            verify(image, cluster_size)
            external_roundtrip(image, cluster_size)
            extract(image, raw)
            roundtrip = check_volume(tools, raw)
            run([args.binary.resolve(), image, "verify-roundtrip", cluster_size])
            extract(image, raw)
            final = check_volume(tools, raw)
            report["cases"].append({"name": case, "cluster_size": cluster_size, "status": "passed",
                                    "baseline_fsck": baseline.strip(), "library_fsck": after.strip(),
                                    "roundtrip_fsck": roundtrip.strip(), "final_fsck": final.strip()})
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {len(report['cases'])} external-format interoperability cases", flush=True)


if __name__ == "__main__":
    main()
