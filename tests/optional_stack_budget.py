"""Compile the PIC24 feature matrix and check internal stack (<400 bytes) and static RAM.

The bound includes library calls and return PCs, but excludes backend, application
callbacks and compiler/libc routines as specified by pic24_stack_budget.py.
Run from the repository root; --library permits an isolated source snapshot.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from pic24_stack_budget import analyze

CONFIGURATIONS = ('default', 'fat_legacy', 'fat_only', 'fat_lfn', 'exfat_only',
                  'exfat_freefile', 'lfn_exfat', 'lfn_no_journal')
PROFILES = {'single': [], 'multi': ['-DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2'],
            'query': ['-DAFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2', '-DAFATFS_READ_SESSION_QUERY=1']}


def static_storage(path):
    """Count XC16 writable section bytes, including alignment between objects."""
    sections = {}
    section = None
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if match := re.match(r'\s*\.section\s+([^, ]+)(.*)', line):
            name, flags = match.groups()
            section = name if 'info' not in flags and re.search(r',(?:bss|data)(?:,|$)', flags) else None
            if section:
                sections.setdefault(section, 0)
        elif section:
            if match := re.match(r'\s*\.align\s+(\d+)', line):
                alignment = int(match[1])
                sections[section] = (sections[section] + alignment - 1) // alignment * alignment
            elif match := re.match(r'\s*\.(?:skip|space)\s+(\d+)', line):
                sections[section] += int(match[1])
            elif match := re.match(r'\s*\.(byte|word|long)\s+(.+)', line):
                sections[section] += {'byte': 1, 'word': 2, 'long': 4}[match[1]] * len(match[2].split(','))
    # XC16 gives anonymous aligned buffers a generated section name containing
    # compiler-internal identifiers; compare their sizes, not those identifiers.
    anonymous = 0
    normalized = {}
    for name, size in sections.items():
        if name.startswith('*'):
            name = f'anonymous-{anonymous}'
            anonymous += 1
        normalized[name] = size
    return normalized


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, default=Path('lib'))
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='C:/Program Files/Microchip/xc16/v2.10/bin/xc16-gcc.exe')
    parser.add_argument('--configuration', action='append', choices=CONFIGURATIONS)
    parser.add_argument('--profile', action='append', choices=PROFILES)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    versions = {'after': arguments.library.resolve()}
    if arguments.baseline:
        versions['before'] = arguments.baseline.resolve()
    report = {'budget_exclusive': 400, 'results': [], 'sources': {}}
    for version, library in versions.items():
        report['sources'][version] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                      for path in library.iterdir() if path.suffix in ('.c', '.h')}
    failed = False
    for configuration in arguments.configuration or CONFIGURATIONS:
        for profile in arguments.profile or PROFILES:
            for mode in ('normal', 'short'):
                measurements = {}
                for version, library in versions.items():
                    flags = ['-mcpu=24FJ256GA702', '-std=c99', '-Os', f'-I{library}', *PROFILES[profile]]
                    if configuration != 'default':
                        name = 'fat_lfn' if configuration == 'lfn_no_journal' else configuration
                        flags += ['-DAFATFS_EXTERNAL_CONFIG', f'-I{Path("tests/config", name).resolve()}']
                    if configuration == 'lfn_no_journal':
                        flags.append('-DAFATFS_DISABLE_FAT_JOURNAL')
                    if mode == 'short':
                        flags.append('-fshort-enums')
                    assemblies = []
                    storage = {}
                    for filename in ('asyncfatfs', 'fat_standard'):
                        output = arguments.output / f'{version}-{configuration}-{profile}-{mode}-{filename}.s'
                        command = [arguments.compiler, *flags, '-S', str(library / f'{filename}.c'), '-o', str(output)]
                        result = subprocess.run(command, capture_output=True, text=True)
                        output.with_suffix('.log').write_text(result.stdout + result.stderr)
                        if result.returncode:
                            raise RuntimeError(f'Compilation failed: {output.with_suffix(".log")}')
                        assemblies.append(output)
                        storage[filename] = static_storage(output)
                    measurements[version] = dict(stack=analyze(assemblies, 10000), static_sections=storage,
                                                  static_bytes=sum(sum(item.values()) for item in storage.values()))
                peak = measurements['after']['stack']['conservative_internal_bytes']
                passed = peak < 400
                if 'before' in measurements:
                    passed &= measurements['before']['static_sections'] == measurements['after']['static_sections']
                failed |= not passed
                report['results'].append(dict(configuration=configuration, profile=profile, enums=mode,
                                              passed=passed, measurements=measurements))
                print(f'{configuration}/{profile}/{mode}: {peak} bytes; {"passed" if passed else "FAILED"}', flush=True)
    report['passed'] = not failed
    (arguments.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
