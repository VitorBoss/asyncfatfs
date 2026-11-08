"""Validate optional multi-read resource and benchmark evidence."""
import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path

from pending_allocation_report import memory_result, pic_frames
from upstream_comparison_report import coff_layout


def assembly_layout(path):
    body = path.read_text(encoding="utf-8-sig").split("_comparison_layout:", 1)[1]
    values = list(map(int, re.findall(r"^\s*\.word\s+(\d+)", body, re.M)[:3]))
    assert len(values) == 3
    return values


def summarize(root, variant, directory=None):
    directory = directory if directory is not None else root / f"{variant}-64"
    batches = {}
    audit = (directory / "audit.log").read_text(encoding="utf-8-sig")
    for line in audit.splitlines():
        if line.startswith("READ_BATCH,"):
            fields = line.split(",")
            batches[(fields[1], *map(int, fields[2:7]))] = tuple(map(int, fields[7:10]))
        if line.startswith("MIRROR,") and variant in ("default", "after"):
            assert line.split(",")[-1] == "0"
    groups = {}
    with (directory / "results.csv").open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            key = (row["phase"], *map(int, (row["profile"], row["cluster_sectors"], row["chunk_bytes"], row["files"])))
            groups.setdefault(key, []).append(row)
    assert len(groups) == 32
    results = {}
    for key, rows in groups.items():
        assert len(rows) == 7 and {int(row["repetition"]) for row in rows} == set(range(7))
        result = {}
        for field in ("payload_bytes", "reads", "writes", "metadata_reads", "metadata_writes", "virtual_us", "polls"):
            samples = {int(row[field]) for row in rows}
            assert len(samples) == 1, (variant, key, field, samples)
            result[field] = samples.pop()
        batch_samples = {batches[(*key, int(row["repetition"]))] for row in rows}
        assert len(batch_samples) == 1
        result["read_sessions"], result["batched_read_blocks"], result["read_stops"] = batch_samples.pop()
        result["read_commands"] = result["reads"] - result["batched_read_blocks"] + result["read_sessions"]
        result["model_mib_s"] = result["payload_bytes"] / 1048576 / (result["virtual_us"] / 1000000)
        result["host_mib_s"] = statistics.median(int(row["payload_bytes"]) / 1048576 /
                                                float(row["host_seconds"]) for row in rows)
        results[key] = result
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory
    resources = {}
    stacks = {}
    for abi in ("32", "64", "16-normal", "16-short"):
        memory = {variant: memory_result(root, variant, abi) for variant in ("before", "default", "after")}
        assert memory["before"] == memory["default"] == memory["after"], (abi, memory)
        if abi in ("32", "64"):
            backend = {variant: list(coff_layout(root / f"{variant}-{abi}/backend-layout.obj"))
                       for variant in ("before", "after")}
        else:
            backend = {variant: assembly_layout(root / f"{variant}-{abi}/backend-layout.s")
                       for variant in ("before", "after")}
            for component in ("layout.s", "backend-layout.s"):
                before = pic_frames(root / f"before-{abi}/{component}")
                after = pic_frames(root / f"after-{abi}/{component}")
                increases = {name: [size, after[name]] for name, size in before.items()
                             if name in after and after[name] > size}
                assert not increases, (abi, component, increases)
                entry = dict(common_function_frame_increases=increases,
                             new_frames={name: size for name, size in after.items() if name not in before})
                if component == "layout.s":
                    # Both new scans are leaves, called sequentially by cacheSector.
                    assert max(after["_afatfs_readLimit"], after["_afatfs_readExtent"]) + 4 <= (
                        before["_afatfs_allocateCacheSector"] + 4 + before["_afatfs_cacheSectorInit"] + 4)
                    entry["cache_sector_frame"] = [before["_afatfs_cacheSector"], after["_afatfs_cacheSector"]]
                stacks[f"{abi}/{component}"] = entry
        assert backend["before"] == backend["after"], (abi, backend)
        resources[abi] = dict(library=memory, backend_state_operation_enum=backend)
    summaries = {variant: summarize(root, variant) for variant in
                 ("default", "after", "upstream_equal", "upstream_native")}
    for key, old in summaries["default"].items():
        new = summaries["after"][key]
        assert old["reads"] == new["reads"] and old["writes"] == new["writes"], key
        if key[0] == "write" or key[2] != 1 or key[4] != 1:
            assert old["virtual_us"] == new["virtual_us"], key
            assert new["read_sessions"] == 0, key
    hashes = {variant: {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in sorted((root / variant / "lib").iterdir()) if path.is_file()}
              for variant in ("before", "after", "upstream")}
    assert hashes["after"] == {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in Path("lib").iterdir() if path.is_file()}
    for name in ("sdcard_sim.c", "sdcard_sim.h"):
        assert (root / "after/tests" / name).read_bytes() == (Path("tests") / name).read_bytes()
    rows = [dict(variant=variant, phase=key[0], profile=key[1], cluster_sectors=key[2], chunk_bytes=key[3], files=key[4], **values)
            for variant, summary in summaries.items() for key, values in summary.items()]
    payload = dict(evidence_directory=str(root), manifest=json.loads((root / "manifest.json").read_text()),
                   source_hashes=hashes, resources=resources, pic24_frames=stacks, repetitions=7, results=rows)
    arguments.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    for row in rows:
        if row["phase"] == "read" and row["profile"] == 1 and row["cluster_sectors"] == 1:
            print(f"{row['variant']}: chunk={row['chunk_bytes']}, readers={row['files']}, R={row['reads']}, "
                  f"commands={row['read_commands']}, sessions={row['read_sessions']}, MiB/s={row['model_mib_s']:.6f}")
    print("Validated: same RAM/IO, unchanged existing PIC24 frames, bounded new leaf scans, matching live sources.")


if __name__ == "__main__":
    main()
