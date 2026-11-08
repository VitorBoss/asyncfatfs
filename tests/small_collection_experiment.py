"""Prepare an isolated small-request EOC collection benchmark."""
from pathlib import Path
import shutil
from metadata_write_audit import diagnostic_harness, instrument

root = Path('tests/build/small-write-collection')
for version in ('before', 'prototype'):
    original = root / version / 'lib'
    target = root / 'audit' / version / 'lib'
    shutil.copytree(original, target, dirs_exist_ok=True)
    (target / 'asyncfatfs.c').write_text(instrument((original / 'asyncfatfs.c').read_text()))
(root / 'audit/harness.c').write_text(diagnostic_harness(Path('tests/upstream_comparison.c').read_text()))
shutil.copyfile('tests/build/small-write-pipeline/audit/portability.h', root / 'audit/portability.h')
