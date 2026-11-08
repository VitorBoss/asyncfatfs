"""Conservative direct-call stack bound for the library's PIC24 assembly.

Includes return PCs and all positive stack adjustments (even mutually exclusive
ones). External routines and indirect application callbacks are explicit cutoffs.
No C recursion, alloca, VLA, or unmatched stack growth is accepted silently.
"""
import argparse
import json
import re
from pathlib import Path


def normalize(name):
    return re.sub(r"\.(?:clone|constprop|isra)\.\d+$", "", name)


def parse_functions(path):
    source = path.read_text(encoding="utf-8-sig")
    functions = {}
    for match in re.finditer(r"^\s*\.type\s+(_[\w.]+),@function\n\1:\n(.*?)^\s*\.size\s+\1,", source, re.M | re.S):
        raw_name, body = match.groups()
        name = normalize(raw_name)
        positive = 0
        calls = set()
        indirect = False
        for line in body.splitlines():
            line = line.strip()
            if allocation := re.fullmatch(r"lnk\s+#(\d+)", line):
                positive += int(allocation[1]) + 2
            elif allocation := re.fullmatch(r"add\s+w15,#(\d+),w15", line):
                positive += int(allocation[1])
            elif allocation := re.fullmatch(r"add\s+#(\d+),w15", line):
                positive += int(allocation[1])
            elif re.fullmatch(r"mov(?:\.[bwd])?\s+.+,\[w15\+\+\]", line):
                positive += 4 if line.startswith("mov.d") else 2
            elif re.fullmatch(r"push(?:\.d)?\s+\S+", line):
                positive += 4 if line.startswith("push.d") else 2
            elif re.match(r"(?:add|sub)\s+.*[, ]w15$", line):
                if not re.fullmatch(r"sub\s+(?:w15,#\d+,w15|#\d+,w15)", line):
                    raise ValueError(f"Unbounded/unrecognized SP adjustment in {name}: {line}")
            if call := re.match(r"(?:rcall|call|bra)\s+(_[\w.]+)$", line):
                if re.search(r"(?:^|_)alloca(?:$|_)", call[1]):
                    raise ValueError(f"Dynamic allocation in {name}: {line}")
                calls.add(normalize(call[1]))
            if re.match(r"(?:rcall|call)\s+w\d+$", line):
                indirect = True
        # XC16 sometimes uses [w15] as a two-byte arithmetic flag scratch.
        own_bound = positive + (2 if "[w15]" in body else 0)
        previous = functions.get(name, dict(own_bound=0, calls=set(), indirect=False))
        functions[name] = dict(own_bound=max(previous["own_bound"], own_bound),
                               calls=previous["calls"] | calls, indirect=previous["indirect"] or indirect)
    assert functions, path
    return functions


def analyze(paths, budget):
    functions = {}
    for path in paths:
        additions = parse_functions(path)
        assert not (functions.keys() & additions.keys()), "Duplicate compilation units"
        functions.update(additions)
    completed = {}

    def visit(name, active):
        if name in active:
            raise ValueError(f"Recursive direct-call cycle requires a separate bound: {active + [name]}")
        if name in completed:
            return completed[name]
        own = functions[name]["own_bound"]
        peak = own
        chain = [name]
        for called in functions[name]["calls"]:
            if called not in functions:
                continue
            nested, nested_chain = visit(called, active + [name])
            candidate = own + 4 + nested
            if candidate > peak:
                peak = candidate
                chain = [name] + nested_chain
        if functions[name]["calls"] or functions[name]["indirect"]:
            peak = max(peak, own + 4) # Return PC at the external/indirect boundary.
        completed[name] = peak, chain
        return completed[name]

    for name in functions:
        visit(name, [])
    name = max(completed, key=lambda entry: completed[entry][0])
    peak, chain = completed[name]
    peak += 4 # Return PC of the library entry from its caller.
    if peak > budget:
        raise ValueError(f"Internal bound {peak} exceeds {budget}: {chain}")
    return dict(budget_bytes=budget, conservative_internal_bytes=peak,
                remaining_bytes=budget - peak, deepest_chain=chain,
                chain_frames=[dict(function=item, conservative_own_bytes=functions[item]["own_bound"])
                              for item in chain],
                external_cutoffs=sorted({called for item in functions.values() for called in item["calls"]
                                         if called not in functions}),
                indirect_callback_cutoffs=sorted(name for name, item in functions.items() if item["indirect"]),
                function_count=len(functions))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assembly", type=Path, action="append", required=True)
    parser.add_argument("--budget", type=int, default=400)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    report = analyze(arguments.assembly, arguments.budget)
    report["assembly"] = [str(path) for path in arguments.assembly]
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
