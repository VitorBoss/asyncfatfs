"""Validate the consumed-reader-page cache policy and its 400-byte PIC24 budget."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from multiread_report import summarize
from pending_allocation_report import memory_result, pic_frames
from pic24_stack_budget import analyze


def reuse_records(directory):
    result = {}
    for line in (directory / "audit.log").read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("READ_REUSE,"):
            fields = line.split(",")
            key = (fields[1], *map(int, fields[2:6]))
            result.setdefault(key, set()).add(tuple(map(int, fields[7:9])))
    assert len(result) == 32
    assert all(len(values) == 1 for values in result.values())
    return {key: next(iter(values)) for key, values in result.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    root = arguments.directory
    baseline = summarize(root, "default", root / "reuse-before")
    audited = summarize(root, "after", root / "reuse-after")
    variants = {variant: summarize(root, variant) for variant in
                ("default", "after", "upstream_equal", "upstream_native")}
    variants["before"] = baseline
    variants["after_audit"] = audited
    reuse = {version: reuse_records(root / f"reuse-{version}") for version in ("before", "after")}
    for key in baseline:
        old = baseline[key]
        new = variants["after"][key]
        assert new["reads"] <= old["reads"] and new["writes"] == old["writes"], key
        assert new["virtual_us"] <= old["virtual_us"], key
        assert new["reads"] == audited[key]["reads"] and new["virtual_us"] == audited[key]["virtual_us"]
        if key == ("read", 1, 1, 4096, 2):
            assert reuse["before"][key] == (4096, 510)
            assert reuse["after"][key] == (4096, 0)
            assert old["reads"] == 4640 and new["reads"] == 4130
    resources = {}
    for abi in ("32", "64", "16-normal", "16-short"):
        memory = {variant: memory_result(root, variant, abi) for variant in ("before", "default", "after")}
        assert memory["before"] == memory["default"] == memory["after"], (abi, memory)
        resources[abi] = memory
    stacks = {}
    frames = {}
    for mode in ("normal", "short"):
        assembly = root / f"fat_standard-{mode}.s"
        assert (root / "before/lib/fat_standard.c").read_bytes() == (root / "after/lib/fat_standard.c").read_bytes()
        command = ["C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe", "-mcpu=24FJ256GA702",
                   "-std=c99", "-Os", f"-I{root / 'after/lib'}", "-S", str(root / "after/lib/fat_standard.c"),
                   "-o", str(assembly)]
        if mode == "short":
            command.append("-fshort-enums")
        subprocess.run(command, check=True, capture_output=True)
        before = pic_frames(root / f"before-16-{mode}/layout.s")
        for variant in ("default", "after"):
            current = pic_frames(root / f"{variant}-16-{mode}/layout.s")
            increases = {name: [size, current[name]] for name, size in before.items()
                         if name in current and current[name] > size}
            assert not increases, (variant, mode, increases)
            frames[f"{variant}/{mode}"] = dict(common_frame_increases=increases,
                fread_frame=[before["_afatfs_fread"], current["_afatfs_fread"]],
                unlock_frame=[before["_afatfs_fileUnlockCacheSector"], current["_afatfs_fileUnlockCacheSector"]])
        for variant in ("before", "default", "after"):
            stacks[f"{variant}/{mode}"] = analyze(
                [root / f"{variant}-16-{mode}/layout.s", root / f"fat_standard-{mode}.s"], 400)
    hashes = {version: {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in sorted((root / version / "lib").iterdir()) if path.is_file()}
              for version in ("original", "before", "after", "upstream")}
    assert hashes["after"] == {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in Path("lib").iterdir() if path.is_file()}
    rows = []
    for variant, results in variants.items():
        for key, value in results.items():
            row = dict(variant=variant, phase=key[0], profile=key[1], cluster_sectors=key[2],
                       chunk_bytes=key[3], readers=key[4], **value)
            reuse_variant = "after" if variant == "after_audit" else variant
            if reuse_variant in reuse:
                row["unique_data_reads"], row["repeated_data_reads"] = reuse[reuse_variant][key]
            rows.append(row)
    report = dict(repetitions=7, evidence_directory=str(root),
                  baseline_note="Original DMA commit with cache-pointer restoration fix applied equally before/after",
                  source_hashes=hashes, resources=resources, frames=frames, stack_cascades=stacks,
                  upstream_manifest=json.loads((root / "manifest.json").read_text()), results=rows)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for row in rows:
        if row["phase"] == "read" and row["profile"] == 1 and row["cluster_sectors"] == 1:
            print(f"{row['variant']}: chunk={row['chunk_bytes']}, readers={row['readers']}, "
                  f"R={row['reads']}, MiB/s={row['model_mib_s']:.6f}")
    print("Validated: 510 -> 0 repeated data reads; unchanged RAM; all internal PIC24 cascades <=400 bytes.")


if __name__ == "__main__":
    main()
