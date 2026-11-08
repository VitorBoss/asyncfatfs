"""Shared validation for paired cache-eight CPU and persistence reports."""
import json
from pathlib import Path
import statistics
import subprocess

from cache_index_cpu_report import resources
from full_upstream_report import digest, FIELDS
from mirror_profile_fix_report import object_code
from small_write_cpu_results import collect


def validate_final(root, repetitions=21):
    """Require matching sources, deterministic I/O, unchanged RAM and bounded stack."""
    manifest = json.loads((root / 'manifest.json').read_text())
    assert all(digest(Path('lib') / name) == value for name, value in manifest['production'].items())
    for snapshot, sources in manifest['sources'].items():
        assert all(digest(root / snapshot / 'lib' / name) == value for name, value in sources.items())
    assert digest(root / 'harness.c') == manifest['harness']
    assert subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip() == manifest['upstream']
    variants = ('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')
    results = collect(root, variants, repetitions)
    for variant in variants:
        for filename in ('asyncfatfs', 'fat_standard'):
            expected = object_code(root / f'cpu-serial-{variant}/{filename}.obj')
            for schedule in ('serial', 'interleaved'):
                for kind in ('cpu', 'io'):
                    assert object_code(root / f'{kind}-{schedule}-{variant}/{filename}.obj') == expected
    paired = {}
    for schedule in ('serial', 'interleaved'):
        for profile in ('', '_multi', '_query'):
            before = results[f'{schedule}-before{profile}']
            after = results[f'{schedule}-after{profile}']
            assert len(before['io']) == len(after['io']) == 32
            assert len(before['cpu']) == len(after['cpu']) == 16
            for key in before['io']:
                for field in (*FIELDS, 'fat1', 'fat2', 'directory', 'fat1_reads', 'fat2_reads', 'directory_reads',
                              'read_sessions', 'read_blocks', 'read_stops'):
                    assert before['io'][key][field] == after['io'][key][field], (schedule, profile, key, field)
            paired[f'{schedule}{profile}'] = {key: statistics.median([(right / left - 1) * 100
                for left, right in zip(before['cpu'][key]['samples'], after['cpu'][key]['samples'])]) for key in before['cpu']}
    matrix = json.loads((root / 'validation/matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['production']['asyncfatfs.c']
    for record in matrix['results']:
        assert record['measurements']['before']['static_sections'] == record['measurements']['after']['static_sections']
        assert record['measurements']['after']['stack']['conservative_internal_bytes'] < 400
    footprints = {'x64': {name: resources(root / f'cpu-serial-{variant}') for name, variant in (('before', 'before'), ('after', 'after'))},
                  'x86': {name: resources(root / f'resources-32-{variant}') for name, variant in (('before', 'before'), ('after', 'normal'))}}
    assert all(record['before']['static_ram_bytes'] == record['after']['static_ram_bytes'] for record in footprints.values())
    checks = root / 'validation/checks'
    assert len(list(checks.glob('*/test.exe'))) == 28
    recovery = {}
    for profile in ('mirror_cache', 'mirror_cache_multi', 'mirror_cache_query'):
        for scenario in ('requested_recovery', 'small_collection'):
            log = (checks / f'{scenario}_{profile}/results.log').read_text()
            assert 'retry scenarios' in log
            recovery[f'{scenario}_{profile}'] = log.splitlines()[-1]
        assert 'FAT mirror cache:' in (checks / profile / 'results.log').read_text()
    peak = max(record['measurements']['after']['stack']['conservative_internal_bytes'] for record in matrix['results'])
    return dict(manifest=manifest, repetitions=repetitions, results=results, paired_cpu_delta_percent=paired,
                matrix=matrix, resources=footprints, recovery=recovery, peak_stack_bytes=peak)
