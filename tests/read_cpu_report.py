"""Validate read CPU trials, unchanged I/O/RAM and the internal PIC24 stack cap."""
import csv
import argparse
import hashlib
import json
import statistics
import struct
import re
import subprocess
from pathlib import Path

from multiread_report import summarize
from pending_allocation_report import memory_result, pic_frames
from pic24_stack_budget import analyze
from upstream_comparison_report import coff_sections


def function_code(path, wanted):
    data, sections = coff_sections(path)
    symbol_offset, symbol_count = struct.unpack_from('<II', data, 8)
    strings = symbol_offset + symbol_count * 18
    functions = []
    index = 0
    while index < symbol_count:
        entry = data[symbol_offset + index * 18:symbol_offset + (index + 1) * 18]
        if entry[:4] == bytes(4):
            start = strings + struct.unpack_from('<I', entry, 4)[0]
            name = data[start:data.index(0, start)].decode()
        else:
            name = entry[:8].split(bytes(1))[0].decode()
        value, section, kind = struct.unpack_from('<IhH', entry, 8)
        if kind == 0x20 and section > 0:
            functions.append((name.lstrip('_'), section, value))
        index += 1 + entry[17]
    selected = [item for item in functions if item[0] == wanted]
    assert len(selected) == 1, (path, wanted)
    _, section, start = selected[0]
    record = sections[section - 1]
    end = min([value for _, other, value in functions if other == section and value > start] + [record['size']])
    code = data[record['offset'] + start:record['offset'] + end]
    return dict(bytes=len(code), sha256=hashlib.sha256(code).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/fat32-read-cpu-20261116'))
    parser.add_argument('--output', type=Path, default=Path('tests/FAT32_READ_CPU.json'))
    parser.add_argument('--repetitions', type=int, default=7)
    parser.add_argument('--verify-multiread-code', action='store_true')
    parser.add_argument('--verify-physical-inline', action='store_true')
    arguments = parser.parse_args()
    root = arguments.directory
    variants = ['before_default', 'default', 'before', 'after', 'after_no_query',
                'upstream_equal', 'upstream_native']
    if (root / 'cpu-before_query/results.csv').exists():
        variants.append('before_query')
    if (root / 'cpu-reference_query/results.csv').exists():
        variants.append('reference_query')
    if (root / 'cpu-reference_default/results.csv').exists():
        variants.append('reference_default')
    cpu = {}
    samples = {}
    deterministic = {}
    for variant in variants:
        directory = root / f'cpu-{variant}'
        cycles = {}
        for line in (directory / 'audit.log').read_text(encoding='utf-8-sig').splitlines():
            if line.startswith('CPU_CYCLES,'):
                fields = line.split(',')
                if fields[1] == 'read':
                    assert int(fields[2]) == 0 and int(fields[3]) == 1
                    key = tuple(map(int, fields[4:7]))
                    cycles[key] = int(fields[7]) / int(fields[8])
        groups = {}
        with (directory / 'results.csv').open(encoding='utf-8-sig', newline='') as source:
            for row in csv.DictReader(source):
                if row['phase'] == 'read':
                    key = (int(row['chunk_bytes']), int(row['files']))
                    groups.setdefault(key, []).append(row)
        cpu[variant] = {}
        deterministic[variant] = {}
        samples[variant] = {}
        assert len(groups) == 4
        for key, rows in groups.items():
            assert len(rows) == arguments.repetitions
            assert {int(row['repetition']) for row in rows} == set(range(arguments.repetitions))
            values = [cycles[(*key, trial)] for trial in range(arguments.repetitions)]
            samples[variant][key] = values
            fields = ('payload_bytes', 'reads', 'writes', 'metadata_reads',
                      'metadata_writes', 'virtual_us', 'polls')
            measurements = {}
            for field in fields:
                measured = {int(row[field]) for row in rows}
                assert len(measured) == 1, (variant, key, field)
                measurements[field] = measured.pop()
            assert measurements['payload_bytes'] == key[1] * 1048576 * 32
            deterministic[variant][key] = measurements
            cpu[variant][str(key)] = dict(cycles_per_byte=statistics.median(values),
                minimum=min(values), maximum=max(values), samples=values, **measurements)
    comparisons = {}
    pairs = [('before_default', 'default'), ('before', 'after'), ('before', 'after_no_query')]
    if 'before_query' in variants:
        pairs.append(('before_query', 'after'))
    if 'reference_query' in variants:
        pairs.append(('reference_query', 'after'))
    if 'reference_default' in variants:
        pairs.append(('reference_default', 'default'))
    for older, newer in pairs:
        assert deterministic[older] == deterministic[newer], (older, newer)
        comparisons[f'{older}/{newer}'] = {str(key): statistics.median(
            100 * (1 - new / old) for old, new in zip(samples[older][key], samples[newer][key]))
            for key in samples[older]}
    normal = {variant: summarize(root, variant) for variant in
              ('default', 'after', 'upstream_equal', 'upstream_native')}
    normal['before'] = summarize(root, 'before', root / 'normal-before')
    normal_pairs = [('before', 'after')]
    if (root / 'normal-before-default/results.csv').exists():
        normal['before_default'] = summarize(root, 'before_default', root / 'normal-before-default')
        normal_pairs.append(('before_default', 'default'))
    for older, newer in normal_pairs:
        for key in normal[older]:
            for field in ('reads', 'writes', 'metadata_reads', 'metadata_writes', 'virtual_us', 'polls',
                          'read_sessions', 'batched_read_blocks', 'read_stops'):
                assert normal[older][key][field] == normal[newer][key][field], (older, newer, key, field)
    resources = {}
    for abi in ('32', '64', '16-normal', '16-short'):
        memory = {variant: memory_result(root, variant, abi) for variant in ('before', 'default', 'after')}
        assert memory['before'] == memory['default'] == memory['after'], (abi, memory)
        resources[abi] = memory
    stacks = {}
    frames = {}
    for mode in ('normal', 'short'):
        assembly = root / f'fat_standard-{mode}.s'
        command = ['C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe', '-mcpu=24FJ256GA702',
                   '-std=c99', '-Os', f'-I{root / "after/lib"}', '-S',
                   str(root / 'after/lib/fat_standard.c'), '-o', str(assembly)]
        if mode == 'short':
            command.append('-fshort-enums')
        subprocess.run(command, check=True, capture_output=True)
        for variant in ('before', 'default', 'after'):
            listing = root / f'{variant}-16-{mode}/layout.s'
            stacks[f'{variant}/{mode}'] = analyze([listing, assembly], 400)
            measured = pic_frames(listing)
            frames[f'{variant}/{mode}'] = {name: measured[name] for name in
                ('_afatfs_fread', '_afatfs_fprefetch', '_afatfs_fileOperationsPoll', '_afatfs_cacheSector')}
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (root / 'after/lib').iterdir() if path.is_file()}
    assert hashes == {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in Path('lib').iterdir() if path.is_file()}
    report = dict(evidence_directory=str(root), repetitions=arguments.repetitions, passes=32,
        source_hashes=hashes, cpu=cpu, paired_cpu_reduction_percent=comparisons,
        upstream_manifest=json.loads((root / 'manifest.json').read_text()),
        snapshot_hashes={version: {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (root / version / 'lib').iterdir() if path.is_file()}
            for version in ('before', 'after', 'upstream', 'reference')
            if (root / version / 'lib').exists()},
        resources=resources, stack_cascades=stacks, frames=frames,
        normal_results=[dict(variant=variant, key=list(key), **value)
                        for variant, results in normal.items() for key, value in results.items()])
    if 'reference_default' in variants or arguments.verify_multiread_code:
        preserved = {}
        for older, newer in (('before', 'after_no_query'), ('before_query', 'after')):
            old = function_code(root / f'cpu-{older}/asyncfatfs.obj', 'afatfs_fread')
            new = function_code(root / f'cpu-{newer}/asyncfatfs.obj', 'afatfs_fread')
            assert old == new, (older, newer, old, new)
            preserved[f'{older}/{newer}'] = new
        report['unchanged_multiread_fread_code'] = preserved
    if arguments.verify_physical_inline:
        instruction_evidence = {}
        for mode in ('normal', 'short'):
            mode_evidence = {}
            for variant in ('before', 'default', 'after'):
                listing = (root / f'{variant}-16-{mode}/layout.s').read_text()
                body = re.search(r'_afatfs_fread:\n(.*?)\n\s*\.size\s+_afatfs_fread', listing, re.S).group(1)
                emitted = '_afatfs_fileGetCursorPhysicalSector:' in listing
                calls = len(re.findall(r'(?:r?call)\s+_afatfs_fileGetCursorPhysicalSector', body))
                assert emitted == (variant == 'before')
                assert (calls > 0) == (variant == 'before')
                mode_evidence[variant] = dict(helper_emitted=emitted, fread_call_sites=calls)
            instruction_evidence[mode] = mode_evidence
        report['physical_inline_pic24'] = instruction_evidence
        report['physical_inline_msvc_fread'] = {}
        report['physical_inline_msvc_fprefetch'] = {}
        for older, newer in (('before_default', 'default'), ('before', 'after_no_query'), ('before_query', 'after')):
            old = function_code(root / f'cpu-{older}/asyncfatfs.obj', 'afatfs_fread')
            new = function_code(root / f'cpu-{newer}/asyncfatfs.obj', 'afatfs_fread')
            assert old == new, (older, newer, 'afatfs_fread', old, new)
            report['physical_inline_msvc_fread'][f'{older}/{newer}'] = dict(before=old, after=new, identical=old == new)
            old = function_code(root / f'cpu-{older}/asyncfatfs.obj', 'afatfs_fprefetch')
            new = function_code(root / f'cpu-{newer}/asyncfatfs.obj', 'afatfs_fprefetch')
            assert old == new, (older, newer, 'afatfs_fprefetch', old, new)
            report['physical_inline_msvc_fprefetch'][f'{older}/{newer}'] = dict(before=old, after=new, identical=old == new)
    arguments.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(cpu={variant: {key: round(value['cycles_per_byte'], 4)
        for key, value in results.items()} for variant, results in cpu.items()},
        reductions=comparisons, ram={abi: value['after']['static_ram'] for abi, value in resources.items()},
        stack={key: value['conservative_internal_bytes'] for key, value in stacks.items()}, frames=frames), indent=2))


if __name__ == '__main__':
    main()
