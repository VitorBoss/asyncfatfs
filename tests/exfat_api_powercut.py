"""Cut every durable write of exFAT request APIs and their undo recovery.

Regular test images only. A clean fsck exit and an entirely old/new state are
required; fsck repair is never used. Completed requests must retain the new state.
"""
import argparse
import copy
import hashlib
import json
import shutil
import struct
from pathlib import Path

import exfat_powercut as cuts
from exfat_journal_recovery import header, replay

tools = cuts.tools
SCENARIOS = ("api-truncate-contiguous", "api-truncate-fragmented", "api-truncate-vdl",
             "api-truncate-zero", "api-truncate-eof", "api-rename",
             "api-rename-directory", "api-expand", "api-expand-hint")
NEW_DIRECTORY = "renamed directory with a longer ASCII name"
LONG_NAME = "n" * 251 + ".bin"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(image, scenario):
    root = tools.volume(image)
    entries = {}

    def walk(directory, prefix=""):
        for name in directory.listdir():
            if name.upper() == "AFATFS.JNL":
                continue
            entry = directory.find(name)
            path = prefix + name
            size, valid = entry.u64DataLength, entry.u64ValidDataLength
            info = {"size": size, "valid": valid, "first": entry.Start(),
                    "contiguous": bool(size and entry.IsContig()), "attributes": entry.wFileAttributes,
                    "created": entry.dwCTime}
            if not (name == "target.bin" and (scenario.startswith("api-truncate") or scenario.startswith("api-expand"))):
                info["modified"] = entry.dwMTime
            entries[path] = info
            if entry.IsDir():
                walk(directory.opendir(name), path + "/")
            else:
                raw = tools.read(directory, name)
                info["sha256"] = digest(raw[:valid] + bytes(size - valid))

    try:
        walk(root)
        return {"entries": entries, "free": root.boot.bitmap.free_clusters}
    finally:
        tools.close_volume(root)


def fixture(raw, base, cluster, scenario, binary):
    cuts.fixture(raw, base, cluster, scenario, binary)
    root = tools.volume(base, "r+b")
    try:
        if scenario == "api-rename-directory":
            directory = root.mkdir("target directory")
            tools.create(directory, "child.bin", tools.pattern(1203, 13))
        elif scenario.startswith("api-expand"):
            tools.create(root, "target.bin", b"")
        elif scenario == "api-truncate-fragmented":
            file = root.create("target.bin", prealloc=1)
            file.write(tools.pattern(cluster, 11))
            tools.create(root, "blocker.bin", tools.pattern(cluster, 19))
            file.write(tools.pattern(cluster * 3 + 123, (cluster * 17 + 11) & 255))
            file.close()
            assert not root.find("target.bin").IsContig()
        else:
            tools.create(root, "target.bin", tools.pattern(cluster * 4 + 123, 11))
            assert root.find("target.bin").IsContig()
            if scenario == "api-truncate-vdl":
                entry = root.find("target.bin")
                entry.u64ValidDataLength = cluster + 37
                root.stream.seek(entry._pos)
                root.stream.write(entry.pack())
                root._update_dirtable(entry)
        journal = root.find("AFATFS.JNL")
        journal_first = journal.Start()
    finally:
        tools.close_volume(root)
    tools.extract(base, raw)
    with raw.open("rb") as source:
        boot = source.read(512)
    return 2048 + struct.unpack_from("<I", boot, 88)[0] + (journal_first - 2) * (cluster // 512)


def expected(before, actual, cluster, scenario):
    result = copy.deepcopy(before)
    entries = result["entries"]
    if scenario.startswith("api-truncate"):
        target = entries["target.bin"]
        old_clusters = (target["size"] + cluster - 1) // cluster
        size = 0 if scenario.endswith("zero") else target["size"] if scenario.endswith("eof") else cluster + 17
        target.update(size=size, valid=min(size, target["valid"]), sha256=digest(tools.pattern(size, 11)))
        if not size:
            target.update(first=0, contiguous=False)
        result["free"] += old_clusters - (size + cluster - 1) // cluster
    elif scenario == "api-rename":
        entries[LONG_NAME] = entries.pop("target.bin")
        result["free"] = actual["free"]  # Destination directory growth is implementation-dependent.
    elif scenario == "api-rename-directory":
        for name in list(entries):
            if name == "target directory" or name.startswith("target directory/"):
                entries[NEW_DIRECTORY + name[len("target directory"):]] = entries.pop(name)
        result["free"] = actual["free"]
    elif scenario == "api-expand":
        size = cluster * 4 + 123
        target = entries["target.bin"]
        assert actual["entries"]["target.bin"]["first"] >= 2
        target.update(size=size, valid=0, first=actual["entries"]["target.bin"]["first"],
                      contiguous=True, sha256=digest(bytes(size)))
        result["free"] -= 5
    assert actual == result, {"scenario": scenario, "expected": result, "actual": actual}
    return result


def checked(command):
    result = cuts.command(command)
    assert result["returncode"] == 0, result
    return result


def recover(args, image, raw, result_file, scenario, before, after, require_after=False, trace="unused"):
    run = checked([args.binary, image, "recover", scenario, trace, result_file])
    info = json.loads(result_file.read_text())
    tools.extract(image, raw)
    fsck = checked([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", raw])
    assert ": clean." in fsck["stdout"], fsck
    actual = snapshot(image, scenario)
    assert actual == after if require_after else actual in (before, after), {"probe": info, "actual": actual}
    return info, fsck, "after" if actual == after else "before"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--exfatprogs", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("tests/build/api-powercut"))
    parser.add_argument("--cluster", type=int, action="append", choices=(512, 4096))
    parser.add_argument("--scenario", action="append", choices=SCENARIOS)
    parser.add_argument("--no-replay", action="store_true", help="Skip additional power cuts during undo replay")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"status": "incomplete", "binary_sha256": digest(args.binary.read_bytes()),
              "exfatprogs_sha256": digest(args.exfatprogs.read_bytes()),
              "source_sha256": {str(path.relative_to(tools.ROOT)): digest(path.read_bytes())
                                for folder in ("lib", "tests") for path in sorted((tools.ROOT / folder).glob("*"))
                                if path.suffix in (".c", ".h", ".inc", ".py")},
              "cases": [], "cuts": 0, "replay_cuts": 0, "protected_handoffs": 0}
    work, cut, probe = [args.output / name for name in ("work.img", "cut.img", "probe.img")]
    raw, result_file = [args.output / name for name in ("checked-volume.img", "probe.json")]
    try:
        for cluster in args.cluster or (512, 4096):
            for scenario in args.scenario or SCENARIOS:
                label = f"{scenario}-{cluster}"
                base = args.output / f"{label}-base.img"
                journal_sector = fixture(raw, base, cluster, scenario, args.binary)
                checked([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", raw])
                before = snapshot(base, scenario)
                trace, checkpoints = args.output / f"{label}.trace", args.output / f"{label}.json"
                shutil.copyfile(base, work)
                checked([args.binary, work, cluster, scenario, trace, checkpoints])
                points = json.loads(checkpoints.read_text())
                tools.extract(work, raw)
                checked([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", raw])
                after = expected(before, snapshot(work, scenario), cluster, scenario)
                records = trace.read_bytes()
                assert len(records) == points["writes"] * 516
                case = {"name": label, "checkpoints": points, "cuts": [], "replay": []}
                report["cases"].append(case)
                shutil.copyfile(base, cut)
                replay_base, replay_trace = args.output / f"{label}-replay.img", args.output / f"{label}-replay.trace"
                chosen = False
                for index in range(points["writes"] + 1):
                    for mode in ((None, "prefix256", "suffix256") if index < points["writes"] else (None,)):
                        shutil.copyfile(cut, probe)
                        if mode:
                            apply_record(probe, records, index, mode)
                        info, fsck, state = recover(args, probe, raw, result_file, scenario, before, after,
                                                    index >= points["operation"])
                        protected = info["readOnly"]
                        assert not protected or (info["writes"] == 0 and info["error"] == 4
                            and (index <= points["mount"] or index > points["closed"])), info
                        report["protected_handoffs"] += int(protected)
                        case["cuts"].append({"write": index, "mode": mode or "atomic", "state": state,
                                             "probe": info, "fsck": fsck})
                        report["cuts"] += 1
                        if not chosen and not mode and not args.no_replay and index < points["operation"]:
                            h = header(cut, journal_sector)
                            if struct.unpack_from("<I", h, 20)[0] and struct.unpack_from("<I", h, 16)[0] >= 2:
                                shutil.copyfile(cut, replay_base); shutil.copyfile(cut, probe)
                                replay_info, _, replay_state = recover(args, probe, raw, result_file, scenario, before, after, trace=replay_trace)
                                assert replay_state == "before", "An active undo log must restore the original state"
                                chosen = True
                    if index < points["writes"]:
                        apply_record(cut, records, index)
                if chosen:
                    undo_records = replay_trace.read_bytes()
                    for index in range(replay_info["mountWrites"] + 1):
                        for mode in ((None, "prefix256", "suffix256") if index < replay_info["mountWrites"] else (None,)):
                            replay(replay_base, probe, undo_records, index, mode)
                            info, fsck, state = recover(args, probe, raw, result_file, scenario, before, after)
                            assert not info["readOnly"] and state == "before", info
                            case["replay"].append({"write": index, "mode": mode or "atomic", "state": state,
                                                   "probe": info, "fsck": fsck})
                            report["replay_cuts"] += 1
                print(f"PASS {label}: {len(case['cuts'])} operation cuts, {len(case['replay'])} replay cuts", flush=True)
        report["status"] = "passed"
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {report['cuts']} operation cuts, {report['replay_cuts']} interrupted-replay cuts", flush=True)


def apply_record(image, records, index, mode=None):
    sector = struct.unpack_from("<I", records, index * 516)[0]
    start, end = (0, 256) if mode == "prefix256" else (256, 512) if mode == "suffix256" else (0, 512)
    with image.open("r+b") as target:
        target.seek(sector * 512 + start)
        target.write(records[index * 516 + 4 + start:index * 516 + 4 + end])


if __name__ == "__main__":
    main()
