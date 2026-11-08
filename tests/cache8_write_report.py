"""Validate and summarize the eight-sector write CPU comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from cache_index_cpu_report import measurements, resources


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    manifest = json.loads((root / 'manifest.json').read_text())
    assert manifest['cache_sectors'] == 8
    for source in (root / 'normal/lib/asyncfatfs_config.h', root / 'upstream/lib/asyncfatfs.c',
                   root / 'matrix-work/tests/config_common.h'):
        assert re.search(r'^#define AFATFS_NUM_CACHE_SECTORS 8\s*$', source.read_text(), re.M), source
    variants = ('default', 'after_no_query', 'after', 'upstream_native')
    data = {name: measurements(root / f'cpu-{name}', args.repetitions) for name in variants}
    memory = {abi: {name: resources(root / f'{prefix}-{name}') for name in variants}
              for abi, prefix in (('x64', 'cpu'), ('x86', 'x86'))}
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    upstream_pic = json.loads((root / 'upstream-pic24.json').read_text())
    peak = max(row['measurements']['after']['stack']['conservative_internal_bytes']
               for row in matrix['results'])
    assert peak < 400
    for name, digest in manifest['source_hashes'].items():
        assert hashlib.sha256((root / 'normal/lib' / name).read_bytes()).hexdigest() == digest
    lines = ['# Escrita com oito setores de cache', '',
             'Rodada de 18/11/2026. FAT32 8.3, clusters de 512 bytes, oito setores de cache '
             'nas duas bibliotecas, três handles e escrita multibloco habilitada. '
             f"Upstream/master local: `{manifest['upstream_commit']}`.", '',
             'API original, LFN/exFAT/journal desligados no benchmark. Imagem sintética '
             'MBR com 65.530 clusters, sem FSInfo ou boot de backup. Dois escritores '
             'são intercalados na mesma thread; o harness termina cada bloco antes '
             'de tentar o próximo escritor.', '',
             f'MSVC x64 /O2, {args.repetitions} repetições intercaladas em ordem alternada, '
             'afinidade fixa, arquivos de 1 MiB por escritor, sem compilações concorrentes. '
             'São ciclos do host por byte de payload agregado, incluindo backend simulado, '
             'polling, abertura/fechamento e preparação dos dados. A auditoria completa '
             'do volume ocorre fora da janela de escrita. Não são ciclos isolados da biblioteca '
             'nem medições em cartão físico/PIC24.', '',
             'A hora FAT foi fixada em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. '
             'As fontes normais da biblioteca não foram modificadas. O benchmark também '
             'faz 32 passagens de leitura para conferir conteúdo após a escrita.', '',
             '## CPU de escrita', '',
             'A coluna CPU adicional compara a razão das medianas com upstream. '
             'Valores positivos indicam maior consumo. Os perfis multibloco abaixo '
             'habilitam a leitura multibloco opcional; a escrita multibloco já está ligada '
             'em todos os perfis, inclusive no padrão e no upstream. As opções de leitura '
             'também selecionam especializações existentes no caminho de escrita.', '',
             '| Perfil atual | Bloco | Escritores | Atual, ciclos/byte | Upstream, ciclos/byte | CPU adicional |',
             '|---|---:|---:|---:|---:|---:|']
    labels = {'default': 'Padrão', 'after_no_query': 'Leitura multibloco',
              'after': 'Leitura multibloco + consulta'}
    for variant, label in labels.items():
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'write/0/1/{chunk}/{files}'
                current = data[variant][key]['cycles_per_byte']
                upstream = data['upstream_native'][key]['cycles_per_byte']
                extra = (current / upstream - 1) * 100
                lines.append(f'| {label} | {chunk} | {files} | {current:.4f} | '
                             f'{upstream:.4f} | +{extra:.2f}% |')
    lines += ['', '## I/O durante a escrita', '',
              'R/W contam setores de 512 bytes e incluem metadados e drenagem do fechamento. '
              'As contagens são determinísticas nas repetições. Os três perfis atuais '
              'têm o mesmo I/O de escrita neste ensaio.', '',
              '| Bloco | Escritores | R atual / upstream | W atual / upstream | Metadados W atual / upstream | Polls atual / upstream |',
              '|---|---:|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = f'write/0/1/{chunk}/{files}'
            current = data['default'][key]
            upstream = data['upstream_native'][key]
            fields = ('reads', 'writes', 'metadata_reads', 'metadata_writes', 'polls', 'virtual_us')
            for variant in labels:
                assert all(data[variant][key][field] == current[field] for field in fields)
            values = [f"{current[field]} / {upstream[field]}"
                      for field in ('reads', 'writes', 'metadata_writes', 'polls')]
            lines.append(f'| {chunk} | {files} | ' + ' | '.join(values) + ' |')
    lines += ['', 'A auditoria das duas FATs passou em todas as execuções atuais. '
              'O upstream deixa diferenças entre as cópias nesse workload. Portanto, '
              'o ensaio não compara implementações com garantias de espelhamento iguais. '
              'Os contadores não isolam quanto da diferença vem de espelhamento, '
              'ordenação de persistência ou política de cache; não atribuir toda a '
              'diferença de CPU apenas à segunda FAT.', '', '## RAM estática da biblioteca', '',
              'Soma das seções graváveis de asyncfatfs e fat_standard, incluindo cache '
              'e alinhamento; exclui backend, mídia em RAM e buffers do aplicativo/harness. '
              'Não é somente sizeof do estado global. Os três perfis atuais mediram a mesma RAM.', '',
              '| ABI | Atual | Upstream | Diferença |', '|---|---:|---:|---:|']
    for abi in ('x86', 'x64'):
        current = memory[abi]['default']['static_ram_bytes']
        upstream = memory[abi]['upstream_native']['static_ram_bytes']
        assert all(memory[abi][name]['static_ram_bytes'] == current for name in labels)
        lines.append(f'| {abi} | {current} B | {upstream} B | +{current-upstream} B |')
    for mode, label in (('normal', 'PIC24, enums normais'), ('short', 'PIC24, enums curtos')):
        row = next(row for row in matrix['results'] if row['configuration'] == 'default'
                   and row['profile'] == 'single' and row['enums'] == mode)
        current = row['measurements']['after']['static_bytes']
        assert all(item['measurements']['after']['static_bytes'] == current
                   for item in matrix['results'] if item['configuration'] == 'default'
                   and item['enums'] == mode)
        upstream = upstream_pic[mode]['static_bytes']
        lines.append(f'| {label} | {current} B | {upstream} B | +{current-upstream} B |')
    lines += ['', '## Pilha interna PIC24', '',
              'XC16 2.10, PIC24FJ256GA702, C99, -Os. Inclui chamadas internas e retornos; '
              'exclui backend, callbacks da aplicação e rotinas do compilador/libc, '
              'conforme o escopo acordado. Limite conservador de compilação, não execução em hardware.', '',
              '| Biblioteca / perfil | Enums normais | Enums curtos |', '|---|---:|---:|']
    for profile, label in (('single', 'Padrão'), ('multi', 'Leitura multibloco'),
                           ('query', 'Leitura multibloco + consulta')):
        values = [next(row for row in matrix['results'] if row['configuration'] == 'default'
                       and row['profile'] == profile and row['enums'] == mode)
                  ['measurements']['after']['stack']['conservative_internal_bytes']
                  for mode in ('normal', 'short')]
        lines.append(f'| {label} | {values[0]} B | {values[1]} B |')
    lines.append('| Upstream/master | '
                 f"{upstream_pic['normal']['stack']['conservative_internal_bytes']} B | "
                 f"{upstream_pic['short']['stack']['conservative_internal_bytes']} B |")
    lines += ['', f'As **48 variantes com oito setores** passaram; pico entre as configurações '
              f'opcionais: **{peak} bytes**, estritamente abaixo de 400. '
              'Os configs opcionais foram copiados para matrix-work/tests e seu config_common '
              'foi fixado em oito setores apenas nessa cópia; os configs originais do repositório não foram alterados.', '',
              '## Evidências e reprodução do relatório', '',
              f'Snapshots, objetos, trials, hashes, matriz e medições upstream PIC24: `{root.as_posix()}`. '
              'normal/lib preserva a fonte normal; after/lib é a cópia do benchmark com hora controlada. '
              'O JSON conserva todas as amostras e contagens. Não comparar diretamente estes tempos '
              'com rodadas históricas de quatro setores para atribuir um ganho ao cache sem um ensaio pareado.', '',
              '```powershell', f'python tests/cache8_write_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}', '```', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, cpu=data,
        memory=memory, pic24_matrix=matrix, upstream_pic24=upstream_pic), indent=2) + '\n')
    print(f'Passed: eight sectors, {args.repetitions} trials, 48 stack variants; peak {peak} bytes.')


if __name__ == '__main__':
    main()
