"""Prepare frozen reserve-flush A/B sources or publish validated CPU/I/O results."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess

from cache_index_cpu_report import measurements, resources
from io_cpu_relation_report import audit
from write_dispatch_cpu_report import COUNTERS


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(root):
    previous = root / 'before/lib'
    assert previous.is_dir(), 'Freeze the original library before editing production.'
    shutil.copytree('lib', root / 'normal/lib', dirs_exist_ok=True)
    shutil.copytree('tests/build/io-cpu-relation/upstream/lib', root / 'upstream/lib', dirs_exist_ok=True)
    for version, source in (('before', previous), ('after', root / 'normal/lib')):
        target = root / f'cpu-{version}-source/lib'
        shutil.copytree(source, target, dirs_exist_ok=True)
    shutil.copyfile('tests/upstream_comparison.c', root / 'harness.c')
    shutil.copyfile('tests/build/io-cpu-relation/portability.h', root / 'portability.h')
    # Control build timestamps identically, without altering production sources.
    for directory in (root / 'cpu-before-source/lib', root / 'cpu-after-source/lib', root / 'upstream/lib'):
        source = directory / 'asyncfatfs.c'
        content = source.read_text()
        content = content.replace('COMPUTE_BUILD_HOUR', '12').replace('COMPUTE_BUILD_MIN', '0').replace('COMPUTE_BUILD_SEC', '0')
        source.write_text(content)
    work = root / 'matrix-work/tests'
    shutil.copytree('tests/config', work / 'config', dirs_exist_ok=True)
    common = Path('tests/config_common.h').read_text().replace('AFATFS_NUM_CACHE_SECTORS 4', 'AFATFS_NUM_CACHE_SECTORS 8')
    (work / 'config_common.h').write_text(common)
    shutil.copytree('tests', root / 'functional/tests', ignore=shutil.ignore_patterns('build', '__pycache__'), dirs_exist_ok=True)
    shutil.copytree(root / 'normal/lib', root / 'functional/lib', dirs_exist_ok=True)
    (root / 'functional/tests/config_common.h').write_text(common)
    commit = subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip()
    upstream = subprocess.check_output(['git', 'show', 'upstream/master:lib/asyncfatfs.c'])
    assert upstream.decode().replace('\r\n', '\n').replace('COMPUTE_BUILD_HOUR', '12').replace('COMPUTE_BUILD_MIN', '0').replace('COMPUTE_BUILD_SEC', '0') == (root / 'upstream/lib/asyncfatfs.c').read_text()
    (root / 'manifest.json').write_text(json.dumps(dict(upstream=commit, sources={name: {item.name: digest(item) for item in (root / f'{name}/lib').iterdir() if item.is_file()} for name in ('before', 'normal', 'upstream')}), indent=2))


def publish(root, repetitions, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['sources']['normal']['asyncfatfs.c']
    before_source = (root / 'before/lib/asyncfatfs.c').read_text()
    old = '                        afatfs_flushCache();\n                        return AFATFS_OPERATION_IN_PROGRESS;'
    new = ('                        // Keep the old directory image buffered while both reservation FAT pages drain.\n'
           '                        afatfs_flushAllocationCache(file);\n                        return AFATFS_OPERATION_IN_PROGRESS;')
    assert before_source.count(old) == 1
    assert before_source.replace(old, new) == (root / 'normal/lib/asyncfatfs.c').read_text()
    for name, expected in manifest['sources']['before'].items():
        if name != 'asyncfatfs.c':
            assert manifest['sources']['normal'][name] == expected, name
    benchmark_sources = {'harness': digest(root / 'harness.c')}
    for version, original in (('before', 'before'), ('after', 'normal')):
        expected = (root / f'{original}/lib/asyncfatfs.c').read_text()
        expected = expected.replace('COMPUTE_BUILD_HOUR', '12').replace('COMPUTE_BUILD_MIN', '0').replace('COMPUTE_BUILD_SEC', '0')
        source = root / f'cpu-{version}-source/lib/asyncfatfs.c'
        assert source.read_text() == expected
        benchmark_sources[version] = digest(source)
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['sources']['normal']['asyncfatfs.c']
    assert matrix['sources']['before']['asyncfatfs.c'] == manifest['sources']['before']['asyncfatfs.c']
    peak = max(item['measurements']['after']['stack']['conservative_internal_bytes'] for item in matrix['results'])
    upstream_matrix = json.loads((root / 'upstream-matrix/report.json').read_text())
    assert upstream_matrix['passed'] and len(upstream_matrix['results']) == 2
    x86 = {version: resources(root / f'resources-x86-{version}') for version in ('before', 'after', 'upstream')}
    assert x86['before']['static_ram_bytes'] == x86['after']['static_ram_bytes']
    functional = {}
    cases = ('order_idle', 'three_files', 'pending_order', 'three_pending', 'near_full',
             'metadata_hints', 'mirror_cache', 'mirror_cache_multi', 'mirror_cache_query',
             'reservation_commit', 'pending_allocations')
    for name in cases:
        directory = root / f'functional/tests/build/fat32-allocation-checks/{name}'
        filename = 'results.csv' if name in cases[:5] else 'results.log'
        assert (directory / filename).stat().st_size > 0
        functional[name] = digest(directory / filename)
    for profile in ('default', 'multi', 'query'):
        path = root / f'functional/tests/build/reserve-wait-{profile}/results.log'
        assert 'Reserve wait: directory excluded, both FAT submission/ACK retries and remount passed.' in path.read_text()
        functional[f'reserve_wait_{profile}'] = digest(path)
    reader = root / 'functional/tests/build/reader-final/results.log'
    assert 'Reader cache:' in reader.read_text()
    functional['reader_cache'] = digest(reader)
    assert 'directoryEntry->state == AFATFS_CACHE_STATE_DIRTY' in (root / 'negative/error.log').read_text()
    for filename in ('multiread-checks.log', 'query-checks.log'):
        assert (root / filename).read_text().count(': passed.') == 11
        functional[filename] = digest(root / filename)
    assert (root / 'functional-checks.log').read_text().count('global idle and clean power-off status: passed') == 7
    functional['functional-checks.log'] = digest(root / 'functional-checks.log')
    result = dict(manifest=manifest, benchmark_sources=benchmark_sources, matrix=matrix, upstream_matrix=upstream_matrix, x86=x86,
                  functional=functional, experiments={})
    lines = ['# Flush filtrado na espera pela reserva', '',
             f"Upstream/master local: `{manifest['upstream']}`. FAT32 8.3, cache **8 setores**, clusters 512 bytes, arquivos de 1 MiB.", '',
             'A espera da reserva usa flushAllocationCache(file), evitando enviar a imagem antiga do diretório. As verificações IN_SYNC das duas FATs permanecem antes de consumir a reserva. A reserva única, EOC confirmado antes do vínculo e vínculo confirmado antes do diretório permanecem.', '',
             f'MSVC x64 /O2, {repetitions} repetições pareadas com ordem alternada, afinidade fixa e 32 passagens de leitura. Sem compilações concorrentes durante CPU. Hora FAT fixada em 12:00:00 somente nas cópias de benchmark. Ciclos/B incluem biblioteca, harness e backend simulado; não medem PIC24 ou cartão físico. O modo intercalado prepara o payload uma vez por pedido. Auditorias instrumentadas separadas, timings descartados. Redução positiva significa menos CPU; pequenas diferenças podem ser ruído.', '',
             f'PIC24: 48 configurações/perfis/ABIs, pilha interna máxima **{peak} bytes**, incluindo chamadas internas e PCs de retorno, excluindo driver/aplicação/libc. RAM estática igual antes/depois em todas. Recursos x64 abaixo contam somente os objetos da biblioteca.', '']
    for schedule in ('serial', 'prepared'):
        for profile in ('default', 'multi', 'query'):
            group = f'{schedule}-{profile}'
            data = {version: measurements(root / f'cpu-{group}-{version}', repetitions) for version in ('before', 'after', 'upstream')}
            traced = {version: audit(root / f'audit-{group}-{version}') for version in data}
            memory = {version: resources(root / f'cpu-{group}-{version}') for version in data}
            assert memory['before']['static_ram_bytes'] == memory['after']['static_ram_bytes']
            for version, rows in data.items():
                for key, row in rows.items():
                    assert all(row[field] == traced[version][key][field] for field in COUNTERS), (group, version, key)
                    assert row['metadata_writes'] == sum(traced[version][key][field] for field in ('primary_writes', 'mirror_writes', 'directory_writes'))
                    if key.startswith('write/'):
                        assert row['writes'] - row['metadata_writes'] == row['payload_bytes'] // 512
                        assert traced[version][key]['generated_bytes'] == row['payload_bytes']
            result['experiments'][group] = dict(cpu=data, audit=traced, resources=memory)
            lines += [f'## {group}', '',
                      '| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |',
                      '|---|---:|---:|---:|---:|---:|---:|']
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = f'write/0/1/{chunk}/{files}'
                    before, after, upstream = (data[version][key] for version in ('before', 'after', 'upstream'))
                    reduction = statistics.median((1 - new / old) * 100 for old, new in zip(before['samples'], after['samples']))
                    extra = (after['cycles_per_byte'] / upstream['cycles_per_byte'] - 1) * 100
                    lines.append(f"| {chunk}/{files} | {before['cycles_per_byte']:.4f} → {after['cycles_per_byte']:.4f} | {reduction:+.2f}% | {upstream['cycles_per_byte']:.4f} | {extra:+.2f}% | {before['metadata_writes']} → {after['metadata_writes']} / {upstream['metadata_writes']} | {after['reads']}/{after['writes']} / {upstream['reads']}/{upstream['writes']} |")
            lines += ['', f'Recursos: `{json.dumps(memory, ensure_ascii=False)}`.', '',
                      '| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |', '|---|---:|---:|---:|---:|']
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = f'write/0/1/{chunk}/{files}'
                    old, new = traced['before'][key], traced['after'][key]
                    lines.append(f"| {chunk}/{files} | {old['primary_writes']} → {new['primary_writes']} | {old['mirror_writes']} → {new['mirror_writes']} | {old['directory_writes']} → {new['directory_writes']} | {old['multi_sessions']}/{old['multi_blocks']} → {new['multi_sessions']}/{new['multi_blocks']} |")
            lines += ['', 'Controle de leitura (redução pareada de CPU): ' + ', '.join(f"{chunk}/{files}: {statistics.median((1-new/old)*100 for old,new in zip(data['before'][f'read/0/1/{chunk}/{files}']['samples'],data['after'][f'read/0/1/{chunk}/{files}']['samples'])):+.2f}%" for chunk in (257,4096) for files in (1,2)) + '.', '']
    principal = result['experiments']['prepared-default']
    principal_key = 'write/0/1/4096/2'
    old, new, upstream = (principal['cpu'][version][principal_key] for version in ('before', 'after', 'upstream'))
    paired = statistics.median((1 - after / before) * 100 for before, after in zip(old['samples'], new['samples']))
    serial_cost = max(-statistics.median((1 - after / before) * 100 for before, after in zip(experiment['cpu']['before'][key]['samples'], row['samples']))
                      for name, experiment in result['experiments'].items() if name.startswith('serial-')
                      for key, row in experiment['cpu']['after'].items() if key.startswith('write/'))
    summary = ['## Resultado principal e custo observado', '',
               f"Padrão, dois escritores/4096 intercalados: CPU reduzida **{paired:.2f}%**, ainda **{(new['cycles_per_byte']/upstream['cycles_per_byte']-1)*100:.2f}%** acima do upstream. Metadados 8547 → 8259 (−3,37%); escritas totais 12643 → 12355 (−2,28%); diretório 291 → 3; FAT1/FAT2 permanecem em 4128 cada. Sequências multibloco preservadas: 288 sessões/576 setores.", '',
               f"O ganho de CPU não é geral: nos casos seriais o I/O permanece igual e o maior aumento pareado de CPU observado foi **{serial_cost:.2f}%**, no perfil opcional com consulta. As tabelas preservam todos os resultados, incluindo aumentos. A alteração resolve o flush evitável do diretório; não resolve o custo dominante de publicação da FAT por cluster.", '']
    lines[10:10] = summary
    lines += ['## Recursos adicionais', '', f'Recursos x86: `{json.dumps(x86)}`.', '',
              '| PIC24 padrão | Atual antes → depois (RAM/pilha B) | Upstream (RAM/pilha B) |', '|---|---:|---:|']
    for mode in ('normal', 'short'):
        actual = next(item['measurements'] for item in matrix['results'] if item['configuration'] == 'default' and item['profile'] == 'single' and item['enums'] == mode)
        upstream = next(item['measurements']['after'] for item in upstream_matrix['results'] if item['enums'] == mode)
        old, new = actual['before'], actual['after']
        lines.append(f"| {mode} | {old['static_bytes']}/{old['stack']['conservative_internal_bytes']} → {new['static_bytes']}/{new['stack']['conservative_internal_bytes']} | {upstream['static_bytes']}/{upstream['stack']['conservative_internal_bytes']} |")
    lines += ['', '## Persistência e limites', '',
              'Passaram 15 testes de alocação/cache, sete suítes funcionais padrão/opcionais e 11 suítes em cada perfil de leitura multibloco (com/sem consulta). Cobrem reserva/agrupamento com ACKs falhos e retries, espelho, quase cheio, três escritores e ordenação por gravação. O teste específico reserve_wait mantém um diretório sujo enquanto falham submissões/ACKs da primária ou do espelho, confirmando que a reserva não é consumida e o diretório não é enviado. O controle negativo com a biblioteca anterior falha na asserção que exige manter o diretório sujo. Não equivale a uma campanha física de corte de energia.', '',
              'O upstream deste workload não atualiza a segunda FAT. As contagens são setores transferidos, não programas NAND. A mudança reduz flushes evitáveis de diretório; a amplificação dominante da FAT permanece. Evidências: tests/build/reserve-flush.']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/reserve-flush'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, default=Path('tests/RESERVE_FLUSH.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    else:
        publish(arguments.directory, arguments.repetitions, arguments.output)
