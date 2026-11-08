"""Validate and summarize the isolated prefetch-window experiment."""

import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path

from pending_allocation_report import pic_frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory
    manifest = json.loads((root / "manifest.json").read_text())
    current_hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in Path("lib").iterdir() if path.is_file()}
    assert current_hashes == manifest["source_hashes"]["current"], "Live library changed during experiment"
    results = []
    for variant in ("upstream_equal", "current", "window2", "window3"):
        groups = {}
        with (root / f"{variant}-64/results.csv").open(encoding="utf-8-sig", newline="") as source:
            for row in csv.DictReader(source):
                key = (row["phase"], *map(int, (row["profile"], row["cluster_sectors"], row["chunk_bytes"], row["files"])))
                groups.setdefault(key, []).append(row)
        assert len(groups) == 32
        for key, rows in groups.items():
            assert len(rows) == 7
            assert {int(row["repetition"]) for row in rows} == set(range(7))
            values = {}
            for field in ("payload_bytes", "reads", "writes", "metadata_reads", "metadata_writes", "virtual_us", "polls"):
                samples = {int(row[field]) for row in rows}
                assert len(samples) == 1, (variant, key, field, samples)
                values[field] = samples.pop()
            values["model_mib_s"] = values["payload_bytes"] / 1048576 / (values["virtual_us"] / 1000000)
            values["host_mib_s"] = statistics.median(int(row["payload_bytes"]) / 1048576 /
                                                    float(row["host_seconds"]) for row in rows)
            results.append(dict(variant=variant, phase=key[0], profile=key[1], cluster_sectors=key[2],
                                chunk_bytes=key[3], files=key[4], **values))
    frames = {variant: pic_frames(root / f"{variant}-pic24.s")["_afatfs_fprefetch"]
              for variant in ("current", "window")}
    payload = dict(manifest=manifest, evidence_directory=str(root), repetitions=7,
                   pic24_prefetch_frame=frames, results=results)
    arguments.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    for row in results:
        if row["phase"] == "read" and row["profile"] == 1 and row["cluster_sectors"] == 1:
            print(f"{row['variant']}: chunk={row['chunk_bytes']}, files={row['files']}, "
                  f"R={row['reads']}, metadata={row['metadata_reads']}, MiB/s={row['model_mib_s']:.6f}")
    print(f"PIC24 prefetch frames: {frames}. Live library matches baseline snapshot.")


if __name__ == "__main__":
    main()
