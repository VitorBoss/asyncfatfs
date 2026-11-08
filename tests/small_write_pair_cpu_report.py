"""Validate dirty-pair collection CPU, unchanged I/O, RAM, stack and recovery."""
import json
from pathlib import Path
import statistics
import subprocess

from cache_index_cpu_report import resources
from full_upstream_report import digest, FIELDS
from mirror_profile_fix_report import object_code
from read_cpu_report import function_code
from small_write_cpu_results import collect

root = Path('tests/build/small-write-cpu2/final')
manifest = json.loads((root / 'manifest.json').read_text())
assert all(digest(Path('lib') / name) == value for name, value in manifest['production'].items())
for snapshot, sources in manifest['sources'].items():
    assert all(digest(root / snapshot / 'lib' / name) == value for name, value in sources.items())
assert digest(root / 'harness.c') == manifest['harness']
assert subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip() == manifest['upstream']
variants = ('before', 'after', 'before_multi', 'after_multi', 'before_query', 'after_query', 'upstream')
repetitions = 21
results = collect(root, variants, repetitions)
generated_code = {profile or 'default': {function: {version: function_code(
    root / f'cpu-serial-{version}{profile}/asyncfatfs.obj', function) for version in ('before', 'after')}
    for function in ('afatfs_fwrite', 'afatfs_fread', 'afatfs_poll', 'afatfs_collectNext')}
    for profile in ('', '_multi', '_query')}
for function in ('afatfs_fread', 'afatfs_poll'):
    assert generated_code['default'][function]['before'] == generated_code['default'][function]['after']
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
assert 'checkCollectedPair();' in (root / 'validation/tests/fat32_cache_mirror.c').read_text()
peak = max(record['measurements']['after']['stack']['conservative_internal_bytes'] for record in matrix['results'])
main_deltas = [change for records in paired.values() for key, change in records.items()
               if key.startswith('write/0/1/257/')]
lines = ['# CPU: coleção com o par FAT já sujo em cache', '',
    f"Upstream local: `{manifest['upstream']}`. Fonte atual SHA-256: `{manifest['production']['asyncfatfs.c']}`.", '',
    'A é a versão anterior, com vínculos preparados por página; B acrescenta o caminho para o par '
    'FAT já DIRTY. Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.', '',
    f'Nos 12 casos de 257 bytes/cluster 512, a redução pareada de CPU ficou entre '
    f'{-max(main_deltas):.2f}% e {-min(main_deltas):.2f}%, com I/O idêntico. '
    'A diferença para upstream diminuiu, mas permanece. Os resultados dos demais cenários, '
    'inclusive variações/regressões, estão registrados abaixo.', '',
    '## Alteração e persistência', '',
    'afatfs_collectNext procura as duas páginas na mesma passagem pelo cache. Se ambas estão DIRTY, '
    'suas imagens já foram pareadas e nenhuma está em uso pelo backend ou pode ser expulsa. '
    'Verifica a entrada livre e as reservas, grava o EOC na primária e copia somente seus quatro '
    'bytes para o espelho. Preserva os bits altos e as entradas vizinhas. '
    'Mantém os três acessos lógicos do caminho anterior: leitura, aquisição da primária e aquisição '
    'do espelho, com os mesmos timestamps e atualização de fsInfoDirty. '
    'Não altera writeTimestamp, dirtyEntries ou retainCount dessas páginas já sujas.', '',
    'Página ausente, limpa ou em transferência usa o caminho anterior integral. '
    'O par só pode ser modificado quando as duas páginas estão disponíveis. As barreiras '
    'EOC nas duas FATs → vínculos nas duas FATs → diretório permanecem. '
    'A janela continua limitada aos pedidos já recebidos, sem nova reserva ou buffer.', '',
    'O código gerado padrão MSVC de fwrite passou de '
    f"{generated_code['default']['afatfs_fwrite']['before']['bytes']} para "
    f"{generated_code['default']['afatfs_fwrite']['after']['bytes']} bytes; o total da biblioteca cresce "
    'conforme a tabela de recursos. O compilador também muda a organização do caminho de escrita. '
    'fread e poll mantêm os mesmos bytes no objeto padrão; diferenças de leitura pequenas podem '
    'refletir disposição do executável e ruído. Esses efeitos não comprovam o mesmo ganho no PIC24.', '',
    '## Método', '',
    'MSVC x64 /O2, 21 repetições alternadas AB/BA, afinidade fixa e aquecimento, sem compilações '
    'concorrentes durante CPU. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, 257/4096 bytes por pedido, '
    'clusters 512/4096 bytes, serial/intercalado, padrão/multiread/query. '
    'CPU QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação. Throughput host '
    'usa tempo real medido, não cartão físico. Leituras CPU usam 16 passagens. '
    'Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. '
    'Diferenças pequenas perante a dispersão não comprovam ganho; quartis e amostras estão no JSON.', '',
    'Upstream original usa mínimo multibloco quatro, atual dois. O controle anterior mostrou '
    'que esse limiar não explica a maior parte da diferença. Upstream não grava FAT2 neste workload '
    'e termina com espelhos diferentes; as garantias de persistência não são equivalentes.', '',
    '## Escritas de 257 bytes — clusters de 512 bytes', '',
    '| Ritmo | Perfil | Escritores | CPU A → B (ciclos/B) | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |',
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
    '| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |', '|---|---|---:|---|---:|']
for name, records in paired.items():
    for key, change in records.items():
        phase, _, cluster, chunk, files = key.split('/')
        if phase == 'write' and cluster == '1' and chunk == '257':
            continue
        lines.append(f'| {name} | {phase} | {int(cluster)*512} | {files}/{chunk} | {change:+.2f}% |')
lines += ['', '## I/O', '',
    'Os 192 pares de fases A/B preservaram exatamente leituras, escritas, metadados por FAT/diretório, '
    'polls, sessões e blocos multibloco nas duas latências. Portanto o throughput virtual também '
    'é igual. Conteúdo integral, FATs iguais e ordem EOC/vínculo/diretório foram auditados.', '',
    '| Latência | Escritores/bloco | R | W | Metadados |', '|---:|---|---:|---:|---:|']
for latency in (0, 1):
    for files, chunk in ((1, 257), (2, 257), (1, 4096), (2, 4096)):
        record = results['interleaved-after']['io'][f'write/{latency}/1/{chunk}/{files}']
        lines.append(f"| {latency} | {files}/{chunk} | {record['reads']} | {record['writes']} | {record['metadata_writes']} |")
lines += ['', '## Recursos e validação', '',
    '| ABI | RAM A → B (bytes) | Código A → B (bytes) |', '|---|---:|---:|']
for name, record in footprints.items():
    lines.append(f"| {name} | {record['before']['static_ram_bytes']} → {record['after']['static_ram_bytes']} | {record['before']['code_bytes']} → {record['after']['code_bytes']} |")
lines += ['', f'Matriz PIC24 de 48 configurações: RAM estática igual em todos os pares; pilha interna '
    f'máxima **{peak} bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.', '',
    'Passaram 28 testes de alocação/cache, incluindo nove novos cenários dirigidos do par coletado '
    'em cada perfil: timestamps e estouro de cacheTimer, bits altos/vizinhos, retainCount, '
    'FSInfo, reparo de espelho limpo, primária limpa, cada FAT em transferência, entrada ocupada, '
    'reserva de outro escritor e dados em transferência independentes. '
    'Os testes existentes cobrem volumes quase cheios, três escritores, fsync imediato, '
    'imutabilidade do buffer e cortes de energia/recuperação/retry:', '']
lines += [f'- {name}: {message}' for name, message in recovery.items()]
lines += ['', f'Evidências congeladas: `{root.as_posix()}`. '
    'As alternativas aritmética e combinada ficaram somente nos snapshots experimentais.']
output = Path('tests/SMALL_WRITE_PAIR_CPU.md')
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, repetitions=repetitions,
    results=results, paired_cpu_delta_percent=paired, generated_code=generated_code,
    matrix=matrix, resources=footprints, recovery=recovery), indent=2))
print(output)
