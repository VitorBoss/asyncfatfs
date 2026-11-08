"""Freeze the selected batch-link optimization and prepare its final validation."""
from pathlib import Path
import json
import shutil

from full_upstream_report import digest, fixed_clock

experiment = Path('tests/build/small-write-cpu')
root = experiment / 'final'
root.mkdir(parents=True, exist_ok=True)
old = Path('tests/build/full-upstream-20261119')
for source, name in ((old / 'normal/lib', 'before'), (Path('lib'), 'normal')):
    shutil.copytree(source, root / name / 'lib', dirs_exist_ok=True)
for name, source in (('before', root / 'before/lib'), ('after', root / 'normal/lib')):
    target = root / f'{name}-bench/lib'
    shutil.copytree(source, target, dirs_exist_ok=True)
    path = target / 'asyncfatfs.c'
    path.write_text(fixed_clock(path.read_text()))
assert (root / 'after-bench/lib/asyncfatfs.c').read_text() == (experiment / 'links/lib/asyncfatfs.c').read_text()
shutil.copytree(old / 'upstream', root / 'upstream', dirs_exist_ok=True)
for filename in ('harness.c', 'portability.h'):
    shutil.copyfile(experiment / filename, root / filename)
for schedule in ('serial', 'interleaved'):
    for kind in ('cpu', 'io'):
        for source_name, target_name in (('before', 'before'), ('links', 'after'), ('upstream', 'upstream')):
            shutil.copytree(experiment / f'{kind}-{schedule}-{source_name}', root / f'{kind}-{schedule}-{target_name}', dirs_exist_ok=True)
        for profile, old_profile in (('multi', 'multiread'), ('query', 'query')):
            shutil.copytree(old / f'{kind}-{schedule}-{old_profile}', root / f'{kind}-{schedule}-before_{profile}', dirs_exist_ok=True)
runner = Path('tests/run_full_upstream_comparison.ps1').read_text()
runner = runner.replace("'tests/build/full-upstream-20261119'", "'tests/build/small-write-cpu/final'")
runner = runner.replace("@('default', 'multiread', 'query', 'upstream', 'upstream_min2')",
    "@('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')")
runner = runner.replace('if (!$RunOnly) {', 'if (!$RunOnly -and !(Test-Path "$rootAbsolute/cpu-$schedule-$variant/benchmark.exe")) {')
runner = runner.replace("$sourceName = if ($variant.StartsWith('upstream')) { $variant } else { 'current' }",
    "$sourceName = if ($variant -eq 'upstream') { 'upstream' } elseif ($variant.StartsWith('before')) { 'before-bench' } else { 'after-bench' }")
runner = runner.replace("$variant -in @('multiread', 'query')", "$variant -match '_(multi|query)$'")
runner = runner.replace("$variant -eq 'query'", "$variant.EndsWith('_query')")
Path('tests/run_small_write_cpu_confirmation.ps1').write_text(runner)
workspace = root / 'validation'
shutil.copytree(root / 'normal/lib', workspace / 'lib', dirs_exist_ok=True)
shutil.copytree(root / 'before/lib', workspace / 'before/lib', dirs_exist_ok=True)
(workspace / 'tests').mkdir(parents=True, exist_ok=True)
for path in Path('tests').iterdir():
    if path.is_file():
        shutil.copyfile(path, workspace / 'tests' / path.name)
shutil.copytree('tests/config', workspace / 'tests/config', dirs_exist_ok=True)
path = workspace / 'tests/config_common.h'
path.write_text(path.read_text().replace('#define AFATFS_NUM_CACHE_SECTORS 4', '#define AFATFS_NUM_CACHE_SECTORS 8'))
manifest = dict(upstream=json.loads((old / 'manifest.json').read_text())['upstream'],
    production={path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()},
    sources={name: {path.name: digest(path) for path in (root / name / 'lib').iterdir() if path.is_file()}
             for name in ('before', 'normal', 'before-bench', 'after-bench', 'upstream')},
    harness=digest(root / 'harness.c'))
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2))
print(root)
