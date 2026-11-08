"""Summarize isolated small-write pair-hit CPU candidates."""
import argparse
import json
from pathlib import Path
import statistics

from full_upstream_report import FIELDS
from small_write_cpu_results import collect

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--directory', type=Path, default=Path('tests/build/small-write-cpu2'))
parser.add_argument('--repetitions', type=int, default=11)
parser.add_argument('--variants', nargs='+', default=['before', 'arithmetic', 'hit', 'both', 'upstream'])
arguments = parser.parse_args()
variants = arguments.variants
results = collect(arguments.directory, variants, arguments.repetitions, ('interleaved',))
baseline = results['interleaved-before']
for variant in variants[1:-1]:
    current = results[f'interleaved-{variant}']
    for key in baseline['io']:
        assert all(baseline['io'][key][field] == current['io'][key][field] for field in FIELDS), (variant, key)
for key in baseline['cpu']:
    if key.startswith('write/0/1/'):
        print(key)
        for variant in variants:
            current = results[f'interleaved-{variant}']['cpu'][key]
            delta = statistics.median([(right/left-1)*100 for left, right in zip(baseline['cpu'][key]['samples'], current['samples'])])
            print(f"  {variant}: {current['median']:.3f} cycles/B, paired {delta:+.2f}%")
(arguments.directory / 'experiment.json').write_text(json.dumps(results, indent=2))
