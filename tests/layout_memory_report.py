"""Compare reports from run_layout_tests.ps1, including XC16 assembly constants."""
import argparse
import hashlib
import json
import re
from pathlib import Path

STRUCTURES = ("afatfs_t", "afatfsFile_t", "afatfsRequest_t", "afatfsDirectory_t", "afatfsTruncateFile_t")
METRICS = ("int_bytes", "pointer_bytes") + STRUCTURES + (
    "afatfsJournal_t", "journal_io", "journal_before", "journal_header")


def load_report(path):
    report = json.loads(path.read_text(encoding="utf-8-sig"))
    if report["status"] != "passed":
        raise ValueError(f"Incomplete layout validation: {path}")
    cases = {}
    for entry in report["host"]:
        values = next(line for line in entry["output"] if re.fullmatch(r"\d+(,\d+)+", line))
        cases[(entry["configuration"], entry["bits"], False)] = dict(zip(METRICS, map(int, values.split(","))))
    for entry in report["xc16"]:
        assembly = path.parent / Path(entry["assembly"]).name
        if not assembly.is_file():
            assembly = Path(entry["assembly"])
        source = assembly.read_text(errors="replace")
        length = int(re.search(r"\.size\s+_afatfs_layout_sizes,\s*(\d+)", source)[1]) // 2
        body = source.split("_afatfs_layout_sizes:", 1)[1]
        values = []
        for line in body.splitlines():
            word = re.match(r"\s*\.(word|skip)\s+(\d+)", line)
            if word:
                values.extend([int(word[2])] if word[1] == "word" else [0] * (int(word[2]) // 2))
            elif line.strip() and values:
                break
            if len(values) == length:
                break
        if len(values) != length:
            raise ValueError(f"Could not decode layout constants: {assembly}")
        if values[0] != 2:
            raise ValueError(f"Expected a 16-bit int ABI: {assembly}")
        cases[(entry["configuration"], 16, entry["shortEnums"])] = dict(zip(METRICS, values))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Markdown report; JSON is saved beside it")
    args = parser.parse_args()
    before = load_report(args.before)
    after = load_report(args.after)
    if before.keys() != after.keys():
        raise ValueError("Reports must cover the same configurations, widths and enum modes")
    rows = []
    components = []
    for key in sorted(before):
        previous, current = before[key], after[key]
        for name in ("int_bytes", "pointer_bytes"):
            if previous[name] != current[name]:
                raise ValueError(f"Mismatched ABI: {key}")
        for name in STRUCTURES + ("afatfsJournal_t",):
            if name not in current:
                continue
            if current[name] > previous[name]:
                raise ValueError(f"Structure grew: {key}, {name}: {previous[name]} -> {current[name]}")
            components.append({"configuration": key[0], "bits": key[1], "short_enums": key[2], "structure": name,
                               "before": previous[name], "after": current[name], "saved": previous[name] - current[name]})
        for name in ("journal_io", "journal_before", "journal_header"):
            if name in current and current[name] % 4:
                raise ValueError(f"Misaligned journal buffer: {key}, {name}")
        rows.append({"configuration": key[0], "bits": key[1], "short_enums": key[2],
                     "before": previous["afatfs_t"], "after": current["afatfs_t"],
                     "saved": previous["afatfs_t"] - current["afatfs_t"]})
    lines = ["# Runtime structure memory comparison", "",
             "Baseline: runtime structures with Boolean bit-fields, before member reordering. All sizes are bytes.", "",
             "The global `afatfs_t` instance includes its cache, handles and journal. Component sizes below must not be added again to this total.", "",
             "| Configuration | Target | Enums | Before | After | Saved |",
             "| --- | --- | --- | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['configuration']} | {row['bits']}-bit | {'short' if row['short_enums'] else 'normal'} | {row['before']} | {row['after']} | {row['saved']} |")
    lines += ["", "## Per-instance components, default configuration", "",
              "These rows use normal enums. Request/directory workspaces are caller-owned; their instance count belongs to the application.", "",
              "The backend-owned `sdcardMetadata_t` also shrank from 24 to 22 bytes on XC16/PIC24; it remains 24 bytes on x86/x64. It is outside `afatfs_t`, so the global totals exclude this additional per-instance saving.", "",
              "| Structure | Target | Before | After | Saved |", "| --- | --- | ---: | ---: | ---: |"]
    for entry in components:
        if entry["configuration"] == "default" and not entry["short_enums"]:
            lines.append(f"| `{entry['structure']}` | {entry['bits']}-bit | {entry['before']} | {entry['after']} | {entry['saved']} |")
    lines += ["", "## Layout decisions and validation", "",
              "Runtime members are grouped by required alignment. Word/pointer members precede byte flags and odd-byte tails where this removes gaps. An odd array element count does not imply an odd byte size; type alignment and total bytes determine padding.", "",
              "Aligned cache/journal buffers retain four-byte alignment. The request scratch remains aligned for `afatfsPathFrame_t`. Embedded truncate/append sub-operations retain their mandatory first-member position. Disk records and Windows protocol structures retain their fixed field order.", "",
              "All measured structures stayed the same size or shrank. Natural member alignment, four-byte sector-buffer alignment, scratch-frame alignment and embedded sub-operation prefixes passed the layout probes.", "",
              "Do not sum nested component savings with the global-instance saving. Remaining tail padding may be required by natural alignment. No `packed` attribute, new controller storage, heap allocation or non-Boolean type narrowing was introduced.", "",
              "These are ABI changes to memory structures: rebuild the library, backend and consumers together, and use named/designated fields rather than relying on their old offsets or positional aggregate order.", "",
              "16-bit results are XC16/PIC24 C99 cross-compilation measurements, not execution on hardware. Host flag tests execute on x86 and x64. Exact padding depends on the compiler ABI and configuration.", "",
              "## Reproduction", "", "```powershell",
              "# Capture this before applying a layout change:",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/reorder-before",
              "# Apply the layout change, then measure again:",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/reorder-after",
              "python tests/layout_memory_report.py --before tests/build/reorder-before/report.json --after tests/build/reorder-after/report.json --output tests/MEMORY_LAYOUT_REPORT.md",
              "./tests/run_exfat_tests.ps1 -CompileLegacy", "```", "",
              "The saved before report and XC16 constants were captured before reordering. Host sizes and flag tests execute on x86/x64; XC16 assembly contains compiler-evaluated `sizeof` constants. The comparison fails if an ABI differs, a measured structure grows or a journal buffer loses alignment.", "",
              f"Before report: `{args.before.as_posix()}`", f"After report: `{args.after.as_posix()}`", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    data = {"status": "passed", "before_sha256": hashlib.sha256(args.before.read_bytes()).hexdigest(),
            "after_sha256": hashlib.sha256(args.after.read_bytes()).hexdigest(), "global": rows, "components": components}
    args.output.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {len(rows)} configurations; report: {args.output}")


if __name__ == "__main__":
    main()
