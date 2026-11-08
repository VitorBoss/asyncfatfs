"""Relate physical sector I/O, CPU cycles and retry-dependent harness work."""
import argparse
import json
from pathlib import Path

from cache_index_cpu_report import measurements, resources
from mirror_profile_fix_report import digest, object_code
from write_dispatch_cpu_report import COUNTERS


SCHEDULES = ('serial', 'interleaved', 'prepared')
VERSIONS = ('current', 'upstream')
TRACE_FIELDS = {
    'META': ('primary_writes', 'mirror_writes', 'directory_writes'),
    'META_READ': ('primary_reads', 'mirror_reads', 'directory_reads'),
    'FAT_NOOP': ('unchanged_primary_writes', 'unchanged_mirror_writes'),
    'WORK': ('generated_bytes', 'write_attempts', 'pump_calls'),
}


def audit(directory):
    # Timing from these instrumented executables is deliberately discarded.
    observed = measurements(directory, 1)
    result = {key: {field: row[field] for field in COUNTERS} for key, row in observed.items()}
    seen = set()
    for line in (directory / 'trial-0.log').read_text().splitlines():
        parts = line.split(',')
        if parts[0] in TRACE_FIELDS:
            key = '/'.join(parts[1:6])
            assert (parts[0], key) not in seen
            seen.add((parts[0], key))
            fields = TRACE_FIELDS[parts[0]]
            values = list(map(int, parts[7:]))
            assert len(values) == len(fields)
            result[key].update(zip(fields, values))
    assert len(seen) == 32
    return result


def key_for(chunk, files):
    return f'write/0/1/{chunk}/{files}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, default=21)
    parser.add_argument('--previous-report', type=Path, default=Path('tests/WRITE_DISPATCH_CPU.json'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.directory
    previous = json.loads(args.previous_report.read_text())
    assert digest(Path('lib/asyncfatfs.c')) == digest(root / 'current/lib/asyncfatfs.c')
    assert digest(root / 'current/lib/asyncfatfs.c') == previous['sources']['normal']
    for name, expected in previous['matrix']['sources']['after'].items():
        assert digest(Path('lib') / name) == expected, name
        if name != 'builddefs.h':
            assert digest(root / 'current/lib' / name) == expected, name
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'current/lib/asyncfatfs_config.h').read_text()
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (root / 'upstream/lib/asyncfatfs.c').read_text()
    experiments = {}
    code = {}
    for version in VERSIONS:
        code[version] = {}
        for source in ('asyncfatfs', 'fat_standard'):
            expected = object_code(root / f'cpu-serial-{version}/{source}.obj')
            for schedule in SCHEDULES:
                for kind in ('cpu', 'audit'):
                    assert expected == object_code(root / f'{kind}-{schedule}-{version}/{source}.obj'), (kind, schedule, version, source)
            code[version][source] = expected
    for schedule in SCHEDULES:
        data = {version: measurements(root / f'cpu-{schedule}-{version}', args.repetitions) for version in VERSIONS}
        audits = {version: audit(root / f'audit-{schedule}-{version}') for version in VERSIONS}
        for version in VERSIONS:
            for key, row in data[version].items():
                traced = audits[version][key]
                assert all(row[field] == traced[field] for field in COUNTERS), (schedule, version, key)
                assert row['metadata_writes'] == sum(traced[field] for field in TRACE_FIELDS['META'])
                assert row['metadata_reads'] == sum(traced[field] for field in TRACE_FIELDS['META_READ'])
                assert row['polls'] >= row['reads'] + row['writes']
                if key.startswith('write/'):
                    assert row['writes'] - row['metadata_writes'] == row['payload_bytes'] // 512
                    if schedule != 'interleaved':
                        assert traced['generated_bytes'] == row['payload_bytes']
        experiments[schedule] = dict(cpu=data, audit=audits)
    for version in VERSIONS:
        for key in experiments['prepared']['cpu'][version]:
            original = experiments['interleaved']['audit'][version][key]
            prepared = experiments['prepared']['audit'][version][key]
            assert all(original[field] == prepared[field] for field in (*COUNTERS, 'write_attempts', 'pump_calls'))
    memory = {version: resources(root / f'cpu-serial-{version}') for version in VERSIONS}
    assert memory['current'] == previous['resources']['x64/default']['after']
    report = dict(repetitions=args.repetitions, cache_sectors=8, experiments=experiments,
                  code_identities=code, library_resources=memory,
                  sources={version: digest(root / f'{version}/lib/asyncfatfs.c') for version in VERSIONS},
                  harness_sha256=digest(root / 'harness.c'),
                  original_harness_sha256=digest(root / 'harness-original.c'),
                  previous_report=str(args.previous_report))
    lines = ['# Relação entre I/O e CPU: versão atual e upstream/master', '',
             'FAT32 8.3, clusters de 512 bytes, API original, **oito setores de cache** '
             'nos dois códigos. Atual = fonte de WRITE_DISPATCH_CPU, sem alterações '
             'na biblioteca nesta investigação. Upstream/master local: '
             '`60c5f3580152410cb08342942cbbe3a30a449c24`.', '',
             'O I/O adicional é inteiramente de metadados: os dados são escritos '
             'exatamente uma vez em ambos. Mais transferências e etapas de confirmação '
             'produzem mais trabalho de CPU, mas os totais não crescem na mesma '
             'proporção. Há também uma amplificação no harness intercalado: ele '
             'regenera dados pendentes a cada retry. O experimento abaixo separa '
             'essa preparação de dados da sequência de operações da biblioteca.', '',
             '## Método e limites', '',
             f'MSVC x64 /O2, {args.repetitions} repetições pareadas, ordem alternada, '
             'afinidade fixa, 1 MiB por arquivo e 32 passagens de leitura para validar '
             'o conteúdo. Todas as compilações terminaram antes da medição. '
             'Ciclos incluem biblioteca, backend simulado e harness; não são tempos '
             'exclusivos da biblioteca nem medições em PIC24 ou cartão físico.', '',
             'A CPU foi medida sem os novos contadores. Executáveis separados '
             'COMPARISON_METADATA_TRACE + COMPARISON_CPU_WORK_AUDIT contam FAT primária, '
             'espelho, diretório, bytes gerados para escrita, tentativas fwrite e '
             'chamadas pump. O timing desses executáveis foi descartado. Seus '
             'contadores de I/O e polling coincidem com os dos executáveis de CPU.', '',
             'O perfil CPU usa latência virtual mínima: cada transferência termina '
             'no próximo sdcard_poll. Não mede espera física do cartão. Portanto '
             '`polls − (leituras + escritas)` conta polls sem conclusão de setor '
             'nesse perfil; não representa espera medida de hardware.', '',
             '## I/O e CPU por bloco', '',
             'Valores nas células: atual / upstream. CPU é a mediana de ciclos por '
             'byte aceito. I/Os são leituras + escritas de setores de 512 bytes '
             'no backend simulado, não comandos físicos do cartão nem programações NAND. '
             'As razões são descritivas, não coeficientes causais de custo por operação.', '',
             '| Bloco | Escritores | I/Os | Razão I/O | CPU ciclos/B | Razão CPU | Polls | Polls sem conclusão de setor |',
             '|---|---:|---:|---:|---:|---:|---:|---:|']
    serial = experiments['serial']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = key_for(chunk, files)
            current = serial['cpu']['current'][key]
            upstream = serial['cpu']['upstream'][key]
            current_io = current['reads'] + current['writes']
            upstream_io = upstream['reads'] + upstream['writes']
            current_cpu = current['cycles_per_byte']
            upstream_cpu = upstream['cycles_per_byte']
            lines.append(f"| {chunk} | {files} | {current_io} / {upstream_io} | {current_io / upstream_io:.3f}× | "
                         f"{current_cpu:.4f} / {upstream_cpu:.4f} | {current_cpu / upstream_cpu:.3f}× | "
                         f"{current['polls']} / {upstream['polls']} | "
                         f"{current['polls'] - current_io} / {upstream['polls'] - upstream_io} |")
    lines += ['', 'Com 257 bytes, muitos polls adicionais não concluem setor; o custo '
              'inclui retomadas da máquina de estados e novas tentativas de escrita. '
              'Com 4096 bytes, o I/O aumenta proporcionalmente mais que a CPU. '
              'Isso impede explicar o tempo apenas multiplicando ciclos por número '
              'de setores gravados.', '',
              '| Bloco | Escritores | Pedidos lógicos mínimos | Tentativas fwrite atual/up | Tentativas além do mínimo atual/up |',
              '|---|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = key_for(chunk, files)
            current = serial['audit']['current'][key]
            upstream = serial['audit']['upstream'][key]
            requests = ((1048576 + chunk - 1) // chunk) * files
            lines.append(f"| {chunk} | {files} | {requests} | {current['write_attempts']} / {upstream['write_attempts']} | "
                         f"{current['write_attempts'] - requests} / {upstream['write_attempts'] - requests} |")
    lines += ['', 'As tentativas excedentes incluem retornos parciais e tentativas sem '
              'aceitar bytes; não são uma contagem isolada de BUSY. No serial, os dois '
              'códigos geram exatamente 1× o payload, portanto essa tabela não tem '
              'a regeneração do modo intercalado histórico.', '',
              '## De onde vêm as gravações adicionais', '',
              'Contagem de transferências por destino, por bloco. Não confundir UPDATE_FAT1/'
              'UPDATE_FAT2 (EOC/vínculo) com as duas cópias da FAT.', '',
              '| Bloco | Escritores | Dados atual/up | FAT primária atual/up | Espelho atual/up | Diretório atual/up | W adicional total |',
              '|---|---:|---:|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = key_for(chunk, files)
            current = serial['audit']['current'][key]
            upstream = serial['audit']['upstream'][key]
            data_current = current['writes'] - current['metadata_writes']
            data_upstream = upstream['writes'] - upstream['metadata_writes']
            extra = current['writes'] - upstream['writes']
            primary = current['primary_writes'] - upstream['primary_writes']
            mirror = current['mirror_writes'] - upstream['mirror_writes']
            directory = current['directory_writes'] - upstream['directory_writes']
            assert extra == primary + mirror + directory
            values = [f"{current[field]} / {upstream[field]}" for field in TRACE_FIELDS['META']]
            lines.append(f'| {chunk} | {files} | {data_current} / {data_upstream} | ' +
                         ' | '.join(values) + f' | {extra} = {primary} + {mirror} + ({directory}) |')
    lines += ['', 'O espelho explica parte da diferença. A primária também pode ser '
              'gravada mais vezes: a versão atual confirma EOC nas duas FATs antes '
              'de publicar o vínculo e confirma o vínculo antes do diretório. O '
              'upstream pode coalescer alterações ainda não confirmadas e não '
              'atualiza a segunda FAT nesse workload. Em contrapartida, a política '
              'atual pode reduzir publicações intermediárias do diretório. A '
              'decomposição acima separa esses efeitos em vez de atribuir todo '
              'o excesso à segunda cópia.', '',
              '## Amplificação de trabalho no harness intercalado', '',
              'O modo histórico gera `amount − written[index]` bytes antes de cada '
              'tentativa, inclusive quando fwrite não aceita nada. O modo preparado '
              'mantém os padrões distintos por arquivo, mas os gera uma única vez '
              'por pedido em buffers separados. Não altera a biblioteca, a ordem '
              'de tentativas ou o loop de polling. I/O, polling, tentativas fwrite '
              'e chamadas pump são **exatamente iguais** nos dois modos, por versão.', '',
              '| Bloco | Escritores | Bytes gerados/payload histórico atual/up | Tentativas fwrite atual/up | CPU histórico atual/up | CPU preparado atual/up | CPU extra ao upstream histórico → preparado |',
              '|---|---:|---:|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = key_for(chunk, files)
            old_current = experiments['interleaved']['cpu']['current'][key]
            old_upstream = experiments['interleaved']['cpu']['upstream'][key]
            new_current = experiments['prepared']['cpu']['current'][key]
            new_upstream = experiments['prepared']['cpu']['upstream'][key]
            work_current = experiments['interleaved']['audit']['current'][key]
            work_upstream = experiments['interleaved']['audit']['upstream'][key]
            generated_current = work_current['generated_bytes'] / work_current['payload_bytes']
            generated_upstream = work_upstream['generated_bytes'] / work_upstream['payload_bytes']
            first = old_current['cycles_per_byte']
            second = old_upstream['cycles_per_byte']
            third = new_current['cycles_per_byte']
            fourth = new_upstream['cycles_per_byte']
            lines.append(f"| {chunk} | {files} | {generated_current:.2f}× / {generated_upstream:.2f}× | "
                         f"{work_current['write_attempts']} / {work_upstream['write_attempts']} | "
                         f'{first:.4f} / {second:.4f} | {third:.4f} / {fourth:.4f} | '
                         f'{(first / second - 1) * 100:+.2f}% → {(third / fourth - 1) * 100:+.2f}% |')
    key = key_for(4096, 2)
    original_gap = experiments['interleaved']['cpu']['current'][key]['cycles_per_byte'] - experiments['interleaved']['cpu']['upstream'][key]['cycles_per_byte']
    prepared_gap = experiments['prepared']['cpu']['current'][key]['cycles_per_byte'] - experiments['prepared']['cpu']['upstream'][key]['cycles_per_byte']
    report['interleaved_4096_two_writer_gap'] = dict(original_cycles_per_byte=original_gap,
                                                   prepared_cycles_per_byte=prepared_gap,
                                                   reduction_percent=(1 - prepared_gap / original_gap) * 100)
    lines += ['', 'Nos buffers preparados, bytes gerados/payload = **1× nos dois '
              'códigos**, assim como no teste serial. Para dois escritores/4096, '
              f'a diferença absoluta de CPU cai de {original_gap:.4f} para '
              f'{prepared_gap:.4f} ciclos/B ({(1 - prepared_gap / original_gap) * 100:.2f}% menor), '
              'sem reduzir I/O. Essa é a diferença observada ao remover a regeneração '
              'e mudar a preparação/cache dos buffers; não é um percentual de CPU '
              'exclusiva da biblioteca nem uma melhoria no código FAT.', '',
              'Não misturar os destinos da tabela serial com os do teste intercalado. '
              'Para dois escritores/4096 nos modos intercalado histórico e preparado:', '',
              '| Versão | R | W dados | W primária | W espelho | W diretório | Polls | Sequências/setores multibloco de escrita |',
              '|---|---:|---:|---:|---:|---:|---:|---:|']
    for version in VERSIONS:
        row = experiments['prepared']['audit'][version][key]
        lines.append(f"| {version} | {row['reads']} | {row['writes'] - row['metadata_writes']} | "
                     f"{row['primary_writes']} | {row['mirror_writes']} | {row['directory_writes']} | "
                     f"{row['polls']} | {row['multi_sessions']} / {row['multi_blocks']} |")
    noops = sum(experiments[schedule]['audit']['current'][key][field]
                for schedule in ('serial', 'interleaved')
                for key in experiments[schedule]['audit']['current'] if key.startswith('write/')
                for field in TRACE_FIELDS['FAT_NOOP'])
    lines += ['', f'A auditoria encontrou **{noops} gravações FAT atuais com imagem '
              'inteira idêntica à já armazenada**, nos casos serial e intercalado. '
              'Isso não prova que o número de gravações seja mínimo: imagens '
              'diferentes ainda podem ser coalescidas quando a ordenação permitir. '
              'O teste não contou imagens idênticas do diretório.', '',
              '## Relação no percurso do código', '',
              '| Trabalho | Relação com I/O/CPU |', '|---|---|',
              '| Atualizar FAT | A versão atual adquire/mantém duas páginas e, no padrão, copia a imagem para o espelho; upstream altera a primária. Isso custa CPU antes da submissão. |',
              '| Submeter e concluir setor | Cada transferência executa busca/estado de cache, backend, cópia de 512 bytes no simulador e callback. Mais setores implicam mais eventos. |',
              '| Esperar barreiras e retomar | Pode exigir polls e novas entradas em fwrite sem concluir outro setor. É trabalho adicional além da transferência. |',
              '| Coalescer metadados | Muda o número de escritas por byte útil; depende de cache, tamanho do pedido e garantias de ordenação. |',
              '| Preparar dados da aplicação | No harness histórico intercalado, retries induzidos por I/O repetem a geração do restante do bloco, amplificando a CPU medida. |', '',
              'Há controles anteriores com I/O constante e CPU diferente: o atalho '
              'de despacho de WRITE_DISPATCH_CPU reduziu CPU sem alterar os contadores; '
              'os perfis multibloco atuais também têm o mesmo I/O de escrita do '
              'padrão, com custos de CPU diferentes. Logo a correlação é real, mas '
              'não determina sozinha o custo de execução.', '',
              'Não é possível atribuir com estes totais uma fração exata da CPU '
              'ao driver, aos callbacks ou à segunda FAT. Dividir ciclos por '
              'transferências/polls dá médias do workload inteiro, não o custo '
              'marginal de cada evento. Também não se deve comparar as duas '
              'implementações como se oferecessem a mesma persistência.', '',
              '## Integridade, RAM e reprodução', '',
              'Todos os executáveis verificaram o conteúdo dos arquivos e a imagem '
              'do volume. A atual manteve as duas FATs iguais. Os objetos da biblioteca '
              'têm instruções idênticas entre os três modos e entre CPU/auditoria, '
              'por versão. A fonte da biblioteca tem o mesmo hash da última matriz '
              'validada em WRITE_DISPATCH_CPU; não houve mudança de RAM, cache, pilha '
              'ou persistência. A matriz PIC24 anterior continua aplicável à mesma '
              'fonte (máximo 378 bytes internos); não foi recompilada nesta análise.', '',
              'Os contadores extras existem somente no harness de auditoria. O modo '
              'preparado usa três buffers locais de 4096 bytes na aplicação de teste '
              'Windows; eles não entram na RAM estática ou pilha interna da biblioteca '
              'e não fazem parte da produção. As macros são opcionais; o modo histórico '
              'permanece disponível para reproduzir relatórios antigos.', '',
              f'`{root.as_posix()}` conserva fontes congeladas das bibliotecas, '
              'harness original/modificado, executáveis de CPU/auditoria, builds e '
              'trials. O JSON ao lado conserva todas as amostras não instrumentadas '
              'e somente os contadores das auditorias (seus timings não foram usados).', '',
              '```powershell',
              f'./tests/run_io_cpu_relation.ps1 -OutputDirectory {root.as_posix()} -Repetitions {args.repetitions}',
              f'python tests/io_cpu_relation_report.py --directory {root.as_posix()} '
              f'--repetitions {args.repetitions} --output {args.output.as_posix()}', '```', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
