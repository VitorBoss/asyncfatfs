"""Summarize paired trials and verify unchanged cache/backend activity."""
import argparse
import json
from pathlib import Path
import statistics

from full_upstream_report import FIELDS, log_data, rows


def collect(root, variants, repetitions, schedules=('serial', 'interleaved')):
    result = {}
    for schedule in schedules:
        for variant in variants:
            name = f'{schedule}-{variant}'
            io = rows(root / f'io-{name}/results.csv')
            trace, mirrors = log_data(root / f'io-{name}/audit.log', not variant.startswith('upstream'))
            for key, record in io.items():
                record.update(trace[key])
            trials = {}
            throughput = {}
            counters = {}
            for trial in range(repetitions):
                cpu, _ = log_data(root / f'cpu-{name}/trial-{trial}.log', not variant.startswith('upstream'))
                counts = rows(root / f'cpu-{name}/trial-{trial}.csv')
                for key, record in cpu.items():
                    observed = {field: counts[key][field] for field in FIELDS}
                    assert counters.setdefault(key, observed) == observed
                    assert record['bytes'] == counts[key]['payload_bytes']
                    if key.startswith('write/'):
                        assert all(counts[key][field] == io[key][field] for field in FIELDS)
                    trials.setdefault(key, []).append(record['cycles'] / record['bytes'])
                    throughput.setdefault(key, []).append(counts[key]['payload_bytes'] / 1048576 / counts[key]['host_seconds'])
            result[name] = dict(io=io, mirrors=mirrors, cpu={key: dict(median=statistics.median(values),
                samples=values, quartiles=statistics.quantiles(values, n=4, method='inclusive'),
                host_mib_s=statistics.median(throughput[key])) for key, values in trials.items()})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/small-write-cpu'))
    parser.add_argument('--variants', nargs='+', default=['before', 'loop', 'fast', 'upstream'])
    parser.add_argument('--repetitions', type=int, default=9)
    args = parser.parse_args()
    result = collect(args.directory, args.variants, args.repetitions)
    (args.directory / 'summary.json').write_text(json.dumps(result, indent=2))
    for schedule in ('serial', 'interleaved'):
        for key in result[f'{schedule}-{args.variants[0]}']['cpu']:
            if key.startswith('write/0/1/'):
                values = [result[f'{schedule}-{variant}']['cpu'][key]['median'] for variant in args.variants]
                print(schedule, key, ' / '.join(f'{value:.3f} ({(value/values[0]-1)*100:+.2f}%)' for value in values))
