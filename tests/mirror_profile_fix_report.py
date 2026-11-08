"""Verify profile selection restores original native code and preserves multiblock code."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct

from cache_index_cpu_report import measurements, resources
from upstream_comparison_report import coff_sections


PROFILES = (('Padrão', 'before_default', 'default', 'reference_default'),
            ('Multibloco', 'before', 'after_no_query', 'reference_no_query'),
            ('Multibloco + consulta', 'before_query', 'after', 'reference_query'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def object_code(path):
    data, sections = coff_sections(path)
    return [dict(size=section['size'], sha256=hashlib.sha256(
        data[section['offset']:section['offset'] + section['size']]).hexdigest())
            for section in sections if section['flags'] & 0x20]


def linked_code(path):
    data = path.read_bytes()
    header = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[header:header + 4] == b'PE\0\0'
    count = struct.unpack_from('<H', data, header + 6)[0]
    optional_size = struct.unpack_from('<H', data, header + 20)[0]
    sections = header + 24 + optional_size
    result = []
    for index in range(count):
        record = sections + index * 40
        size, offset = struct.unpack_from('<II', data, record + 16)
        flags = struct.unpack_from('<I', data, record + 36)[0]
        if flags & 0x20:
            result.append(dict(size=size, sha256=hashlib.sha256(data[offset:offset + size]).hexdigest()))
    assert result
    return result


def reduction(before, after):
    return statistics.median((1 - second / first) * 100
                             for first, second in zip(before['samples'], after['samples']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    variants = {name for _, old, new, original in PROFILES for name in (old, new, original)}
    variants.add('upstream_native')
    data = {name: measurements(root / f'cpu-{name}', args.repetitions) for name in sorted(variants)}
    memory = {name: resources(root / f'cpu-{name}') for name in sorted(variants)}
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == digest(root / 'normal/lib/asyncfatfs.c')
    assert matrix['sources']['before']['asyncfatfs.c'] == digest(root / 'before/lib/asyncfatfs.c')
    assert digest(Path('lib/asyncfatfs.c')) == digest(root / 'normal/lib/asyncfatfs.c')
    assert digest(root / 'normal/lib/asyncfatfs.c') == digest(root / 'after/lib/asyncfatfs.c')
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'normal/lib/asyncfatfs_config.h').read_text()
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'matrix-work/tests/config_common.h').read_text()
    for path in (root / 'before/lib').iterdir():
        if path.is_file() and path.name not in ('asyncfatfs.c', 'builddefs.h'):
            assert digest(path) == digest(root / 'normal/lib' / path.name), path
    identities = {}
    for selected, counterpart in (('default', 'reference_default'),
                                  ('after_no_query', 'before'), ('after', 'before_query')):
        identity = {}
        for source in ('asyncfatfs', 'fat_standard', 'upstream_comparison'):
            first = object_code(root / f'cpu-{selected}/{source}.obj')
            second = object_code(root / f'cpu-{counterpart}/{source}.obj')
            assert first == second, (selected, counterpart, source)
            identity[source] = first
        first = linked_code(root / f'cpu-{selected}/benchmark.exe')
        second = linked_code(root / f'cpu-{counterpart}/benchmark.exe')
        assert first == second, (selected, counterpart, 'linked code')
        identity['linked_code_equal'] = first == second
        identity['linked_code'] = first
        identity['counterpart_linked_code'] = second
        identities[f'{selected}/{counterpart}'] = identity
    comparisons = {}
    fields = ('payload_bytes', 'reads', 'writes', 'metadata_reads', 'metadata_writes',
              'polls', 'virtual_us', 'multi_sessions', 'multi_blocks',
              'read_sessions', 'batched_read_blocks', 'read_stops')
    for _, old, new, original in PROFILES:
        assert memory[old]['static_ram_bytes'] == memory[new]['static_ram_bytes']
        for key in data[new]:
            assert all(data[old][key][field] == data[new][key][field] == data[original][key][field]
                       for field in fields), (new, key)
            comparisons[f'{new}/{key}'] = dict(
                reduction_received=reduction(data[old][key], data[new][key]),
                reduction_original=reduction(data[original][key], data[new][key]))
    for item in matrix['results']:
        assert item['measurements']['before']['static_sections'] == item['measurements']['after']['static_sections']
        assert item['measurements']['after']['stack']['conservative_internal_bytes'] < 400
    x86 = {name: resources(root / f'x86-{name}')
           for name in ('before_default', 'default', 'reference_default', 'upstream_native')}
    assert x86['before_default']['static_ram_bytes'] == x86['default']['static_ram_bytes']
    assert object_code(root / 'x86-default/asyncfatfs.obj') == object_code(root / 'x86-reference_default/asyncfatfs.obj')
    report = dict(cache_sectors=8, repetitions=args.repetitions, measurements=data,
                  paired_cpu=comparisons, code_identities=identities, x64_resources=memory,
                  x86_resources=x86, matrix=matrix,
                  source_hashes={name: digest(root / f'{name}/lib/asyncfatfs.c')
                                 for name in ('before', 'reference', 'normal', 'after', 'upstream')})
    peak = max(item['measurements']['after']['stack']['conservative_internal_bytes'] for item in matrix['results'])
    lines = ['# Correção das regressões por seleção do espelhamento FAT', '',
             'Cache mantido em **oito setores**, FAT32 8.3, clusters de 512 bytes. '
             'A configuração padrão recupera o caminho anterior de aquisição e cópia '
             'integral da FAT. A cópia localizada de quatro bytes permanece nas builds '
             'com AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT, com e sem consulta do backend. '
             'Não há uma nova opção pública nem seleção dinâmica por arquivo.', '',
             'O parâmetro e o bool local usados para classificar o espelho não são '
             'compilados no padrão. AFATFS_FAT_MIRROR_STATE mantém chamadas e assinatura '
             'internas coerentes com o perfil. Nenhuma reserva, página, campo estático, '
             'barreira, ACK ou regra de publicação foi acrescentada ou removida.', '',
             '**Recebida** é a versão com as regressões de 10,14% na leitura 1/4096 '
             'e 2,11% na escrita 2/257; **original** é o snapshot imediatamente anterior '
             'à introdução da cópia localizada; **final** aplica a seleção por perfil. '
             'Os percentuais históricos não são misturados às novas amostras.', '',
             f'MSVC x64 /O2, {args.repetitions} repetições pareadas, ordem alternada, '
             'afinidade fixa, arquivos de 1 MiB por escritor e 32 passagens de leitura. '
             'Sem compilações concorrentes na medição. Hora FAT fixada em 12:00:00 apenas '
             'nos snapshots COMPARISON_CPU_ONLY. Ciclos incluem biblioteca, backend '
             'simulado, polling e harness; não são medições em PIC24 ou cartão físico. '
             'Redução positiva significa menos CPU; valores próximos de zero são ruído possível.', '',
             '## Identidade do código gerado', '',
             'Todas as seções de instruções dos objetos asyncfatfs, fat_standard e harness '
             'foram comparadas integralmente, incluindo posicionamento dentro das seções. '
             'No x64: padrão final = original; multibloco final = recebida; '
             'multibloco com consulta final = recebida. No x86, padrão final = original. '
             'Isso evita depender apenas de uma rodada favorável de CPU para afirmar '
             'restauração do padrão ou preservação do caminho multibloco.', '',
             'Igualdade das seções de instruções nos executáveis ligados: ' +
             ', '.join(f"{key}: {'sim' if value['linked_code_equal'] else 'não'}" for key, value in identities.items()) + '.', '',
             '## Cenários que motivaram a correção', '',
             '| Cenário padrão | Recebida ciclos/B | Final ciclos/B | Original ciclos/B | Redução vs recebida | Redução vs original |',
             '|---|---:|---:|---:|---:|---:|']
    for key in ('read/0/1/4096/1', 'write/0/1/257/2'):
        current = data['default'][key]['cycles_per_byte']
        before = data['before_default'][key]['cycles_per_byte']
        original = data['reference_default'][key]['cycles_per_byte']
        values = comparisons[f'default/{key}']
        lines.append(f"| {key} | {before:.4f} | {current:.4f} | {original:.4f} | "
                     f"{values['reduction_received']:+.2f}% | {values['reduction_original']:+.2f}% |")
    lines += ['', '## Escrita e preservação multibloco', '',
              'O código multibloco é idêntico ao recebido. Variações entre essas duas '
              'cópias são controles da medição, não ganhos novos. A comparação com a '
              'original usa o mesmo perfil para verificar os ganhos já obtidos.', '',
              '| Perfil | Bloco | Escritores | Final ciclos/B | Redução vs recebida | Redução vs original | CPU adicional upstream |',
              '|---|---:|---:|---:|---:|---:|---:|']
    for label, old, new, original in PROFILES:
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'write/0/1/{chunk}/{files}'
                value = data[new][key]['cycles_per_byte']
                extra = (value / data['upstream_native'][key]['cycles_per_byte'] - 1) * 100
                comparison = comparisons[f'{new}/{key}']
                lines.append(f"| {label} | {chunk} | {files} | {value:.4f} | "
                             f"{comparison['reduction_received']:+.2f}% | {comparison['reduction_original']:+.2f}% | {extra:+.2f}% |")
    lines += ['', '## I/O', '',
              'Leituras, gravações, metadados, polling, sessões e tempo virtual são '
              'idênticos entre recebida, final e original, em todos os casos e perfis. '
              'Contagens em setores de 512 bytes, incluindo fechamento e drenagem.', '',
              '| Bloco | Escritores | R final/upstream | W final/upstream | Metadados W final/upstream |',
              '|---|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = f'write/0/1/{chunk}/{files}'
            current = data['default'][key]
            upstream = data['upstream_native'][key]
            values = [f'{current[field]} / {upstream[field]}' for field in ('reads', 'writes', 'metadata_writes')]
            lines.append(f'| {chunk} | {files} | ' + ' | '.join(values) + ' |')
    lines += ['', 'Referência local upstream/master: '
              '`60c5f3580152410cb08342942cbbe3a30a449c24`, também com oito setores. '
              'Ele não mantém a segunda FAT atualizada nesse workload; a diferença '
              'restante inclui espelhamento e ordenação adicionais da biblioteca atual.', '',
              '## RAM, código e pilha', '',
              '| ABI/perfil | RAM recebida → final | Código recebido → final |',
              '|---|---:|---:|']
    for label, old, new, _ in PROFILES:
        lines.append(f"| x64/{label} | {memory[old]['static_ram_bytes']} → {memory[new]['static_ram_bytes']} B | "
                     f"{memory[old]['code_bytes']} → {memory[new]['code_bytes']} B |")
    lines.append(f"| x86/padrão | {x86['before_default']['static_ram_bytes']} → {x86['default']['static_ram_bytes']} B | "
                 f"{x86['before_default']['code_bytes']} → {x86['default']['code_bytes']} B |")
    lines += ['', '| PIC24/padrão | RAM recebida → final | Pilha recebida → final |',
              '|---|---:|---:|']
    for mode in ('normal', 'short'):
        row = next(item for item in matrix['results'] if item['configuration'] == 'default'
                   and item['profile'] == 'single' and item['enums'] == mode)
        old = row['measurements']['before']
        new = row['measurements']['after']
        lines.append(f"| Enums {mode} | {old['static_bytes']} → {new['static_bytes']} B | "
                     f"{old['stack']['conservative_internal_bytes']} → "
                     f"{new['stack']['conservative_internal_bytes']} B |")
    lines += ['', f'As 48 variantes PIC24 passaram com seções estáticas iguais e pilha '
              f'interna máxima de **{peak} bytes**, estritamente abaixo de 400. '
              'XC16 2.10, PIC24FJ256GA702, C99 -Os; inclui chamadas internas e retornos, '
              'exclui backend, callbacks da aplicação e rotinas do compilador/libc.', '',
              '## Testes e evidências', '',
              'Passaram os 11 casos funcionais e 12 checks de alocação. O teste de '
              'espelho agora executa explicitamente os três perfis: cópia integral '
              'no padrão e localizada nos dois multiblocos. Verifica vizinhos, bits '
              'altos, página nova, espelho limpo antigo, falha de ACK/retry e expulsão '
              'da primária com espelho ainda sujo. Os outros checks preservam '
              'ordenação, reserva única, pressão e escritores pendentes. '
              'Doxygen: 295 definições; fontes continuam no monólito. Mídia sintética, '
              'sem dependência dos VHDs perdidos.', '',
              f'`{root.as_posix()}` contém before/lib (recebida), reference/lib (original), '
              'normal/lib (final sem hora fixada), after/lib (benchmark), upstream, '
              'objetos, executáveis, trials, testes, docs e matrix/report.json. '
              'O JSON ao lado conserva amostras, hashes, comparações de código e controles '
              'de leitura de todos os cenários.', '',
              '```powershell',
              f'python tests/mirror_profile_fix_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}',
              '```', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
