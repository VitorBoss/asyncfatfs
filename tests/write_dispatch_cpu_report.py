"""Validate the eight-sector writer continuation fast path and preserved multiblock code."""
import argparse
import json
from pathlib import Path

from cache_index_cpu_report import measurements, resources
from mirror_profile_fix_report import digest, linked_code, object_code, reduction


PROFILES = (('Padrão', 'before_default', 'default'),
            ('Multibloco', 'before', 'after_no_query'),
            ('Multibloco + consulta', 'before_query', 'after'))
COUNTERS = ('payload_bytes', 'reads', 'writes', 'metadata_reads', 'metadata_writes',
            'polls', 'virtual_us', 'multi_sessions', 'multi_blocks',
            'read_sessions', 'batched_read_blocks', 'read_stops')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    matrix = json.loads((root / 'matrix/report.json').read_text())
    upstream_matrix = json.loads((root / 'upstream-matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert upstream_matrix['passed'] and len(upstream_matrix['results']) == 2
    assert matrix['sources']['after']['asyncfatfs.c'] == digest(root / 'normal/lib/asyncfatfs.c')
    assert matrix['sources']['before']['asyncfatfs.c'] == digest(root / 'before/lib/asyncfatfs.c')
    assert upstream_matrix['sources']['after']['asyncfatfs.c'] == digest(root / 'upstream/lib/asyncfatfs.c')
    assert digest(Path('lib/asyncfatfs.c')) == digest(root / 'normal/lib/asyncfatfs.c')
    assert digest(root / 'normal/lib/asyncfatfs.c') == digest(root / 'after/lib/asyncfatfs.c')
    for path in (root / 'before/lib').iterdir():
        if path.is_file() and path.name not in ('asyncfatfs.c', 'builddefs.h'):
            assert digest(path) == digest(root / 'normal/lib' / path.name), path
    for path in (root / 'normal/lib/asyncfatfs_config.h', root / 'matrix-work/tests/config_common.h',
                 root / 'upstream/lib/asyncfatfs.c'):
        assert '#define AFATFS_NUM_CACHE_SECTORS 8' in path.read_text()
    for item in matrix['results']:
        old = item['measurements']['before']
        new = item['measurements']['after']
        assert old['static_sections'] == new['static_sections']
        assert new['stack']['conservative_internal_bytes'] < 400
    identities = {}
    for _, old, new in PROFILES[1:]:
        evidence = {}
        for source in ('asyncfatfs', 'fat_standard', 'upstream_comparison'):
            first = object_code(root / f'cpu-{old}/{source}.obj')
            second = object_code(root / f'cpu-{new}/{source}.obj')
            assert first == second, (old, new, source)
            evidence[source] = first
        first = linked_code(root / f'cpu-{old}/benchmark.exe')
        assert first == linked_code(root / f'cpu-{new}/benchmark.exe')
        evidence['executable'] = first
        identities[new] = evidence
    report = dict(repetitions=args.repetitions, cache_sectors=8, matrix=matrix,
                  upstream_matrix=upstream_matrix, multiblock_code=identities,
                  experiments={}, resources={}, discarded={},
                  sources={name: digest(root / f'{name}/lib/asyncfatfs.c')
                           for name in ('before', 'normal', 'after', 'upstream')})
    lines = ['# CPU de escrita: continuação direta da alocação no escalonador', '',
             'FAT32 8.3, clusters de 512 bytes, **oito setores de cache**, API original. '
             'Referência local upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24` '
             '(blob asyncfatfs.c `81a8b854a345e329e0c289798ff6d82ef31dcf4f`, conferido).', '',
             'No pipeline nativo com cache de pelo menos oito setores e sem leitura '
             'multibloco, fileOperationsPoll chama appendRegularFreeClusterContinue '
             'diretamente para APPEND_FREE_CLUSTER com tipo válido. Evita o despacho '
             'genérico de opcode nessa operação frequente. Mantém a checagem inicial '
             'de operação ativa, a validação de tipo, o dispatcher para os demais '
             'casos, a ordem dos arquivos e a quantidade de passagens por poll.', '',
             'A continuação é exatamente a mesma; não foram alteradas fases, ACKs, '
             'reserva única, duas FATs, publicação do diretório, fechamento ou backend. '
             'As builds x64 multibloco, com/sem consulta, preservam **todas as instruções '
             'dos objetos da biblioteca/harness e dos executáveis ligados**, verificadas '
             'por hashes. Variações de timing nesses pares idênticos não são ganhos novos.', '',
             f'MSVC x64 /O2, {args.repetitions} repetições pareadas, ordem alternada, '
             'afinidade fixa e arquivos de 1 MiB por escritor. 32 passagens de leitura '
             'conferem os dados. Medições sem compilações concorrentes. Hora FAT fixada '
             'em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. Ciclos incluem '
             'biblioteca, backend simulado, polling e harness; não são medições em '
             'PIC24 ou cartão físico. A redução é a mediana das razões pareadas; '
             'CPU adicional ao upstream usa a razão das medianas. Redução positiva significa menos CPU; valores '
             'pequenos podem refletir ruído do host.', '']
    for schedule, directory, profiles in (('Por bloco', root, PROFILES),
                                          ('Pedidos intercalados', root / 'interleaved', PROFILES[:1])):
        variants = {name for _, old, new in profiles for name in (old, new)} | {'upstream_native'}
        data = {name: measurements(directory / f'cpu-{name}', args.repetitions) for name in sorted(variants)}
        changes = {}
        lines += [f'## {schedule}', '',
                  ('Cada pedido de um escritor termina antes de tentar o próximo.' if directory == root else
                   'COMPARISON_INTERLEAVED_REQUESTS tenta atender ambos os escritores antes de aguardar progresso.'), '',
                  '| Perfil | Bloco | Escritores | Antes ciclos/B | Depois ciclos/B | Redução pareada | Upstream ciclos/B | CPU adicional upstream antes → depois |',
                  '|---|---:|---:|---:|---:|---:|---:|---:|']
        for label, old, new in profiles:
            for key in data[new]:
                assert all(data[old][key][field] == data[new][key][field] for field in COUNTERS), (schedule, new, key)
                changes[f'{new}/{key}'] = reduction(data[old][key], data[new][key])
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = f'write/0/1/{chunk}/{files}'
                    before = data[old][key]['cycles_per_byte']
                    after = data[new][key]['cycles_per_byte']
                    upstream = data['upstream_native'][key]['cycles_per_byte']
                    lines.append(f'| {label} | {chunk} | {files} | {before:.4f} | {after:.4f} | '
                                 f"{changes[f'{new}/{key}']:+.2f}% | {upstream:.4f} | "
                                 f"{(before / upstream - 1) * 100:+.2f}% → {(after / upstream - 1) * 100:+.2f}% |")
        lines += ['', 'I/O em setores de 512 bytes, incluindo fechamento e drenagem. '
                  '**Antes/depois idênticos** em leituras, gravações, metadados, polling, '
                  'sessões e tempo virtual para cada perfil/caso. Comparação padrão/upstream:', '',
                  '| Bloco | Escritores | R atual/upstream | W atual/upstream | Metadados W atual/upstream |',
                  '|---|---:|---:|---:|---:|']
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'write/0/1/{chunk}/{files}'
                current = data['default'][key]
                upstream = data['upstream_native'][key]
                values = [f'{current[field]} / {upstream[field]}' for field in ('reads', 'writes', 'metadata_writes')]
                lines.append(f'| {chunk} | {files} | ' + ' | '.join(values) + ' |')
        lines += ['', 'Controles de leitura padrão, redução pareada frente ao baseline:', '',
                  '| Bloco | Leitores | Redução de CPU |', '|---|---:|---:|']
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'default/read/0/1/{chunk}/{files}'
                lines.append(f'| {chunk} | {files} | {changes[key]:+.2f}% |')
        report['experiments'][schedule] = dict(data=data, paired_cpu_reduction_percent=changes)
    lines += ['', 'Upstream não atualiza a segunda FAT nesse workload. A diferença '
              'restante inclui as garantias adicionais de espelhamento e ordenação; '
              'a otimização do dispatcher não reduz essa quantidade de trabalho.', '',
              '## RAM, código e pilha', '',
              'RAM estática da biblioteca, incluindo buffers e alinhamento; exclui '
              'backend e aplicação. Código contado nos objetos da biblioteca.', '',
              '| ABI/perfil | RAM antes → depois / upstream | Código antes → depois / upstream |',
              '|---|---:|---:|']
    for abi, prefix, profiles in (('x64', 'cpu', PROFILES), ('x86', 'x86', PROFILES[:1])):
        upstream = resources(root / f'{prefix}-upstream_native')
        for label, old, new in profiles:
            before = resources(root / f'{prefix}-{old}')
            after = resources(root / f'{prefix}-{new}')
            assert before['static_ram_bytes'] == after['static_ram_bytes']
            report['resources'][f'{abi}/{new}'] = dict(before=before, after=after, upstream=upstream)
            lines.append(f"| {abi}/{label} | {before['static_ram_bytes']} → {after['static_ram_bytes']} / "
                         f"{upstream['static_ram_bytes']} B | {before['code_bytes']} → {after['code_bytes']} / "
                         f"{upstream['code_bytes']} B |")
    lines += ['', '| PIC24 padrão | RAM antes → depois / upstream | Pilha antes → depois / upstream |',
              '|---|---:|---:|']
    for mode in ('normal', 'short'):
        row = next(item for item in matrix['results'] if item['configuration'] == 'default'
                   and item['profile'] == 'single' and item['enums'] == mode)
        up_row = next(item for item in upstream_matrix['results'] if item['enums'] == mode)
        before = row['measurements']['before']
        after = row['measurements']['after']
        upstream = up_row['measurements']['after']
        stack = [item['stack']['conservative_internal_bytes'] for item in (before, after, upstream)]
        lines.append(f"| Enums {mode} | {before['static_bytes']} → {after['static_bytes']} / "
                     f"{upstream['static_bytes']} B | {stack[0]} → {stack[1]} / {stack[2]} B |")
    peak = max(item['measurements']['after']['stack']['conservative_internal_bytes'] for item in matrix['results'])
    lines += ['', f'As **48 variantes PIC24** passaram com seções estáticas iguais e '
              f'pilha interna máxima de **{peak} bytes**, abaixo de 400. XC16 2.10, '
              'PIC24FJ256GA702, C99 -Os, enums normais/curtos. Inclui chamadas internas '
              'e retornos; exclui backend, callbacks da aplicação e rotinas do compilador/libc.', '',
              '## Testes e tentativas descartadas', '',
              'Passaram 11 cenários funcionais e 12 checks de alocação, cobrindo '
              'ordenação, pressão, três escritores, pedidos sobrepostos, espaço '
              'esgotado, reserva, espelhamento nos três perfis e falhas de ACK/retry. '
              'As auditorias do benchmark confirmam ambas as FATs atuais. Doxygen '
              'continua com 295 definições; nenhuma fragmentação em .inc foi introduzida. '
              'Os testes usam mídia sintética, sem depender dos VHDs perdidos.', '',
              'Dois filtros de seleção de páginas foram descartados. Reduziam CPU '
              'de escrita, mas regrediam a leitura padrão 1/4096. Não fazem parte '
              'da fonte mantida e suas amostras não foram combinadas às finais.', '']
    for name in ('cache8-flush-filter', 'cache8-flush-eligibility'):
        trial_root = root.parent / name
        old = measurements(trial_root / 'cpu-before_default', args.repetitions)
        new = measurements(trial_root / 'cpu-default', args.repetitions)
        changes = {key: reduction(old[key], new[key]) for key in old}
        report['discarded'][name] = dict(before=old, after=new, paired_cpu_reduction_percent=changes,
                                        source_sha256=digest(trial_root / 'normal/lib/asyncfatfs.c'))
        lines.append(f"- `{name}`: leitura 1/4096 {changes['read/0/1/4096/1']:+.2f}% de redução (negativo = mais CPU).")
    lines += ['', '## Evidências', '',
              f'`{root.as_posix()}` conserva before/lib, normal/lib (fonte final sem '
              'hora fixada), after/lib (benchmark), upstream canônico, objetos, '
              'executáveis, trials, scripts derivados, testes e matrizes. O JSON '
              'ao lado registra amostras completas, contadores, hashes e tentativas '
              'descartadas. lib/sdcard.h não foi editado.', '',
              '```powershell',
              f'python tests/write_dispatch_cpu_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}', '```', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
