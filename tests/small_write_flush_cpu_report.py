"""Validate and publish CPU gains from read-only flush-policy filtering."""
import json
from pathlib import Path
import statistics
import subprocess

from cache_index_cpu_report import resources
from full_upstream_report import digest, FIELDS
from mirror_profile_fix_report import object_code
from small_write_cpu_results import collect

root = Path('tests/build/small-write-flush-cpu/final')
manifest = json.loads((root / 'manifest.json').read_text())
assert all(digest(Path('lib') / name) == value for name, value in manifest['production'].items())
for snapshot, sources in manifest['sources'].items():
    assert all(digest(root / snapshot / 'lib' / name) == value for name, value in sources.items())
assert digest(root / 'harness.c') == manifest['harness']
assert subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip() == manifest['upstream']
variants = ('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')
repetitions = 21
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
assert 'checkCollectPageRange();' in (root / 'validation/tests/fat32_cache_mirror.c').read_text()
peak = max(record['measurements']['after']['stack']['conservative_internal_bytes'] for record in matrix['results'])
default_deltas = [change for schedule in ('serial', 'interleaved') for key, change in paired[schedule].items()
                  if key.startswith('write/0/1/257/')]
lines = ['# CPU: filtrar candidatas antes das políticas de flush', '',
    f"Upstream local: `{manifest['upstream']}`. Fonte atual SHA-256: `{manifest['production']['asyncfatfs.c']}`.", '',
    'A é a versão anterior, com acesso direto ao par FAT sujo; B acrescenta a filtragem abaixo. '
    'Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.', '',
    f'No padrão, os quatro casos de 257 bytes/cluster 512 reduziram CPU pareada entre '
    f'{-max(default_deltas):.2f}% e {-min(default_deltas):.2f}%. O I/O permaneceu idêntico. '
    'Nos perfis opcionais, vários deltas são pequenos e alguns são positivos; não há ganho '
    'geral comprovado nesses perfis. Todos os resultados estão registrados abaixo.', '',
    '## Alteração', '',
    'afatfs_flushCacheFiltered ignora páginas limpas, travadas e com timestamp que não pode '
    'superar a candidata atual antes de consultar as políticas de retenção. O perfil multibloco '
    'já usava essa ordem; o padrão passa a compartilhar o mesmo laço. '
    'As políticas são consultas puras: não avançam estados, timers ou I/O. '
    'A candidata final e o desempate pelo primeiro índice são preservados, inclusive timestamps iguais.', '',
    'afatfs_collectFatPage rejeita setores fora das duas regiões FAT antes de percorrer os escritores. '
    'A diferença unsigned relativa ao início da FAT e duas subtrações evitam multiplicação com '
    'overflow. Se o setor é da primária ou do espelho de uma página válida, passa pelo filtro '
    'mesmo quando os endereços físicos dão a volta. O laço de proprietários permanece como antes.', '',
    'Nenhuma alteração nos EOCs, vínculos, reservas, limites da coleção, timestamps do cache, '
    'locks, submissões ou ACKs. As barreiras das duas FATs antes do diretório permanecem.', '',
    '## Método', '',
    'MSVC x64 /O2, 21 repetições alternadas, afinidade fixa, aquecimento e sem compilações '
    'concorrentes durante CPU. FAT32 8.3, arquivos de 1 MiB, um/dois arquivos, 257/4096 bytes, '
    'clusters 512/4096 bytes, serial/intercalado e padrão/multiread/query. '
    'QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação; throughput host '
    'usa tempo real medido. Não são medições de PIC24 ou cartão físico. Leitura CPU tem 16 passagens. '
    'Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. '
    'O JSON registra quartis e todas as amostras; diferenças pequenas podem ser ruído.', '',
    'Upstream original: mínimo multibloco quatro; atual: dois. O controle anterior mostrou que '
    'esse limiar não explica a diferença principal. Upstream não grava FAT2 neste workload e '
    'termina com espelhos distintos; as garantias de espelhamento não são iguais.', '',
    '## Escritas de 257 bytes — clusters de 512 bytes', '',
    '| Ritmo | Perfil | Escritores | CPU A → B ciclos/B | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |',
    '|---|---|---:|---:|---:|---:|---:|---:|---:|']
for schedule in ('serial', 'interleaved'):
    for profile in ('', '_multi', '_query'):
        for files in (1, 2):
            key = f'write/0/1/257/{files}'
            before = results[f'{schedule}-before{profile}']['cpu'][key]
            after = results[f'{schedule}-after{profile}']['cpu'][key]
            upstream = results[f'{schedule}-upstream']['cpu'][key]['median']
            ratio = (after['median'] / before['median'] - 1) * 100
            excess = f"{(before['median']/upstream-1)*100:+.2f}% → {(after['median']/upstream-1)*100:+.2f}%"
            lines.append(f"| {schedule} | {profile or 'padrão'} | {files} | {before['median']:.3f} → {after['median']:.3f} | {paired[schedule+profile][key]:+.2f}% | {ratio:+.2f}% | {upstream:.3f} | {excess} | {before['host_mib_s']:.2f} → {after['host_mib_s']:.2f} |")
lines += ['', '## Demais cenários — Δ CPU pareado B/A', '',
    'Inclui variações e regressões; não generalizar um ganho a todos os caminhos.', '',
    '| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |', '|---|---|---:|---|---:|']
for name, records in paired.items():
    for key, change in records.items():
        phase, _, cluster, chunk, files = key.split('/')
        if phase == 'write' and cluster == '1' and chunk == '257':
            continue
        lines.append(f'| {name} | {phase} | {int(cluster)*512} | {files}/{chunk} | {change:+.2f}% |')
lines += ['', '## I/O preservado', '',
    'Os 192 pares auditados A/B coincidem em leituras, escritas, metadados por FAT/diretório, '
    'polls, sessões e blocos multibloco nas duas latências. O throughput virtual também é igual. '
    'Conteúdo integral, FATs iguais e ordem EOC/vínculo/diretório foram conferidos.', '',
    '| Latência | Escritores/bloco | R | W | Metadados |', '|---:|---|---:|---:|---:|']
for latency in (0, 1):
    for files, chunk in ((1, 257), (2, 257), (1, 4096), (2, 4096)):
        record = results['interleaved-after']['io'][f'write/{latency}/1/{chunk}/{files}']
        lines.append(f"| {latency} | {files}/{chunk} | {record['reads']} | {record['writes']} | {record['metadata_writes']} |")
lines += ['', '## Recursos e validação', '',
    '| ABI | RAM A → B bytes | Código A → B bytes |', '|---|---:|---:|']
for name, record in footprints.items():
    lines.append(f"| {name} | {record['before']['static_ram_bytes']} → {record['after']['static_ram_bytes']} | {record['before']['code_bytes']} → {record['after']['code_bytes']} |")
lines += ['', f'Matriz PIC24 de 48 configurações com seções estáticas iguais em cada par. '
    f'Pilha interna máxima **{peak} bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.', '',
    'Passaram 28 testes de alocação/cache. O teste do espelho inclui agora dois escritores e '
    'quatro geometrias de endereçamento: normal, LBA com volta, comprimento acima de 2³¹ e UINT32_MAX. '
    'Confere ambas as FATs e setores sem proprietário. Os nove cenários anteriores de par sujo, '
    'cacheTimer, cópia parcial e propriedade do backend continuam passando nos três perfis. '
    'Também passaram quase cheio, três escritores, reserva única e fsync imediato. Campanhas:', '']
lines += [f'- {name}: {message}' for name, message in recovery.items()]
lines += ['', f'Evidências congeladas: `{root.as_posix()}`. As três alternativas iniciais '
    'estão em tests/build/small-write-flush-cpu; a versão final usa o teste de intervalo sem overflow.']
output = Path('tests/SMALL_WRITE_FLUSH_CPU.md')
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, repetitions=repetitions,
    results=results, paired_cpu_delta_percent=paired, matrix=matrix, resources=footprints, recovery=recovery), indent=2))
print(output)
