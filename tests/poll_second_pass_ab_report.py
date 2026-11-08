"""Summarize isolated conditional/unconditional second-pass experiments."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    manifest = json.loads((root / 'manifest.json').read_text())
    assert manifest['cache_sectors'] == 8
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    peak = {version: max(row['measurements'][version]['stack']['conservative_internal_bytes']
                         for row in matrix['results']) for version in ('before', 'after')}
    assert max(peak.values()) < 400
    result = dict(manifest=manifest, matrix=matrix, experiments={})
    lines = ['# A/B: segunda passagem de operações no poll', '',
             'Rodada de 18/11/2026. FAT32 8.3, clusters/setores de 512 bytes, '
             'oito setores de cache, um/dois arquivos de 1 MiB, blocos de 257/4096 bytes. '
             'Escrita multibloco habilitada nos dois lados.', '',
             '**A** é o código recebido: segunda passagem condicionada a páginas sujas '
             'e grupo de alocações pendentes, incluindo o retorno antecipado quando falso. '
             '**B** substitui esse if/else por uma chamada incondicional a '
             'afatfs_fileOperationsPoll(). A consulta final de grupo antes do flush permanece. '
             'O caminho com gravação ativa no backend continua executando uma única passagem. '
             'Portanto B significa incondicional naquele ponto READY/CLOSING, não em todos os polls.', '',
             'Esta comparação mede as duas políticas completas nesse ponto, incluindo '
             'seus efeitos no escalonamento; não isola apenas as instruções de uma comparação booleana. '
             'Nenhuma barreira, ACK, flush explícito, reserva ou regra de espelhamento foi removida.', '',
             f'MSVC x64 /O2, {args.repetitions} repetições intercaladas, ordem invertida em rodadas '
             'alternadas e afinidade fixa; sem compilações concorrentes. Hora FAT fixada '
             'em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. Os ciclos do host '
             'incluem biblioteca, backend, polling e preparação dos dados. A auditoria '
             'completa do volume fica fora da janela de escrita; os arquivos também são '
             'relidos em 32 passagens. Não são medições em cartão físico ou PIC24.', '',
             'Percentual positivo na coluna CPU B/A significa **B mais caro**. É a mediana '
             'das razões pareadas; não necessariamente a razão das medianas de ciclos/byte. '
             'Pequenas diferenças devem ser interpretadas considerando o ruído do host.', '']
    for schedule, directory in (('Por bloco', root), ('Pedidos sobrepostos', root / 'interleaved')):
        variants = {name for _, old, new in PAIRS for name in (old, new)}
        data = {name: measurements(directory / f'cpu-{name}', args.repetitions) for name in variants}
        memory = {name: resources(directory / f'cpu-{name}') for name in variants}
        comparisons = {}
        lines += [f'## {schedule}', '',
                  ('O harness termina o bloco de um escritor antes de tentar o outro.'
                   if directory == root else
                   'COMPARISON_INTERLEAVED_REQUESTS tenta atender ambos os escritores antes de aguardar progresso. '
                   'Não atribuir diferenças entre os dois escalonamentos à alteração da biblioteca.'), '',
                  '| Perfil | Bloco | Escritores | A ciclos/byte | B ciclos/byte | CPU B/A |',
                  '|---|---:|---:|---:|---:|---:|']
        io_rows = []
        for label, old, new in PAIRS:
            assert memory[old]['static_ram_bytes'] == memory[new]['static_ram_bytes'] == 4872
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = f'write/0/1/{chunk}/{files}'
                    first = data[old][key]
                    second = data[new][key]
                    delta = statistics.median((after / before - 1) * 100
                                              for before, after in zip(first['samples'], second['samples']))
                    comparisons[f'{new}/{key}'] = delta
                    lines.append(f"| {label} | {chunk} | {files} | {first['cycles_per_byte']:.4f} | "
                                 f"{second['cycles_per_byte']:.4f} | {delta:+.2f}% |")
                    values = [f"{first[field]} / {second[field]}" for field in
                              ('reads', 'writes', 'metadata_writes', 'polls')]
                    io_rows.append(f'| {label} | {chunk} | {files} | ' + ' | '.join(values) + ' |')
        lines += ['', 'I/O de escrita em setores de 512 bytes, incluindo fechamento e drenagem. '
                  'Valores nas células: **A / B**.', '',
                  '| Perfil | Bloco | Escritores | R | W | Metadados W | Polls |',
                  '|---|---:|---:|---:|---:|---:|---:|', *io_rows, '']
        result['experiments'][schedule] = dict(data=data, resources=memory, cpu_B_over_A_percent=comparisons)
    first = function_code(root / 'cpu-before_default/upstream_comparison.obj', 'runCase')
    second = function_code(root / 'interleaved/cpu-before_default/upstream_comparison.obj', 'runCase')
    assert first != second, 'Interleaved benchmark flag did not change the harness.'
    lines += ['## RAM, pilha e persistência', '',
              'RAM estática da biblioteca x64: **4.872 bytes em A e B**, nos três perfis '
              'e nos dois escalonamentos. Não houve acréscimo de buffer, campo ou heap. '
              'As 48 comparações de seções estáticas PIC24 entre A/B também foram iguais.', '',
              f"Pilha interna conservadora máxima: **A {peak['before']} B / B {peak['after']} B**, "
              'abaixo de 400 bytes. XC16 2.10, PIC24FJ256GA702, C99 -Os, enums normais/curtos. '
              'Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação '
              'e rotinas do compilador/libc conforme o escopo acordado.', '',
              'Em cada variante passaram 11 cenários gerais e 10 verificações de alocação: '
              'ordenação dos vínculos antes de cada write, três arquivos, pedidos sobrepostos, '
              'volume quase cheio, espelho e reserva única, além de falhas de I/O/ACK, '
              'remoção/reinserção, remontagem e desligamento limpo. A auditoria de conteúdo '
              'e das duas FATs passou em todas as execuções do benchmark. Esses resultados '
              'validam os cenários exercitados; não são uma prova de todas as interrupções possíveis.', '',
              'Os testes opcionais e a matriz usaram cópias de config_common com oito setores. '
              'As fontes e configurações do workspace não foram substituídas pelo experimento.', '',
              '## Evidências', '',
              f'`{root.as_posix()}`: A/lib e B/lib são fontes normais; before/lib e after/lib '
              'são as cópias com hora controlada do benchmark. matrix/report.json contém '
              'os limites e hashes. A/build e B/build contêm as regressões. Trials por bloco '
              'estão na raiz e pedidos sobrepostos em interleaved/. O JSON ao lado deste '
              'relatório conserva todas as amostras, contagens e medições de RAM.', '',
              '```powershell', f'python tests/poll_second_pass_ab_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}', '```', '']
    result['workspace_unchanged'] = all(hashlib.sha256((Path('lib') / name).read_bytes()).hexdigest() == digest
                                      for name, digest in manifest['workspace_hashes'].items())
    assert result['workspace_unchanged']
    summary = ['## Resultado', '',
               '**Manter a segunda passagem condicional (A).** B consumiu mais CPU '
               'em todos os casos medidos e não reduziu I/O ou o número de polls.', '']
    for schedule, experiment in result['experiments'].items():
        changes = list(experiment['cpu_B_over_A_percent'].values())
        assert min(changes) > 0
        for _, old, new in PAIRS:
            for key, first in experiment['data'][old].items():
                if key.startswith('write/'):
                    second = experiment['data'][new][key]
                    assert all(first[field] == second[field] for field in
                               ('reads', 'writes', 'metadata_reads', 'metadata_writes',
                                'polls', 'virtual_us', 'multi_sessions', 'multi_blocks',
                                'read_sessions', 'batched_read_blocks', 'read_stops'))
        summary.append(f'- {schedule}, todos os perfis: B usou **{min(changes):.2f}% a '
                       f'{max(changes):.2f}% mais CPU** que A.')
    summary += ['', 'A checagem final de grupo continuou em B. Retirar o filtro anterior '
                'não eliminou todas as consultas de agrupamento; acrescentou a passagem '
                'de arquivos quando A a dispensava. Não foi observado benefício desse '
                'trabalho adicional nos workloads testados. A condição é útil como '
                'filtro de escalonamento, sem ser ela própria uma barreira de persistência.', '']
    lines[2:2] = summary
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Validated: both schedules, RAM equal, stack A/B {peak}, workspace unchanged.')


if __name__ == '__main__':
    main()
