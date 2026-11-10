"""Check first-journal bootstrap cuts: recover trusted logs, protect ambiguous ones."""
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
    report = {"status": "running", "cuts": []}
    try:
        for cluster in (512, 4096):
            folder = args.output / str(cluster)
            folder.mkdir(exist_ok=True)
            source = args.matrix / f"{cluster}-create"
            header = next(data for _, data in records(source / "writes.trace") if data[:8] == b"AFJFT001")
            head = struct.unpack_from("<I", header, 32)[0]
            sectors = struct.unpack_from("<I", header, 36)[0]
            partition = struct.unpack_from("<I", header, 44)[0]
            fat_start = struct.unpack_from("<I", header, 48)[0]
            heap = struct.unpack_from("<I", header, 52)[0]
            journal = heap + (head - 2) * (cluster // 512)
            virgin = folder / "virgin.img"
            final = folder / "final.img"
            working = folder / "cut.img"
            shutil.copyfile(source / "base.img", virgin)
            with virgin.open("r+b") as media:
                media.seek(partition * 512)
                boot = media.read(512)
                fat_sectors = struct.unpack_from("<I", boot, 36)[0]
                root = struct.unpack_from("<I", boot, 44)[0]
                root_start = (heap + (root - 2) * (cluster // 512)) * 512
                media.seek(root_start)
                entries = media.read(cluster)
                offset = next(index for index in range(0, len(entries), 32)
                              if entries[index:index + 11] == b"AFATFS  JNL")
                media.seek(root_start + offset)
                media.write(b"\xE5")
                for table in (0, 1):
                    media.seek((fat_start + table * fat_sectors) * 512 + head * 4)
                    media.write(b"\0" * (sectors // (cluster // 512) * 4))
            assert not audit(virgin)["issues"]
            shutil.copyfile(virgin, final)
            prepared = invoke(args.binary, final, "prepare", "create", folder / "prepare.trace", folder / "prepare.json")
            assert prepared["code"] == 0, prepared
            writes = records(folder / "prepare.trace")
            selected = {0, 1, 2, len(writes)}
            zeros = [index for index, (_, data) in enumerate(writes) if not any(data)]
            for index in (zeros[0], zeros[len(zeros) // 2], zeros[-1]):
                selected.update((index, index + 1))
            metadata = [index for index, (sector, data) in enumerate(writes)
                        if not journal <= sector < journal + sectors or data[:8] == b"AFJFT001"]
            for index in metadata:
                selected.update((index, index + 1))
            shutil.copyfile(virgin, working)
            prefix = {}
            with virgin.open("rb") as original, working.open("r+b") as replay:
                for cut in range(len(writes) + 1):
                    if cut in selected:
                        probe = invoke(args.binary, working, "guard", "create", folder / "probe.trace", folder / "probe.json")
                        result = json.loads((folder / "probe.json").read_text()) if probe["code"] == 0 else {}
                        after = audit(working)
                        protected = result.get("readOnly") and result.get("mountError") == 4 and result.get("writes") == 0
                        recovered = not result.get("readOnly", True) and not after["issues"]
                        passed = not probe["code"] and (protected or recovered)
                        report["cuts"].append({"cluster": cluster, "cut": cut, "passed": passed,
                                               "protected": protected, "recovered": recovered,
                                               "probe": probe, "result": result, "after": after})
                        assert passed, report["cuts"][-1]
                        for sector, _ in records(folder / "probe.trace"):
                            original.seek(sector * 512)
                            replay.seek(sector * 512)
                            replay.write(prefix.get(sector, original.read(512)))
                    if cut < len(writes):
                        sector, data = writes[cut]
                        prefix[sector] = data
                        replay.seek(sector * 512)
                        replay.write(data)
                    replay.flush()
            print(f"bootstrap/{cluster}: {len(selected)} selected cuts passed", flush=True)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "error"
        report["error"] = repr(error)
        raise
    finally:
        (args.output / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
