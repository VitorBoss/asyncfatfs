"""Validate before/after pending-writer benchmarks and RAM probes."""

import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path

from upstream_comparison_report import coff_layout, coff_sections


def memory_result(root, variant, abi):
    directory = root / f"{variant}-{abi}"
    static_ram = 0
    if abi in ("32", "64"):
        layout = coff_layout(directory / "layout.obj")
        for source in ("asyncfatfs", "fat_standard"):
            _, sections = coff_sections(directory / f"{source}.obj")
            static_ram += sum(section["size"] for section in sections
                              if section["flags"] & 0x80000000 and section["flags"] & 0xC0)
    else:
        assembly = (directory / "layout.s").read_text(encoding="utf-8-sig")
        body = assembly.split("_comparison_layout:", 1)[1]
        layout = tuple(map(int, re.findall(r"^\s*\.word\s+(\d+)", body, re.M)[:3]))
        assert len(layout) == 3
        for source in ("asyncfatfs", "fat_standard"):
            listing = (directory / f"{source}-inspection.txt").read_text(encoding="utf-8-sig")
            pattern = (r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+"
                       r"[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+2\*\*\d+\n\s*([^\n]*)")
            for match in re.finditer(pattern, listing, re.M):
                _, size, flags = match.groups()
                if "ALLOC" in flags and not any(tag in flags for tag in ("CODE", "READONLY", "PSV")):
                    static_ram += int(size, 16)
    return dict(static_ram=static_ram, global_state=layout[0], handle=layout[1], descriptor=layout[2])


def pic_frames(path):
    assembly = path.read_text(encoding="utf-8-sig")
    frames = {}
    for match in re.finditer(r"^\s*\.type\s+(_[\w.]+),@function\n\1:\n(.*?)^\s*\.size\s+\1,",
                             assembly, re.M | re.S):
        name, body = match.groups()
        frame = 0
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith("."):
                continue
            if allocation := re.fullmatch(r"lnk\s+#(\d+)", line):
                frame += int(allocation[1]) + 2
            elif allocation := re.fullmatch(r"add\s+w15,#(\d+),w15", line):
                frame += int(allocation[1])
            elif re.fullmatch(r"mov\.d\s+w\d+,\[w15\+\+\]", line):
                frame += 4
            elif re.fullmatch(r"mov\s+w\d+,\[w15\+\+\]", line):
                frame += 2
            else:
                break
        name = re.sub(r"\.(?:clone|constprop|isra)\.\d+$", "", name)
        frames[name] = max(frames.get(name, 0), frame)
    assert frames
    return frames


def summarize(root, variant, schedule):
    directory = root / f"{variant}-64"
    groups = {}
    with (directory / f"results-{schedule}.csv").open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            key = (row["phase"], int(row["profile"]), int(row["cluster_sectors"]),
                   int(row["chunk_bytes"]), int(row["files"]))
            groups.setdefault(key, []).append(row)
    trace = {}
    audit = (directory / f"audit-{schedule}.log").read_text(encoding="utf-8-sig")
    for line in audit.splitlines():
        if line.startswith("MIRROR,"):
            assert line.split(",")[-1] == "0", line
        if line.startswith("META,"):
            fields = line.split(",")
            key = (fields[1], *map(int, fields[2:6]))
            trace.setdefault(key, []).append(tuple(map(int, fields[7:10])))
    results = {}
    for key, rows in groups.items():
        assert len(rows) == 7, (variant, schedule, key, len(rows))
        assert {int(row["repetition"]) for row in rows} == set(range(7))
        result = {}
        for field in ("reads", "writes", "metadata_reads", "metadata_writes", "virtual_us", "payload_bytes"):
            values = {int(row[field]) for row in rows}
            assert len(values) == 1, (variant, schedule, key, field, values)
            result[field] = values.pop()
        result["model_mib_s"] = result["payload_bytes"] / 1048576 / (result["virtual_us"] / 1000000)
        result["host_mib_s"] = statistics.median(int(row["payload_bytes"]) / 1048576 /
                                                float(row["host_seconds"]) for row in rows)
        assert len(trace[key]) == 7 and len(set(trace[key])) == 1
        result["primary_fat_writes"], result["mirror_fat_writes"], result["directory_writes"] = trace[key][0]
        results[key] = result
    assert len(results) == 32
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory
    memory = {}
    for abi in ("16-normal", "16-short", "32", "64"):
        before = memory_result(root, "before", abi)
        after = memory_result(root, "after", abi)
        assert before == after, (abi, before, after)
        memory[abi] = dict(before=before, after=after)
    stack = {}
    for abi in ("16-normal", "16-short"):
        before = pic_frames(root / f"before-{abi}/layout.s")
        after = pic_frames(root / f"after-{abi}/layout.s")
        increases = {name: [size, after[name]] for name, size in before.items()
                     if name in after and after[name] > size}
        assert not increases, (abi, increases)
        stack[abi] = {
            "common_function_frame_increases": increases,
            "new_helpers": {name: size for name, size in after.items() if name not in before},
            "selected": {name: [before[name], after[name]] for name in
                         ("_afatfs_fileOperationsPoll", "_afatfs_appendRegularFreeClusterContinue", "_afatfs_poll")},
        }
    comparisons = []
    for schedule in ("serial", "interleaved"):
        before = summarize(root, "before", schedule)
        after = summarize(root, "after", schedule)
        assert before.keys() == after.keys()
        for key in before:
            comparisons.append(dict(schedule=schedule, phase=key[0], profile=key[1], cluster_sectors=key[2],
                                    chunk_bytes=key[3], files=key[4], before=before[key], after=after[key]))
    hashes = {}
    for variant in ("before", "after"):
        hashes[variant] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted((root / variant / "lib").iterdir()) if path.is_file()}
    payload = dict(evidence_directory=str(root), repetitions=7, memory=memory, pic24_frames=stack,
                   source_hashes=hashes, comparisons=comparisons)
    arguments.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    for comparison in comparisons:
        if comparison["profile"] == 1 and comparison["cluster_sectors"] == 1 and comparison["chunk_bytes"] == 257:
            print(json.dumps(comparison))
    print(json.dumps(dict(memory=memory, pic24_frames=stack)))


if __name__ == "__main__":
    main()
