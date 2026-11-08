"""Validate and publish the final in-page FAT batch-link CPU comparison."""
import json
from pathlib import Path
import statistics

from cache_index_cpu_report import resources
from full_upstream_report import digest, FIELDS, log_data, rows
from mirror_profile_fix_report import object_code
from small_write_cpu_results import collect

root = Path('tests/build/small-write-cpu/final')
manifest = json.loads((root / 'manifest.json').read_text())
assert all(digest(Path('lib') / name) == value for name, value in manifest['production'].items())
for snapshot, sources in manifest['sources'].items():
    assert all(digest(root / snapshot / 'lib' / name) == value for name, value in sources.items())
assert digest(root / 'harness.c') == manifest['harness']
variants = ('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')
repetitions = 21
results = collect(root, variants, repetitions)
assert len(results) == 14
target = {}
for variant in ('before_multi', 'after_multi'):
    directory = root / f'target-confirm/cpu-interleaved-{variant}'
    assert digest(directory / 'benchmark.exe') == digest(root / f'cpu-interleaved-{variant}/benchmark.exe')
    samples = {}
    for trial in range(31):
        measured, _ = log_data(directory / f'trial-{trial}.log', True)
        counters = rows(directory / f'trial-{trial}.csv')
        for key, record in measured.items():
            if key.startswith('write/'):
                assert all(counters[key][field] == results[f'interleaved-{variant}']['io'][key][field] for field in FIELDS)
            samples.setdefault(key, []).append(record['cycles'] / record['bytes'])
    target[variant] = {key: dict(median=statistics.median(values), samples=values,
        quartiles=statistics.quantiles(values, n=4, method='inclusive')) for key, values in samples.items()}
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
for record in footprints.values():
    assert record['before']['static_ram_bytes'] == record['after']['static_ram_bytes']
checks = root / 'validation/checks'
assert len(list(checks.glob('*/test.exe'))) == 28
recovery = {}
for profile in ('mirror_cache', 'mirror_cache_multi', 'mirror_cache_query'):
    for scenario in ('requested_recovery', 'small_collection'):
        log = (checks / f'{scenario}_{profile}/results.log').read_text()
        assert 'retry scenarios' in log
        recovery[f'{scenario}_{profile}'] = log.splitlines()[-1]
peak = max(record['measurements']['after']['stack']['conservative_internal_bytes'] for record in matrix['results'])
lines = ['# CPU de escrita: vínculos FAT preparados por página', '',
    f"Upstream local: `{manifest['upstream']}`. Fonte atual SHA-256: `{manifest['production']['asyncfatfs.c']}`.", '',
    'A é a versão anterior; B é a otimização mantida. Cache de oito setores, duas FATs, reserva '
    'especulativa única e RAM estática preservados. A janela continua contendo somente clusters '
    'já pedidos. Não há buffer, campo ou reserva adicional.', '',
    '## Alteração', '',
    'Depois dos ACKs dos EOCs nas duas FATs, a fase BATCH_LINKS adquire uma vez o par de páginas '
    'da janela, prepara todos os vínculos reversos na primária e espelha a imagem uma vez. '
    'Antes fazia aquisição/marcação/cópia a cada vínculo. A janela já é limitada à mesma página FAT, '
    'inclusive quando intercala posições de escritores. Bits altos de cada entrada são preservados. '
    'O vínculo anterior, que pode pertencer a outra página, continua no caminho individual. '
    'Uma aquisição incompleta não altera entradas nem avança o progresso. A barreira posterior '
    'das duas FATs continua antes da publicação do diretório.', '',
    '## Método', '',
    'MSVC x64 /O2, 21 repetições alternadas, afinidade fixa e aquecimento, sem compilações concorrentes '
    'durante a medição. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, 257/4096 bytes por pedido, '
    'clusters de 512/4096 bytes, ritmos serial/intercalado. Padrão, leitura multibloco e consulta. '
    'CPU usa QueryThreadCycleTime no perfil de baixa latência; inclui biblioteca, backend RAM, polling '
    'e aplicação. Throughput host usa tempo real medido; não representa SD/PIC24. '
    'As leituras CPU usam 16 passagens. Deltas B/A são medianas das razões pareadas; positivo é pior. '
    'Upstream é a versão original, com mínimo multibloco quatro; a atual usa dois. '
    'Upstream não grava FAT2 no workload e tem espelhos diferentes ao final.', '',
    '## Escritas de 257 bytes — clusters de 512 bytes', '',
    '| Ritmo | Perfil | Escritores | CPU A → B (ciclos/B) | Δ pareado | Δ medianas | CPU upstream | Host A → B (MiB/s) |',
    '|---|---|---:|---:|---:|---:|---:|---:|']
for schedule in ('serial', 'interleaved'):
    for profile in ('', '_multi', '_query'):
        for files in (1, 2):
            key = f'write/0/1/257/{files}'
            before = results[f'{schedule}-before{profile}']['cpu'][key]
            after = results[f'{schedule}-after{profile}']['cpu'][key]
            upstream = results[f'{schedule}-upstream']['cpu'][key]['median']
            ratio = (after['median'] / before['median'] - 1) * 100
            lines.append(f"| {schedule} | {profile or 'padrão'} | {files} | {before['median']:.3f} → {after['median']:.3f} | {paired[schedule+profile][key]:+.2f}% | {ratio:+.2f}% | {upstream:.3f} | {before['host_mib_s']:.2f} → {after['host_mib_s']:.2f} |")
lines += ['', '## Confirmação isolada do perfil multibloco intercalado', '',
    'A rodada ampla teve alta dispersão em um escritor/257: delta pareado e razão das medianas '
    'discordaram no sinal. Sem recompilar, os mesmos executáveis foram repetidos em 31 pares '
    'AB/BA, com três aquecimentos. As duas rodadas são mantidas no JSON.', '',
    '| Escritores/bloco | CPU A → B (ciclos/B) | Δ pareado | Δ medianas |', '|---|---:|---:|---:|']
for files, chunk in ((1, 257), (2, 257), (1, 4096), (2, 4096)):
    key = f'write/0/1/{chunk}/{files}'
    before = target['before_multi'][key]
    after = target['after_multi'][key]
    delta = statistics.median([(right/left-1)*100 for left, right in zip(before['samples'], after['samples'])])
    ratio = (after['median']/before['median']-1)*100
    lines.append(f"| {files}/{chunk} | {before['median']:.3f} → {after['median']:.3f} | {delta:+.2f}% | {ratio:+.2f}% |")
lines += ['', '## Demais cenários de CPU — Δ B/A', '',
    'Inclui regressões e variação do host; os ciclos e quartis completos estão no JSON.', '',
    '| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |', '|---|---|---:|---|---:|']
for name, records in paired.items():
    for key, change in records.items():
        phase, _, cluster, chunk, files = key.split('/')
        if phase == 'write' and cluster == '1' and chunk == '257':
            continue
        lines.append(f'| {name} | {phase} | {int(cluster)*512} | {files}/{chunk} | {change:+.2f}% |')
lines += ['', '## I/O preservado', '',
    'Todos os 192 pares A/B de fases auditadas preservaram exatamente leituras, escritas, '
    'metadados por FAT/diretório, polls e sessões/blocos multibloco, em ambas as latências. '
    'Conteúdo integral, igualdade das FATs e ordem EOC/vínculo/diretório conferidos. '
    'A tabela abaixo usa padrão/intercalado; A e B têm os mesmos valores.', '',
    '| Latência | Escritores/bloco | R | W | Metadados |', '|---:|---|---:|---:|---:|']
for latency in (0, 1):
    for files, chunk in ((1, 257), (2, 257), (1, 4096), (2, 4096)):
        record = results['interleaved-after']['io'][f'write/{latency}/1/{chunk}/{files}']
        lines.append(f"| {latency} | {files}/{chunk} | {record['reads']} | {record['writes']} | {record['metadata_writes']} |")
lines += ['', '## RAM, pilha e persistência', '',
    f'Recursos x64/x86: `{json.dumps(footprints)}`. Matriz PIC24 de 48 configurações: '
    f'RAM estática igual em cada par; pilha interna máxima **{peak} B**, abaixo de 400 B '
    '(inclui PCs internos; exclui backend, aplicação e libc).', '',
    'Passaram os 28 testes de alocação/cache, incluindo três escritores, volume quase cheio, '
    'reserva, páginas de espelho reconstruídas, fsync imediato, buffers em trânsito imutáveis '
    'e seis campanhas de corte/recuperação/retry nos três perfis. Detalhes das campanhas:', '']
lines += [f'- {name}: {message}' for name, message in recovery.items()]
lines += ['', f'Evidências e fontes congeladas: `{root.as_posix()}`. '
    'As alternativas de simplificação do fwrite e polling não foram mantidas; '
    'as primeiras rodadas estão em tests/build/small-write-cpu.']
output = Path('tests/SMALL_WRITE_CPU.md')
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, repetitions=repetitions,
    results=results, isolated_confirmation=target, paired_cpu_delta_percent=paired, matrix=matrix, resources=footprints, recovery=recovery), indent=2))
print(output)
