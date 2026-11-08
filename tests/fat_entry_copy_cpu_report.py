"""Validate paired FAT mirror entry-copy trials, resources and eight-sector stack matrix."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

from cache_index_cpu_report import measurements, resources
from read_cpu_report import function_code


PAIRS = (('Padrão', 'before_default', 'default'),
         ('Leitura multibloco', 'before', 'after_no_query'),
         ('Leitura multibloco + consulta', 'before_query', 'after'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--initial-directory', type=Path)
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
    for result in matrix['results']:
        before = result['measurements']['before']
        after = result['measurements']['after']
        assert before['static_sections'] == after['static_sections']
        assert after['stack']['conservative_internal_bytes'] < 400
    for path in (root / 'normal/lib/asyncfatfs_config.h',
                 root / 'matrix-work/tests/config_common.h',
                 root / 'upstream/lib/asyncfatfs.c'):
        assert '#define AFATFS_NUM_CACHE_SECTORS 8' in path.read_text()
    assert digest(Path('lib/asyncfatfs.c')) == digest(root / 'normal/lib/asyncfatfs.c')
    assert digest(root / 'normal/lib/asyncfatfs.c') == digest(root / 'after/lib/asyncfatfs.c')
    for path in (root / 'before/lib').iterdir():
        if path.is_file() and path.name not in ('asyncfatfs.c', 'builddefs.h'):
            assert digest(path) == digest(root / 'normal/lib' / path.name), path
    report = dict(repetitions=args.repetitions, cache_sectors=8, matrix=matrix,
                  upstream_matrix=upstream_matrix, experiments={}, resources={},
                  source_hashes={name: digest(root / f'{name}/lib/asyncfatfs.c')
                                 for name in ('before', 'normal', 'after', 'upstream')})
    if args.initial_directory:
        initial = args.initial_directory
        confirmation = {name: measurements(initial / f'confirm/cpu-{name}', args.repetitions)
                        for name in ('before_default', 'default', 'upstream_native')}
        report['initial_experiment'] = dict(
            directory=initial.as_posix(),
            report=json.loads((initial / 'initial-report.json').read_text()),
            confirmation=confirmation,
            unchanged_functions={name: function_code(initial / 'cpu-before_default/asyncfatfs.obj', name)
                                 == function_code(initial / 'cpu-default/asyncfatfs.obj', name)
                                 for name in ('afatfs_fread', 'afatfs_cacheSector', 'afatfs_poll', 'afatfs_fwrite')})
    lines = ['# Escrita: atualizações localizadas do espelho da FAT', '',
             'FAT32 8.3, clusters de 512 bytes e **oito setores de cache** em todas as versões. '
             'Referência local upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24`. '
             'A é o código anterior; B é a alteração mantida.', '',
             'No pipeline nativo FAT32, uma página espelho já DIRTY conserva a imagem do '
             'último par preparado. O helper captura essa condição antes de marcar as '
             'páginas sujas e a devolve por um bool local. Depois de alterar a primária, '
             'o chamador copia quatro bytes nesse caso. Espelho EMPTY, limpo ou em outro '
             'estado recebe a imagem integral atualizada de 512 bytes, sem uma cópia '
             'adicional de quatro bytes; não se presume igualdade apenas pela presença no cache. '
             'FAT16 e os caminhos opcionais sem pipeline mantêm a cópia integral. '
             'Preenchimentos de várias entradas continuam espelhando o setor inteiro.', '',
             'Não há novo campo, buffer ou reserva. Aquisição das duas páginas, marcação '
             'DIRTY, submissões, ACKs e barreiras EOC → vínculo → diretório permanecem. '
             'A otimização reduz cópias em RAM, não o tamanho dos setores gravados.', '',
             f'MSVC x64 /O2, {args.repetitions} repetições pareadas, ordem alternada e '
             'afinidade fixa, sem compilações concorrentes durante a medição. '
             'Arquivos de 1 MiB por escritor, backend simulado e 32 passagens de leitura '
             'para conferir conteúdo. Hora FAT fixada em 12:00:00 apenas nos snapshots '
             'COMPARISON_CPU_ONLY. Ciclos incluem biblioteca, backend, polling e preparação '
             'do harness; auditoria completa do volume fora da janela de escrita. '
             'Não são tempos de PIC24 ou cartão físico.', '',
             'Redução B/A positiva significa menos CPU, calculada pela mediana das razões '
             'pareadas. CPU adicional ao upstream usa razão das medianas. '
             'Diferenças pequenas podem refletir ruído do host.', '']
    for schedule, directory, pairs in (
            ('Por bloco', root, PAIRS),
            ('Pedidos intercalados', root / 'interleaved', PAIRS[:1])):
        variants = {'upstream_native'}
        for _, old, new in pairs:
            variants.update((old, new))
        data = {name: measurements(directory / f'cpu-{name}', args.repetitions)
                for name in variants}
        comparisons = {}
        lines += [f'## {schedule}', '',
                  ('O harness termina o pedido de um escritor antes do outro.' if directory == root else
                   'O harness tenta atender ambos os escritores antes de aguardar progresso '
                   '(COMPARISON_INTERLEAVED_REQUESTS).'), '',
                  '| Perfil | Bloco | Escritores | A ciclos/B | B ciclos/B | Redução B/A | Upstream ciclos/B | CPU adicional upstream |',
                  '|---|---:|---:|---:|---:|---:|---:|---:|']
        for label, old, new in pairs:
            for key in data[new]:
                fields = ('payload_bytes', 'reads', 'writes', 'metadata_reads', 'metadata_writes',
                          'polls', 'virtual_us', 'multi_sessions', 'multi_blocks',
                          'read_sessions', 'batched_read_blocks', 'read_stops')
                assert all(data[old][key][field] == data[new][key][field] for field in fields), (schedule, new, key)
                comparisons[f'{new}/{key}'] = statistics.median(
                    (1 - after / before) * 100 for before, after in
                    zip(data[old][key]['samples'], data[new][key]['samples']))
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = f'write/0/1/{chunk}/{files}'
                    before = data[old][key]['cycles_per_byte']
                    after = data[new][key]['cycles_per_byte']
                    upstream = data['upstream_native'][key]['cycles_per_byte']
                    reduction = comparisons[f'{new}/{key}']
                    lines.append(f'| {label} | {chunk} | {files} | {before:.4f} | {after:.4f} | '
                                 f'{reduction:+.2f}% | {upstream:.4f} | {(after / upstream - 1) * 100:+.2f}% |')
        lines += ['', 'I/O determinístico entre repetições e **idêntico antes/depois** '
                  'para cada perfil e escalonamento, incluindo sessões e polling. '
                  'A tabela compara B padrão com upstream; R/W são setores de 512 bytes, '
                  'incluindo fechamento e drenagem.', '',
                  '| Bloco | Escritores | R B/upstream | W B/upstream | Metadados W B/upstream | Polls B/upstream |',
                  '|---|---:|---:|---:|---:|---:|']
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'write/0/1/{chunk}/{files}'
                current = data['default'][key]
                upstream = data['upstream_native'][key]
                values = [f'{current[field]} / {upstream[field]}' for field in
                          ('reads', 'writes', 'metadata_writes', 'polls')]
                lines.append(f'| {chunk} | {files} | ' + ' | '.join(values) + ' |')
        report['experiments'][schedule] = dict(data=data, paired_cpu_reduction_percent=comparisons)
    assert function_code(root / 'cpu-default/upstream_comparison.obj', 'runCase') != function_code(
        root / 'interleaved/cpu-default/upstream_comparison.obj', 'runCase')
    lines += ['', '## RAM, código e pilha', '',
              '| ABI/perfil | RAM A → B / upstream | Código A → B / upstream |',
              '|---|---:|---:|']
    for abi, prefix, pairs in (('x64', 'cpu', PAIRS), ('x86', 'x86', PAIRS[:1])):
        upstream = resources(root / f'{prefix}-upstream_native')
        for label, old, new in pairs:
            before = resources(root / f'{prefix}-{old}')
            after = resources(root / f'{prefix}-{new}')
            assert before['static_ram_bytes'] == after['static_ram_bytes']
            report['resources'][f'{abi}/{new}'] = dict(before=before, after=after, upstream=upstream)
            lines.append(f"| {abi}/{label} | {before['static_ram_bytes']} → {after['static_ram_bytes']} / "
                         f"{upstream['static_ram_bytes']} B | {before['code_bytes']} → {after['code_bytes']} / "
                         f"{upstream['code_bytes']} B |")
    lines += ['', '| PIC24 padrão | RAM A → B / upstream | Pilha A → B / upstream |',
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
    peak = max(item['measurements']['after']['stack']['conservative_internal_bytes']
               for item in matrix['results'])
    lines += ['', f'As **48 variantes PIC24** passaram com seções de RAM estática iguais '
              f'antes/depois. Pico interno conservador: **{peak} bytes**, estritamente abaixo '
              'de 400. XC16 2.10, PIC24FJ256GA702, C99 -Os, enums normais/curtos. '
              'Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação '
              'e rotinas do compilador/libc.', '',
              '## Interpretação da CPU', '',
              'A redução de cópias foi confirmada pelo teste focado. O efeito na CPU '
              'deve ser lido por cenário, incluindo resultados negativos quando presentes '
              'e o ruído da medição. A otimização não reduziu submissões, polling ou '
              'o custo das barreiras.', '',
              '| Controle de leitura por bloco | Menor / maior redução pareada B/A |',
              '|---|---:|']
    serial = report['experiments']['Por bloco']['paired_cpu_reduction_percent']
    for label, _, new in PAIRS:
        values = [value for key, value in serial.items() if key.startswith(f'{new}/read/')]
        lines.append(f'| {label} | {min(values):+.2f}% / {max(values):+.2f}% |')
    lines += ['',
              '## Persistência e controles', '',
              'A validade da atualização localizada é restrita ao pipeline nativo: '
              'as páginas espelho não são carregadas por READ nesse caminho; entram '
              'pelo helper de aquisição pareada. A primeira imagem é integral, e cada '
              'chamador atualiza o espelho antes de devolver controle (por entrada ou '
              'por setor no preenchimento). Não há yield entre as alterações do par. '
              'Falha de ACK conserva o mesmo buffer e DIRTY. Quando o espelho está '
              'limpo ou em voo, o fallback integral continua obrigatório. Estado DIRTY '
              'isoladamente não seria prova de igualdade para um futuro chamador '
              'que alterasse apenas a primária; esse chamador teria de preservar o '
              'contrato de atualização pareada.', '',
              'Passaram os 11 casos funcionais e os 10 checks de alocação. O teste focado '
              'conta cópias: espelho novo exige uma cópia de 512 bytes e nenhuma de quatro; '
              'a alteração seguinte do par sujo exige zero cópias de 512 e uma de quatro. '
              'Verifica vizinhos, bits altos, reparação integral de espelho limpo antigo, '
              'falha de ACK seguida de nova alteração localizada e regravação das duas '
              'FATs com conteúdo esperado, além de primária expulsa após ACK enquanto '
              'o espelho ainda está sujo. Os checks existentes cobrem ordenação, '
              'reserva, pressão, três arquivos e alocações sobrepostas. Auditorias do '
              'benchmark confirmaram igualdade das duas FATs atuais. Upstream não mantém '
              'a segunda cópia atualizada nesse workload; a comparação não representa '
              'contratos de persistência iguais.', '',
              'A cobertura Doxygen continua em 295 definições. Não foram usados VHDs '
              'externos; estes testes geram mídia sintética.', '',
              'Os resultados de leitura de controle, amostras e contagens completas estão '
              'no JSON ao lado. Não atribuir os ciclos agregados exclusivamente ao memcpy. '
              'A diferença restante para upstream inclui duas FATs e ordenação mais forte.', '',
              '## Reprodução e evidências', '',
              f'`{root.as_posix()}` conserva snapshots before/after, normal/lib sem hora '
              'fixada, upstream canônico, objetos, trials, scripts derivados para x86 '
              'e pedidos intercalados, testes e matriz PIC24. O JSON registra hashes.', '',
              '```powershell',
              f'python tests/fat_entry_copy_cpu_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}'
              + (f' --initial-directory {args.initial_directory.as_posix()}' if args.initial_directory else ''),
              '```', '']
    if args.initial_directory:
        lines += ['O primeiro arranjo, não selecionado, repetia os testes de ponteiro/FS '
                  'antes da cópia localizada e apresentou regressão de leitura de 8,71% '
                  'em um leitor/4096, repetida em outra rodada (10,78%). fread, cacheSector, '
                  'poll e fwrite tinham código de objeto idêntico nesse A/B; efeito de '
                  'posicionamento do código é uma hipótese, sem causa isolada demonstrada. '
                  'Uma compactação intermediária também não resolveu esse controle '
                  '(11,05%, evidências em fat-entry-copy-final). A versão final devolve '
                  'a condição da imagem por um bool local e compartilha o fallback de '
                  '512 bytes já existente no chamador. Assim evita inicializar a página '
                  'e depois alterá-la novamente e evita duplicar a cópia integral no helper. '
                  'Preenchimento em lote não é compilado no perfil nativo analisado; '
                  'a tentativa entryUpdate em fat-entry-copy-selected não foi selecionada '
                  'nem medida em CPU. A regressão desse controle de leitura permaneceu '
                  'na versão final (10,14%); a alteração não é melhoria global de CPU. '
                  'Todos os números principais pertencem à nova rodada; '
                  'não foram combinadas amostras das duas implementações. '
                  f'Evidências iniciais: `{args.initial_directory.as_posix()}`, '
                  'também preservadas no JSON.', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
