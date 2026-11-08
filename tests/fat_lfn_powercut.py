"""Replay every completed FAT32 media write and audit the directory/FAT graph independently."""
import argparse
import hashlib
import json
import mmap
import shutil
import struct
import subprocess
from pathlib import Path

from exfat_interop import close_volume, create, pattern, volume, vopen
from FATtools.mkfat import fat_mkfs

SOURCE = "Source long filename with important data.bin"
DESTINATION = "Destination renamed long filename with important data.bin"
MAXIMUM = "R" * 251 + ".bin"
CREATED = "Created long filename spanning directory sectors.bin"
OFFSETS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)


def invoke(binary, image, mode, scenario, trace, result):
    process = subprocess.run([str(binary), str(image), mode, scenario, str(trace), str(result)],
                             capture_output=True, text=True, timeout=60)
    return {"code": process.returncode, "stdout": process.stdout, "stderr": process.stderr}


def records(path):
    data = path.read_bytes()
    assert len(data) % 516 == 0, path
    return [(struct.unpack_from("<I", data, offset)[0], data[offset + 4:offset + 516])
            for offset in range(0, len(data), 516)]


def checksum(name):
    value = 0
    for character in name:
        value = (((value & 1) << 7) + (value >> 1) + character) & 255
    return value


def long_name(entries, short):
    if not entries:
        return None
    count = len(entries)
    if count > 20 or entries[0][0] != count | 0x40:
        return None
    units = []
    for index, entry in enumerate(reversed(entries)):
        if entry[0] != (index + 1 | (0x40 if index + 1 == count else 0)) or entry[12] or entry[26:28] != b"\0\0":
            return None
        if entry[13] != checksum(short):
            return None
        units.extend(struct.unpack_from("<H", entry, offset)[0] for offset in OFFSETS)
    if 0 in units:
        end = units.index(0)
        if any(value != 65535 for value in units[end + 1:]):
            return None
        units = units[:end]
    if not 1 <= len(units) <= 255 or any(value == 65535 for value in units):
        return None
    return bytes().join(struct.pack("<H", value) for value in units).decode("utf-16-le", errors="replace")


def audit(path):
    issues = []
    names = {}
    with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        partition = struct.unpack_from("<I", data, 454)[0]
        boot = partition * 512
        sectors_per_cluster = data[boot + 13]
        reserved = struct.unpack_from("<H", data, boot + 14)[0]
        fat_sectors = struct.unpack_from("<I", data, boot + 36)[0]
        total = struct.unpack_from("<I", data, boot + 32)[0]
        heap = partition + reserved + 2 * fat_sectors
        clusters = (total - heap + partition) // sectors_per_cluster
        root = struct.unpack_from("<I", data, boot + 44)[0]
        fat_bytes = fat_sectors * 512
        first = data[(partition + reserved) * 512:(partition + reserved) * 512 + fat_bytes]
        second = data[(partition + reserved + fat_sectors) * 512:(partition + reserved + fat_sectors) * 512 + fat_bytes]
        if first != second:
            issues.append("fat_mirrors")
        table = struct.unpack(f"<{fat_bytes // 4}I", first)
        owners = {}
        visited = set()

        def chain(head, owner):
            result = []
            current = head
            while current:
                if current < 2 or current >= clusters + 2 or current in result:
                    issues.append("invalid_chain")
                    break
                if current in owners and owners[current] != owner:
                    issues.append("crosslink")
                owners[current] = owner
                result.append(current)
                following = table[current] & 0xFFFFFFF
                if following >= 0xFFFFFF8:
                    break
                if not following:
                    issues.append("referenced_free_cluster")
                    break
                current = following
            return result

        def directory(head, prefix):
            if head in visited:
                issues.append("directory_cycle")
                return
            visited.add(head)
            pending = []
            ended = False
            for cluster in chain(head, prefix):
                start = (heap + (cluster - 2) * sectors_per_cluster) * 512
                for offset in range(start, start + sectors_per_cluster * 512, 32):
                    entry = data[offset:offset + 32]
                    if entry[0] in (0, 0xE5):
                        if pending:
                            issues.append("orphan_lfn")
                            pending = []
                        if entry[0] == 0:
                            ended = True
                            break
                        continue
                    if entry[11] == 15:
                        pending.append(entry)
                        continue
                    name = long_name(pending, entry[:11])
                    if pending and name is None:
                        issues.append("orphan_lfn")
                    pending = []
                    if entry[11] & 8 or entry[:11] in (b".          ", b"..         "):
                        continue
                    if not entry[:8].strip(b" ") or any(value < 32 for value in entry[:11]) or entry[11] & 0xC0:
                        issues.append("invalid_short_entry")
                    if name is None:
                        name = entry[:8].rstrip(b" ").decode("ascii", errors="replace")
                        extension = entry[8:11].rstrip(b" ").decode("ascii", errors="replace")
                        name += "." + extension if extension else ""
                    full_name = prefix + "/" + name
                    head_cluster = struct.unpack_from("<H", entry, 26)[0] | struct.unpack_from("<H", entry, 20)[0] << 16
                    size = struct.unpack_from("<I", entry, 28)[0]
                    names[full_name] = size
                    if entry[11] & 16:
                        if head_cluster < 2:
                            issues.append("invalid_directory")
                        else:
                            directory(head_cluster, full_name)
                    else:
                        allocation = chain(head_cluster, full_name)
                        if size > len(allocation) * sectors_per_cluster * 512:
                            issues.append("short_chain")
                if ended:
                    break
            if pending:
                issues.append("orphan_lfn")

        directory(root, "")
        lost = [cluster for cluster in range(2, clusters + 2)
                if (table[cluster] & 0xFFFFFFF) and cluster not in owners]
        if lost:
            issues.append("lost_clusters")
        keep = names.get("/KEEP.BIN") == 4096
        return {"issues": sorted(set(issues)), "lost_clusters": len(lost), "keep": keep,
                "source": names.get("/" + SOURCE), "destination": names.get("/" + DESTINATION, names.get("/" + MAXIMUM)),
                "created": names.get("/" + CREATED)}


def semantic_issues(entry, completed):
    after = entry["after"]
    scenario = entry["scenario"]
    issues = []
    if entry.get("mount", {}).get("readOnly", True):
        issues.append("unexpected_read_only")
    if scenario.startswith("rename"):
        if (after["source"] is None) == (after["destination"] is None):
            issues.append("rename_missing_or_duplicated")
        expected = 0 if scenario.endswith("empty") else 1703
        if any(value is not None and value != expected for value in (after["source"], after["destination"])):
            issues.append("rename_size")
    if entry["cut"] >= completed["operation"]:
        for field in ("source", "destination", "created"):
            if after[field] != (None if completed[field] < 0 else completed[field]):
                issues.append("acknowledged_" + field)
    return issues


def fixture(path, cluster, scenario, binary):
    raw = path.with_suffix(".volume.img")
    with raw.open("wb") as handle:
        handle.truncate(40 * 1024 * 1024 * (cluster // 512))
    device = vopen(str(raw), "r+b", "disk")
    assert fat_mkfs(device, device.size, params={"fat_bits": 32, "wanted_cluster": cluster,
                                               "reserved_size": 32, "fat32_forbids_low_clusters": True}) == 0
    device.close()
    header = bytearray(512)
    header[450] = 0x0C
    struct.pack_into("<II", header, 454, 1, raw.stat().st_size // 512)
    header[510:512] = b"\x55\xAA"
    with path.open("wb") as output, raw.open("rb") as source:
        output.write(header)
        shutil.copyfileobj(source, output)
    root = volume(path, "r+b")
    try:
        create(root, "KEEP.BIN", pattern(4096, 9))
        create(root, "SURVIVE.BIN", pattern(1701, 67))
        close_volume(root)
        prepared = invoke(binary, path, "prepare", scenario, path.with_suffix(".prepare.trace"), path.with_suffix(".prepare.json"))
        assert prepared["code"] == 0, prepared
        root = volume(path, "r+b")
        # Place the next LFN chain at slot 15, crossing a sector/cluster boundary.
        index = 0
        journal = root.find("AFATFS.JNL")
        last_entry = max(root.find("SURVIVE.BIN")._pos, journal._pos if journal else 0)
        while last_entry // 32 + 1 + index < 15:
            create(root, f"P{index:07}.TMP", b"")
            index += 1
        if scenario not in ("create", "create-data"):
            create(root, SOURCE, b"" if scenario.endswith("empty") else pattern(1703, 31))
    finally:
        close_volume(root)
    baseline = audit(path)
    assert not baseline["issues"] and baseline["keep"], baseline


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cluster", type=int, action="append")
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--torn", action="store_true")
    parser.add_argument("--allow-damage", action="store_true", help="Collect baseline failures without a failing process exit")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"status": "running", "cases": [], "cuts": [], "binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest()}
    try:
        for cluster in args.cluster or (512, 4096):
            for scenario in args.scenario or ("create", "create-data", "rename", "rename-empty", "rename-same", "rename-same-empty", "delete", "delete-empty"):
                folder = args.output / f"{cluster}-{scenario}"
                folder.mkdir(exist_ok=True)
                base = folder / "base.img"
                final = folder / "final.img"
                trace = folder / "writes.trace"
                fixture(base, cluster, scenario, args.binary)
                shutil.copyfile(base, final)
                result = invoke(args.binary, final, "run", scenario, trace, folder / "completed.json")
                assert result["code"] == 0, result
                completed = json.loads((folder / "completed.json").read_text())
                writes = records(trace)
                end = audit(final)
                working = folder / "cut.img"
                shutil.copyfile(base, working)
                prefix = {}
                case = {"cluster": cluster, "scenario": scenario, "writes": len(writes), "cuts": 0, "unclean": 0, "final": end}
                report["cases"].append(case)
                with base.open("rb") as original, working.open("r+b") as replay:
                    def restore(sector):
                        original.seek(sector * 512)
                        replay.seek(sector * 512)
                        replay.write(prefix.get(sector, original.read(512)))

                    def assess(cut, partial):
                        entry = {"cluster": cluster, "scenario": scenario, "cut": cut, "partial": partial}
                        entry["before"] = audit(working)
                        probe_trace = folder / "probe.trace"
                        probe = invoke(args.binary, working, "probe", scenario, probe_trace, folder / "probe.json")
                        entry["probe"] = probe
                        entry["after"] = audit(working)
                        if probe["code"] == 0:
                            entry["mount"] = json.loads((folder / "probe.json").read_text())
                        entry["semantic_issues"] = semantic_issues(entry, completed)
                        case["cuts"] += 1
                        if entry["after"]["issues"] or probe["code"] or entry["semantic_issues"]:
                            case["unclean"] += 1
                        report["cuts"].append(entry)
                        # Restore remount writes to exactly the pre-remount media state.
                        for sector, _ in records(probe_trace):
                            restore(sector)
                        replay.flush()

                    for cut in range(len(writes) + 1):
                        assess(cut, 0)
                        if args.torn and cut < len(writes):
                            sector, data = writes[cut]
                            replay.seek(sector * 512)
                            previous = replay.read(512)
                            for partial in (1, 31, 64, 256, 480, 511):
                                replay.seek(sector * 512)
                                replay.write(data[:partial] + previous[partial:])
                                replay.flush()
                                assess(cut, partial)
                                restore(sector)
                                replay.flush()
                        if cut < len(writes):
                            sector, data = writes[cut]
                            prefix[sector] = data
                            replay.seek(sector * 512)
                            replay.write(data)
                        replay.flush()
                print(f"{cluster}/{scenario}: {case['cuts']} cuts, {case['unclean']} unclean after remount", flush=True)
                (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        report["status"] = "failed" if any(case["unclean"] or case["final"]["issues"] for case in report["cases"]) else "passed"
    except Exception as error:
        report["status"] = "error"
        report["error"] = repr(error)
        raise
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if report["status"] != "passed" and not args.allow_damage:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
