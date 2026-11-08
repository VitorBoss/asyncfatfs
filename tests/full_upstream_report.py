"""Freeze sources and publish a complete CPU, I/O and throughput comparison."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess

from cache_index_cpu_report import resources
from mirror_profile_fix_report import object_code

VARIANTS = ('default', 'multiread', 'query', 'upstream', 'upstream_min2')
SCHEDULES = ('serial', 'interleaved')
FIELDS = ('payload_bytes', 'reads', 'writes', 'metadata_reads', 'metadata_writes', 'polls',
          'virtual_us', 'multi_sessions', 'multi_blocks')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixed_clock(source):
    source, dates = re.subn(r'(?m)^#define AFATFS_DEFAULT_FILE_DATE .*$',
        '#define AFATFS_DEFAULT_FILE_DATE FAT_MAKE_DATE(2026, 11, 19)', source)
    source, times = re.subn(r'(?m)^#define AFATFS_DEFAULT_FILE_TIME .*$',
        '#define AFATFS_DEFAULT_FILE_TIME FAT_MAKE_TIME(12, 0, 0)', source)
    assert dates == times == 1
    return source


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    shutil.copytree('lib', root / 'normal/lib', dirs_exist_ok=True)
    shutil.copytree('lib', root / 'current/lib', dirs_exist_ok=True)
    upstream = root / 'raw-upstream/lib'
    upstream.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip()
    files = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', commit, 'lib'], text=True).splitlines()
    for name in files:
        (upstream / Path(name).name).write_bytes(subprocess.check_output(['git', 'show', f'{commit}:{name}']))
    shutil.copytree(upstream, root / 'upstream/lib', dirs_exist_ok=True)
    for name in ('current', 'upstream'):
        path = root / name / 'lib/asyncfatfs.c'
        path.write_text(fixed_clock(path.read_text()))
    shutil.copytree(root / 'upstream/lib', root / 'upstream_min2/lib', dirs_exist_ok=True)
    path = root / 'upstream_min2/lib/asyncfatfs.c'
    source = path.read_text()
    assert source.count('#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 4') == 1
    path.write_text(source.replace('#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 4',
                                  '#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2'))
    source = Path('tests/upstream_comparison.c').read_text()
    old = '''#ifdef COMPARISON_CPU_ONLY
                if (clusterSectors != 1) {
                    break;
                }
#endif'''
    assert source.count(old) == 1
    source = source.replace(old, '')
    (root / 'harness.c').write_text(source)
    (root / 'portability.h').write_text('#define __attribute__(attributes)\n#pragma pack(push, 1)\n#include "fat_standard.h"\n#pragma pack(pop)\n')
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'current/lib/asyncfatfs_config.h').read_text()
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'upstream/lib/asyncfatfs.c').read_text()
    manifest = dict(upstream=commit, host_cpu=os.environ.get('PROCESSOR_IDENTIFIER'),
        production={path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()},
        sources={name: {path.name: digest(path) for path in (root / name / 'lib').iterdir() if path.is_file()}
                 for name in ('normal', 'current', 'raw-upstream', 'upstream', 'upstream_min2')},
        harness=digest(root / 'harness.c'))
    (root / 'manifest.json').write_text(json.dumps(manifest, indent=2))


def rows(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        result = {}
        for row in csv.DictReader(stream):
            key = '/'.join(row[name] for name in ('phase', 'profile', 'cluster_sectors', 'chunk_bytes', 'files'))
            assert key not in result
            result[key] = {field: int(row[field]) for field in FIELDS}
            result[key]['host_seconds'] = float(row['host_seconds'])
    return result


def log_data(path, current):
    result = {}
    mirrors = []
    for line in path.read_text().splitlines():
        parts = line.split(',')
        if parts[0] in ('CPU_CYCLES', 'META', 'META_READ', 'READ_BATCH'):
            key = '/'.join(parts[1:6])
            record = result.setdefault(key, {})
            fields = {'CPU_CYCLES': ('cycles', 'bytes'), 'META': ('fat1', 'fat2', 'directory'),
                'META_READ': ('fat1_reads', 'fat2_reads', 'directory_reads'),
                'READ_BATCH': ('read_sessions', 'read_blocks', 'read_stops')}[parts[0]]
            assert not any(field in record for field in fields)
            record.update(zip(fields, map(int, parts[7:])))
        elif parts[0] == 'MIRROR':
            mirrors.append(tuple(map(int, parts[1:])))
            if current:
                assert parts[-1] == '0'
    return result, mirrors


def summary(values):
    quartiles = statistics.quantiles(values, n=4, method='inclusive')
    return dict(median=statistics.median(values), q1=quartiles[0], q3=quartiles[2], samples=values)


def publish(root, repetitions, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert all(digest(Path('lib') / name) == value for name, value in manifest['production'].items())
    assert digest(root / 'harness.c') == manifest['harness']
    for snapshot, sources in manifest['sources'].items():
        assert all(digest(root / snapshot / 'lib' / name) == value for name, value in sources.items())
    assert subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip() == manifest['upstream']
    results = {}
    footprints = {}
    for variant in VARIANTS:
        current = not variant.startswith('upstream')
        footprints[variant] = resources(root / f'cpu-serial-{variant}')
        for filename in ('asyncfatfs', 'fat_standard'):
            expected = object_code(root / f'cpu-serial-{variant}/{filename}.obj')
            for schedule in SCHEDULES:
                for kind in ('cpu', 'io'):
                    assert object_code(root / f'{kind}-{schedule}-{variant}/{filename}.obj') == expected
        for schedule in SCHEDULES:
            name = f'{schedule}-{variant}'
            io = rows(root / f'io-{name}/results.csv')
            trace, mirrors = log_data(root / f'io-{name}/audit.log', current)
            assert len(io) == len(trace) == 32
            for key, row in io.items():
                row.update(trace[key])
                assert sum(row[field] for field in ('fat1', 'fat2', 'directory')) == row['metadata_writes']
                assert sum(row[field] for field in ('fat1_reads', 'fat2_reads', 'directory_reads')) == row['metadata_reads']
                assert row['payload_bytes'] == int(key.split('/')[-1]) * 1048576
                row['simulated_mib_s'] = row['payload_bytes'] / 1048576 / (row['virtual_us'] / 1e6)
            samples = {}
            for trial in range(repetitions):
                observed = rows(root / f'cpu-{name}/trial-{trial}.csv')
                measured, _ = log_data(root / f'cpu-{name}/trial-{trial}.log', current)
                assert len(observed) == len(measured) == 16
                for key, row in observed.items():
                    assert measured[key]['bytes'] == row['payload_bytes']
                    record = samples.setdefault(key, dict(cycles=[], throughput=[], counters={field: row[field] for field in FIELDS}))
                    assert record['counters'] == {field: row[field] for field in FIELDS}
                    if key.startswith('write/'):
                        assert all(row[field] == io[key][field] for field in FIELDS)
                    record['cycles'].append(measured[key]['cycles'] / row['payload_bytes'])
                    record['throughput'].append(row['payload_bytes'] / 1048576 / row['host_seconds'])
            results[name] = dict(io=io, mirrors=mirrors, cpu={key: dict(cycles_per_byte=summary(value['cycles']),
                host_mib_s=summary(value['throughput']), counters=value['counters']) for key, value in samples.items()})
    matrix = json.loads(Path('tests/build/small-write-collection/adopted-matrix/report.json').read_text())
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['production']['asyncfatfs.c'] and matrix['passed']
    lines = ['# Comparação completa com upstream/master', '',
        f"Referência local: `{manifest['upstream']}`. Fonte atual: `{manifest['production']['asyncfatfs.c']}`.", '',
        '## Principais resultados', '',
        'Com clusters de 512 bytes, as escritas intercaladas de 257 bytes reduzem I/O, mas custam '
        '28,45%/33,48% mais CPU por byte com um/dois escritores. Dois escritores/4096 bytes '
        'reduzem CPU em 4,38%. Com clusters de 4096 bytes, o custo de CPU da escrita aumenta '
        '30,66% a 73,49%. Menos I/O não significa automaticamente menos CPU.', '',
        'A leitura multibloco melhora o throughput simulado de um leitor com clusters de 512 bytes, '
        'com custo adicional de CPU. Dois leitores não obtêm esse ganho no modelo medido. '
        'O controle do limiar multibloco do upstream não explica a diferença principal nas escritas.', '',
        '## Método', '',
        'FAT32 8.3, cache de oito setores em ambas as versões, arquivos de 1 MiB por arquivo. '
        'Leitura/escrita, um/dois arquivos, blocos de 257/4096 bytes, clusters de 512/4096 bytes. '
        'Escritas seriais e intercaladas; no intercalado os buffers da aplicação são preparados uma vez '
        'por pedido, evitando geração adicional de dados a cada retry.', '',
        f'MSVC x64 /O2, {repetitions} repetições alternadas, afinidade em um processador e um aquecimento '
        'por variante, sem compilações concorrentes durante a medição. CPU usa QueryThreadCycleTime; '
        'throughput do host usa QueryPerformanceCounter. Leitura de CPU tem 16 passagens, incluindo rewind; '
        'I/O e backend simulado usam uma passagem de dados após remontagem; metadados acessados '
        'na montagem podem permanecer no cache. Formatação/montagem ficam fora da medição. '
        'Escrita inclui open, close e drain. CPU usa somente o perfil de baixa latência 0; '
        'o throughput simulado das tabelas usa o perfil 1. Os dois não são somados.', '',
        'Ciclos/byte incluem biblioteca, backend em RAM, polling e trabalho da aplicação/harness. '
        'A leitura inclui verificação byte a byte; a escrita inclui preparação dos buffers. '
        'Não são ciclos exclusivos da biblioteca, medidas no PIC24 ou throughput em cartão físico. '
        'Medianas e quartis por cenário estão no JSON; deltas são razões das medianas. '
        'Diferenças pequenas perante a dispersão não demonstram ganho.', '',
        'Os objetos da biblioteca são idênticos entre executáveis de CPU e auditoria e entre ritmos. '
        'A auditoria de I/O tem contadores adicionais somente no harness. Data/hora FAT foram fixadas '
        'igualmente nas cópias de benchmark; a produção não foi modificada.', '',
        'Atual padrão: escrita multibloco mínima 2; upstream original: mínima 4. O controle upstream_min2 '
        'altera somente esse limiar. Perfis atuais multiread e query habilitam leitura multibloco opcional.', '',
        '## Diferença de persistência', '',
        'A versão atual mantém as duas FATs e suas barreiras, reserva especulativa única e recuperação '
        'de alocações pendentes. O upstream deixa diferenças entre as FATs neste workload. '
        'Seu menor custo em alguns cenários não representa a mesma garantia de espelhamento. '
        'Todas as versões passaram na conferência do conteúdo e da cadeia primária; a atual também '
        'passou na igualdade das FATs e na auditoria de EOC/vínculo/diretório por gravação.', '',
        '## CPU e throughput no host — padrão, intercalado', '',
        'CPU menor é melhor; throughput maior é melhor. Δ CPU positivo indica custo maior da atual.', '',
        '| Operação | Cluster B | Arquivos/bloco | CPU upstream → atual (ciclos/B) | Δ CPU | Host upstream → atual (MiB/s) |',
        '|---|---:|---|---:|---:|---:|']
    for phase in ('write', 'read'):
        for cluster in (1, 8):
            for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
                key = f'{phase}/0/{cluster}/{chunk}/{files}'
                before = results['interleaved-upstream']['cpu'][key]
                after = results['interleaved-default']['cpu'][key]
                ratio = (after['cycles_per_byte']['median'] / before['cycles_per_byte']['median'] - 1) * 100
                lines.append(f"| {phase} | {cluster*512} | {files}/{chunk} | {before['cycles_per_byte']['median']:.3f} → {after['cycles_per_byte']['median']:.3f} | {ratio:+.2f}% | {before['host_mib_s']['median']:.2f} → {after['host_mib_s']['median']:.2f} |")
    lines += ['', '## CPU e throughput no host — padrão, serial', '',
        '| Operação | Cluster B | Arquivos/bloco | CPU upstream → atual (ciclos/B) | Δ CPU | Host upstream → atual (MiB/s) |',
        '|---|---:|---|---:|---:|---:|']
    for phase in ('write', 'read'):
        for cluster in (1, 8):
            for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
                key = f'{phase}/0/{cluster}/{chunk}/{files}'
                before = results['serial-upstream']['cpu'][key]
                after = results['serial-default']['cpu'][key]
                ratio = (after['cycles_per_byte']['median'] / before['cycles_per_byte']['median'] - 1) * 100
                lines.append(f"| {phase} | {cluster*512} | {files}/{chunk} | {before['cycles_per_byte']['median']:.3f} → {after['cycles_per_byte']['median']:.3f} | {ratio:+.2f}% | {before['host_mib_s']['median']:.2f} → {after['host_mib_s']['median']:.2f} |")
    for schedule in SCHEDULES:
        lines += ['', f'## I/O e throughput simulado — padrão, {schedule}', '',
            'R/W/metadados são setores de 512 bytes; números de metadados fazem parte de W. '
            'Perfil realista do simulador: leitura simples 200 µs, leitura multibloco 100 µs, '
            'escrita simples 800 µs, multibloco 400 µs; sessões podem custar 100 µs e cada poll 10 µs. '
            'O throughput virtual é derivado desse modelo, sem somar o tempo de CPU do host.', '',
            '| Operação | Cluster B | Arquivos/bloco | R upstream → atual | W upstream → atual | Metadados upstream → atual | Backend upstream → atual (MiB/s) |',
            '|---|---:|---|---:|---:|---:|---:|']
        for phase in ('write', 'read'):
            for cluster in (1, 8):
                for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
                    key = f'{phase}/1/{cluster}/{chunk}/{files}'
                    before = results[f'{schedule}-upstream']['io'][key]
                    after = results[f'{schedule}-default']['io'][key]
                    lines.append(f"| {phase} | {cluster*512} | {files}/{chunk} | {before['reads']} → {after['reads']} | {before['writes']} → {after['writes']} | {before['metadata_writes']} → {after['metadata_writes']} | {before['simulated_mib_s']:.3f} → {after['simulated_mib_s']:.3f} |")
    lines += ['', '## Composição das escritas — padrão, intercalado, latência 1', '',
        'Cada célula mostra upstream → atual; contagens são setores, incluindo regravações.', '',
        '| Cluster B | Escritores/bloco | Dados | FAT1 | FAT2 | Diretório |',
        '|---:|---|---:|---:|---:|---:|']
    for cluster in (1, 8):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/1/{cluster}/{chunk}/{files}'
            before = results['interleaved-upstream']['io'][key]
            after = results['interleaved-default']['io'][key]
            data = f"{before['writes']-before['metadata_writes']} → {after['writes']-after['metadata_writes']}"
            detail = ' | '.join(f"{before[field]} → {after[field]}" for field in ('fat1', 'fat2', 'directory'))
            lines.append(f'| {cluster*512} | {files}/{chunk} | {data} | {detail} |')
    lines += ['', '## Perfis opcionais de leitura — intercalado', '',
        'CPU é mediana no host; backend é o perfil de latência realista. Cache continua oito setores.', '',
        '| Cluster B | Leitores/bloco | CPU padrão / multiread / query / upstream | Backend padrão / multiread / query / upstream (MiB/s) |',
        '|---:|---|---:|---:|']
    for cluster in (1, 8):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            cpu_key = f'read/0/{cluster}/{chunk}/{files}'
            io_key = f'read/1/{cluster}/{chunk}/{files}'
            cpu = ' / '.join(f"{results[f'interleaved-{variant}']['cpu'][cpu_key]['cycles_per_byte']['median']:.3f}" for variant in ('default', 'multiread', 'query', 'upstream'))
            speed = ' / '.join(f"{results[f'interleaved-{variant}']['io'][io_key]['simulated_mib_s']:.3f}" for variant in ('default', 'multiread', 'query', 'upstream'))
            lines.append(f'| {cluster*512} | {files}/{chunk} | {cpu} | {speed} |')
    lines += ['', '## Controle de escrita multibloco mínima 2 — intercalado', '',
        '| Cluster B | Escritores/bloco | CPU upstream 4 / upstream 2 / atual 2 | Backend upstream 4 / upstream 2 / atual 2 (MiB/s) |',
        '|---:|---|---:|---:|']
    for cluster in (1, 8):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            cpu_key = f'write/0/{cluster}/{chunk}/{files}'
            io_key = f'write/1/{cluster}/{chunk}/{files}'
            cpu = ' / '.join(f"{results[f'interleaved-{variant}']['cpu'][cpu_key]['cycles_per_byte']['median']:.3f}" for variant in ('upstream', 'upstream_min2', 'default'))
            speed = ' / '.join(f"{results[f'interleaved-{variant}']['io'][io_key]['simulated_mib_s']:.3f}" for variant in ('upstream', 'upstream_min2', 'default'))
            lines.append(f'| {cluster*512} | {files}/{chunk} | {cpu} | {speed} |')
    lines += ['', '## Memória e evidências', '',
        f'Recursos dos objetos x64 (biblioteca sem harness): `{json.dumps(footprints)}`. '
        'A matriz de persistência/memória da versão atual é a validada em SMALL_WRITE_COLLECTION: '
        '48 configurações, pilha interna máxima 378 B. O orçamento não inclui backend/aplicação/libc.', '',
        'JSON contém todos os cenários, amostras, quartis, CPU/byte, throughput do host, contagens de '
        'metadados por FAT/diretório, sessões multibloco e diferenças de espelhamento, inclusive latência 0. '
        f'Evidências congeladas: `{root.as_posix()}`. Nenhuma conclusão de velocidade física de SD é inferida.']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, repetitions=repetitions,
        read_passes=16, results=results, footprints=footprints), indent=2) + '\n', encoding='utf-8')
    print(f'Validated {len(results)} configurations, {repetitions} CPU trials each; {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/full-upstream-20261119'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, default=Path('tests/FULL_UPSTREAM_COMPARISON.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    else:
        publish(arguments.directory, arguments.repetitions, arguments.output)
