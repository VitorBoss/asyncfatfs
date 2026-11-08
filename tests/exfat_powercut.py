"""Replay durable-write prefixes and partial-sector cuts; never treat repair as a clean cut."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import exfat_interop as tools


def command(arguments):
    result = subprocess.run([str(value) for value in arguments], capture_output=True, text=True, timeout=120)
    return {"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def fixture(raw, disk, cluster, scenario, binary=None):
    with raw.open("wb") as handle:
        handle.write(b"\xCC" * (16 * 1024 * 1024))
    device = tools.vopen(str(raw), "r+b", "disk")
    assert tools.exfat_mkfs(device, device.size, params={"wanted_cluster": cluster}) == 0
    device.close()
    tools.wrap(raw, disk)
    root = tools.volume(disk, "r+b")
    tools.create(root, "keep.bin", tools.pattern(4096, 9))
    if binary:
        tools.close_volume(root)
        prepared = command([binary, disk, "prepare", scenario, disk.with_suffix(".prepare.trace"), "unused"])
        assert prepared["returncode"] == 0, prepared
        root = tools.volume(disk, "r+b")
    next_entry = root.find("keep.bin")._pos + 96
    n = 0
    while (next_entry // 32) % 16 != 15:
        name = f"f{n:04d}.bin"
        tools.create(root, name, b"")
        next_entry = root.find(name)._pos + 96
        n += 1
        assert n < 16
    if scenario in ("append", "append-sync", "truncate"):
        tools.create(root, "target.bin", tools.pattern(700 if scenario.startswith("append") else cluster * 3, 11))
        assert (root.find("target.bin")._pos // 32) % 16 == 15
        tools.create(root, "blocker.bin", tools.pattern(cluster, 19))
    tools.close_volume(root)
    tools.extract(disk, raw)


def expected_after_close(image, cluster, scenario):
    root = tools.volume(image)
    try:
        if scenario == "mkdir":
            directory = root.opendir("new directory")
            assert directory is not None
            assert directory.listdir() == []
        else:
            name = "n" * 251 + ".bin" if scenario == "long-create" else "target.bin"
            expected = b"" if scenario == "empty-abort" else tools.pattern(cluster * 2 + 123, 73)
            if scenario.startswith("append"):
                expected = tools.pattern(700, 11) + expected
            assert tools.read(root, name) == expected
    finally:
        tools.close_volume(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exfatprogs", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=tools.ROOT / "tests/build/powercut")
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--journal", action="store_true", help="Provision the journal, then require automatic recovery without fsck repair")
    parser.add_argument("--torn", action="store_true")
    parser.add_argument("--repair", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--cluster", type=int, action="append")
    parser.add_argument("--scenario", action="append", choices=["new", "long-create", "append", "append-sync", "truncate", "mkdir", "empty-abort"])
    args = parser.parse_args()
    if args.runtime_dir:
        import os
        os.environ["PATH"] = str(args.runtime_dir.resolve()) + os.pathsep + os.environ.get("PATH", "")
    if args.repair is None:
        args.repair = not args.baseline
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"binary_sha256": hashlib.sha256(args.binary.read_bytes()).hexdigest(), "baseline": args.baseline,
              "journal": args.journal, "source_sha256": {
                  name: hashlib.sha256((tools.ROOT / "lib" / name).read_bytes()).hexdigest()
                  for name in ("asyncfatfs.c", "asyncfatfs_journal.h", "asyncfatfs_journal.inc", "fat_standard.c")},
              "cases": [], "summary": {"cuts": 0, "structural_errors": 0, "unmarked_errors": 0,
              "dirty_mounts_wrote": 0, "keep_failures": 0, "close_failures": 0, "sync_failures": 0, "premature_flush": 0,
              "repair_checks": 0, "repair_failures": 0, "repair_target_prefix_lost": 0,
              "recovery_failures": 0, "recovery_fsck_errors": 0, "recovery_target_prefix_lost": 0}}
    report["summary"]["protected_shutdown_cuts"] = 0
    report["summary"]["protected_startup_cuts"] = 0
    try:
        for cluster in args.cluster or [512, 4096]:
            for scenario in args.scenario or ["new", "long-create", "append", "append-sync", "truncate", "mkdir", "empty-abort"]:
                label = f"{scenario}-{cluster}"
                raw = args.output / (label + "-base-volume.img")
                base = args.output / (label + "-base.img")
                work = args.output / "work.img"
                trace = args.output / (label + ".trace")
                checkpoint = args.output / (label + "-checkpoints.json")
                fixture(raw, base, cluster, scenario, args.binary if args.journal else None)
                baseline = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", raw])
                assert baseline["returncode"] == 0, baseline
                shutil.copyfile(base, work)
                execution = command([args.binary, work, cluster, scenario, trace, checkpoint])
                assert execution["returncode"] == 0, execution
                points = json.loads(checkpoint.read_text())
                records = trace.read_bytes()
                assert len(records) == points["writes"] * 516
                if points["prematureFlush"]:
                    report["summary"]["premature_flush"] += 1
                cut_disk = args.output / "cut.img"
                cut_raw = args.output / "cut-volume.img"
                probe_disk = args.output / "probe.img"
                probe_result = args.output / "probe.json"
                shutil.copyfile(base, cut_disk); shutil.copyfile(raw, cut_raw)
                case = {"name": label, "checkpoints": points, "cuts": []}
                report["cases"].append(case)
                modes = ["atomic", "prefix256", "suffix256"] if args.torn else ["atomic"]
                checked_dirty_only = False
                for index in range(points["writes"] + 1):
                    for mode in modes:
                        if mode != "atomic" and index == points["writes"]:
                            continue
                        backups = []
                        if mode != "atomic":
                            block = struct.unpack_from("<I", records, index * 516)[0]
                            data = records[index * 516 + 4:(index + 1) * 516]
                            start, end = (0, 256) if mode == "prefix256" else (256, 512)
                            for path, offset in ((cut_disk, block * 512), (cut_raw, (block - 2048) * 512)):
                                assert offset >= 0
                                with path.open("r+b") as target:
                                    target.seek(offset); old = target.read(512)
                                    backups.append((path, offset, old))
                                    target.seek(offset + start); target.write(data[start:end])
                        with cut_disk.open("rb") as handle:
                            handle.seek(2048 * 512 + 106); dirty = bool(handle.read(1)[0] & 2)
                        checked = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", cut_raw])
                        structural = checked["returncode"] != 0
                        shutil.copyfile(cut_disk, probe_disk)
                        probed = command([args.binary, probe_disk, "recover" if args.journal else "probe", scenario, "unused", probe_result])
                        probe = json.loads(probe_result.read_text()) if probed["returncode"] == 0 else None
                        recovery = None
                        if args.journal:
                            recovered_raw = args.output / "recovered-volume.img"
                            tools.extract(probe_disk, recovered_raw)
                            verified = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", recovered_raw])
                            protected_shutdown = bool(probe and probe["readOnly"] and index > points["closed"]
                                and not structural and probe["writes"] == 0 and probe["error"] == 4)
                            protected_startup = bool(probe and probe["readOnly"] and index <= points["mount"]
                                and not structural and probe["writes"] == 0 and probe["error"] == 4)
                            report["summary"]["protected_shutdown_cuts"] += int(protected_shutdown)
                            report["summary"]["protected_startup_cuts"] += int(protected_startup)
                            report["summary"]["recovery_failures"] += int(not probe or (probe["readOnly"] and not protected_shutdown and not protected_startup))
                            report["summary"]["recovery_fsck_errors"] += int(verified["returncode"] != 0)
                            if scenario.startswith("append"):
                                report["summary"]["recovery_target_prefix_lost"] += int(not probe or not probe["targetPrefix"])
                            recovery = {"verified": verified}
                        elif args.repair and (structural or (dirty and not checked_dirty_only)):
                            if not structural:
                                checked_dirty_only = True
                            repair_raw = args.output / "repair-volume.img"
                            repair_disk = args.output / "repair.img"
                            shutil.copyfile(cut_raw, repair_raw)
                            repair = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-y", repair_raw])
                            verified = command([args.exfatprogs, "--unpin-program=fsck.exfat", "-n", repair_raw])
                            tools.wrap(repair_raw, repair_disk)
                            remounted = command([args.binary, repair_disk, "probe", scenario, "unused", probe_result])
                            repaired_probe = json.loads(probe_result.read_text()) if remounted["returncode"] == 0 else None
                            okay = verified["returncode"] == 0 and repaired_probe and repaired_probe["keep"] and not repaired_probe.get("readOnly", True)
                            report["summary"]["repair_checks"] += 1
                            report["summary"]["repair_failures"] += int(not okay)
                            if scenario.startswith("append") and repaired_probe and not repaired_probe["targetPrefix"]:
                                report["summary"]["repair_target_prefix_lost"] += 1
                            recovery = {"repair": repair, "verified": verified, "remount": remounted, "probe": repaired_probe}
                        close_failure = False
                        sync_failure = False
                        durable_image = probe_disk if args.journal else cut_disk
                        if mode == "atomic" and index >= points["closed"]:
                            try:
                                expected_after_close(durable_image, cluster, scenario)
                            except Exception:
                                close_failure = True
                        if mode == "atomic" and points.get("synced") and index >= points["synced"]:
                            try:
                                expected_after_close(durable_image, cluster, scenario)
                            except Exception:
                                sync_failure = True
                        summary = report["summary"]
                        summary["cuts"] += 1
                        summary["structural_errors"] += int(structural)
                        summary["unmarked_errors"] += int(structural and not dirty)
                        summary["dirty_mounts_wrote"] += int(bool(not args.journal and dirty and probe is not None and
                            (probe["writes"] or not probe["protected"] or not probe.get("readOnly", False) or probe.get("error") != 4)))
                        summary["keep_failures"] += int(probe is None or not probe["keep"])
                        summary["close_failures"] += int(close_failure)
                        summary["sync_failures"] += int(sync_failure)
                        case["cuts"].append({"write": index, "mode": mode, "dirty": dirty, "fsck": checked,
                                             "probe": probe, "probe_result": probed, "close_failure": close_failure,
                                             "sync_failure": sync_failure, "recovery": recovery})
                        for path, offset, data in backups:
                            with path.open("r+b") as target:
                                target.seek(offset); target.write(data)
                    if index < points["writes"]:
                        block = struct.unpack_from("<I", records, index * 516)[0]
                        data = records[index * 516 + 4:(index + 1) * 516]
                        for path, offset in ((cut_disk, block * 512), (cut_raw, (block - 2048) * 512)):
                            assert offset >= 0
                            with path.open("r+b") as target:
                                target.seek(offset); target.write(data)
                print(f"{label}: {points['writes']} durable writes, {len(case['cuts'])} cuts", flush=True)
        print(json.dumps(report["summary"], indent=2), flush=True)
        if not args.baseline:
            for key in ("unmarked_errors", "dirty_mounts_wrote", "keep_failures", "close_failures", "sync_failures", "premature_flush", "repair_failures", "recovery_failures", "recovery_fsck_errors", "recovery_target_prefix_lost"):
                assert report["summary"][key] == 0, (key, report["summary"][key])
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
