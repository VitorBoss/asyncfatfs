"""Interrupt FAT32 journal recovery, then remount again in a fresh process."""
import argparse
import json
import shutil
import struct
from pathlib import Path

from fat_lfn_powercut import audit, invoke, records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"status": "running", "cases": [], "cuts": []}
    try:
        matrix = json.loads((args.matrix / "report.json").read_text())
        for case in matrix["cases"]:
            name = f"{case['cluster']}-{case['scenario']}"
            folder = args.matrix / name
            writes = records(folder / "writes.trace")
            candidates = [(index + 1, struct.unpack_from("<I", data, 16)[0])
                          for index, (_, data) in enumerate(writes)
                          if data[:8] == b"AFJFT001" and struct.unpack_from("<I", data, 20)[0]
                          and struct.unpack_from("<I", data, 16)[0]]
            selected = sorted({candidates[0][0], max(candidates, key=lambda item: item[1])[0]})
            for operation_cut in selected:
                target = args.output / f"{name}-{operation_cut}"
                target.mkdir(exist_ok=True)
                working = target / "cut.img"
                base = folder / "base.img"
                shutil.copyfile(base, working)
                prefix = dict(writes[:operation_cut])
                with working.open("r+b") as replay:
                    for sector, data in prefix.items():
                        replay.seek(sector * 512)
                        replay.write(data)
                initial = invoke(args.binary, working, "probe", case["scenario"],
                                 target / "recovery.trace", target / "initial.json")
                assert initial["code"] == 0, initial
                recovery = records(target / "recovery.trace")
                item = {"case": name, "operation_cut": operation_cut,
                        "recovery_writes": len(recovery), "cuts": 0, "failed": 0}
                report["cases"].append(item)
                recovery_prefix = {}
                with base.open("rb") as original, working.open("r+b") as replay:
                    def restore(sector):
                        original.seek(sector * 512)
                        replay.seek(sector * 512)
                        replay.write(recovery_prefix.get(sector, prefix.get(sector, original.read(512))))

                    for sector, _ in recovery:
                        restore(sector)
                    replay.flush()
                    for cut in range(len(recovery) + 1):
                        variants = (0, 1, 31, 256, 511) if cut < len(recovery) else (0,)
                        for partial in variants:
                            if partial:
                                sector, data = recovery[cut]
                                replay.seek(sector * 512)
                                previous = replay.read(512)
                                replay.seek(sector * 512)
                                replay.write(data[:partial] + previous[partial:])
                                replay.flush()
                            probe = invoke(args.binary, working, "probe", case["scenario"],
                                           target / "probe.trace", target / "probe.json")
                            after = audit(working)
                            mount = json.loads((target / "probe.json").read_text()) if probe["code"] == 0 else {}
                            failed = bool(probe["code"] or after["issues"] or not after["keep"]
                                          or mount.get("readOnly", True))
                            if case["scenario"].startswith("rename"):
                                failed |= (after["source"] is None) == (after["destination"] is None)
                            item["cuts"] += 1
                            item["failed"] += int(failed)
                            report["cuts"].append({"case": name, "operation_cut": operation_cut,
                                                   "recovery_cut": cut, "partial": partial,
                                                   "failed": failed, "probe": probe, "after": after})
                            for sector, _ in records(target / "probe.trace"):
                                restore(sector)
                            if partial:
                                restore(recovery[cut][0])
                            replay.flush()
                        if cut < len(recovery):
                            sector, data = recovery[cut]
                            recovery_prefix[sector] = data
                            replay.seek(sector * 512)
                            replay.write(data)
                            replay.flush()
                print(f"{name}/{operation_cut}: {item['cuts']} recovery cuts, {item['failed']} failures", flush=True)
                (args.output / "report.json").write_text(json.dumps(report, indent=2))
        report["status"] = "failed" if any(item["failed"] for item in report["cases"]) else "passed"
    except Exception as error:
        report["status"] = "error"
        report["error"] = repr(error)
        raise
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2))
    if report["status"] != "passed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
