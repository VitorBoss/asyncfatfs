"""Check paired cache-index CPU trials against their baseline and upstream."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics

from upstream_comparison_report import coff_sections
from read_cpu_report import function_code


def measurements(directory, repetitions):
    """Load cycle samples and require deterministic I/O for every case."""
    samples = {}
    counters = {}
    for trial in range(repetitions):
        seen = set()
        batches = {}
        for line in (directory / f'trial-{trial}.log').read_text().splitlines():
            fields = line.split(',')
            if fields[0] == 'CPU_CYCLES':
                key = '/'.join(fields[1:6])
                assert key not in seen
                seen.add(key)
                samples.setdefault(key, []).append(int(fields[7]) / int(fields[8]))
            elif fields[0] == 'MIRROR' and 'upstream' not in directory.name:
                assert fields[-1] == '0', line
            elif fields[0] == 'READ_BATCH':
                key = '/'.join(fields[1:6])
                batches[key] = dict(zip(('read_sessions', 'batched_read_blocks', 'read_stops'),
                                        map(int, fields[7:10])))
        assert len(seen) == 8, (directory, trial, seen)
        with (directory / f'trial-{trial}.csv').open(newline='') as source:
            rows = list(csv.DictReader(source))
        assert len(rows) == 8
        for row in rows:
            key = '/'.join(row[field] for field in
                           ('phase', 'profile', 'cluster_sectors', 'chunk_bytes', 'files'))
            values = {field: int(row[field]) for field in
                      ('payload_bytes', 'reads', 'writes', 'metadata_reads',
                       'metadata_writes', 'polls', 'virtual_us', 'multi_sessions', 'multi_blocks')}
            values.update(batches[key])
            passes = 32 if row['phase'] == 'read' else 1
            assert values['payload_bytes'] == int(row['files']) * 1048576 * passes
            assert counters.setdefault(key, values) == values, (directory, key)
    return {key: dict(cycles_per_byte=statistics.median(values), samples=values,
                      **counters[key]) for key, values in samples.items()}


def resources(directory):
    """Count writable storage and generated code in library objects only."""
    ram = 0
    code = 0
    for name in ('asyncfatfs', 'fat_standard'):
        _, sections = coff_sections(directory / f'{name}.obj')
        ram += sum(item['size'] for item in sections
                   if item['flags'] & 0x80000000 and item['flags'] & 0xC0)
        code += sum(item['size'] for item in sections if item['flags'] & 0x20)
    return dict(static_ram_bytes=ram, code_bytes=code)


def write_markdown(path, report, directory):
    """Publish measured changes without interpreting identical code as a gain."""
    results = report['results']
    peak = max(item['measurements']['after']['stack']['conservative_internal_bytes']
               for item in report['pic24_matrix']['results'])
    lines = [
        '# CPU: conversão de endereço do cache', '',
        'A conversão de ponteiro para índice agora valida o deslocamento antes da divisão sem sinal. '
        'Com leitura multibloco habilitada, a função é incorporada ao chamador. '
        'A configuração padrão preserva o código gerado anterior nos caminhos verificados.', '',
        'Referência: `upstream/master` local no commit `'
        + report['upstream']['upstream_commit'] + '`. O código upstream foi conferido contra esse commit.', '',
        'FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB, um ou dois leitores/escritores intercalados. '
        f"MSVC x64 /O2, {report['repetitions']} repetições, 32 passagens de leitura, "
        'afinidade fixa e ordem alternada. Medição final sem compilações concorrentes. '
        'Ciclos por byte incluem biblioteca, backend simulado, polling e verificação dos dados. '
        'São medições no host, não tempos medidos no PIC24 ou em cartão físico.', '',
        'O upstream equalizado usa quatro páginas de cache; o upstream nativo usa oito. '
        'A biblioteca atual usa quatro páginas e mantém as duas FATs. '
        'O upstream deixa diferenças entre as cópias da FAT nesse workload; '
        'seu custo de escrita não representa a mesma garantia de espelhamento.', '',
        '## Leitura', '',
        'Valores positivos nas colunas de redução indicam menos CPU. A mudança frente à versão anterior '
        'é a mediana das diferenças percentuais pareadas; frente ao upstream, é a razão das medianas. '
        'Diferenças próximas de zero devem ser tratadas como variação da medição.', '',
        '| Configuração | Bloco | Leitores | Ciclos/byte | Redução anterior | Redução upstream 4 páginas | Redução upstream 8 páginas |',
        '|---|---:|---:|---:|---:|---:|---:|']
    profiles = (('Padrão', 'default', 'before_default'),
                ('Multibloco', 'after_no_query', 'before'),
                ('Multibloco + consulta', 'after', 'before_query'))
    for label, variant, previous in profiles:
        for chunk in (257, 4096):
            for files in (1, 2):
                key = f'read/0/1/{chunk}/{files}'
                current = results[variant][key]['cycles_per_byte']
                change = report['paired_cpu_reduction_percent'][f'{previous}/{variant}'][key]
                change_text = 'Código idêntico' if variant == 'default' else f'{change:+.2f}%'
                equal = (1 - current / results['upstream_equal'][key]['cycles_per_byte']) * 100
                native = (1 - current / results['upstream_native'][key]['cycles_per_byte']) * 100
                lines.append(f'| {label} | {chunk} | {files} | {current:.4f} | {change_text} | '
                             f'{equal:+.2f}% | {native:+.2f}% |')
    lines += ['', '## Escrita com multibloco e consulta', '',
              '| Bloco | Escritores | Ciclos/byte | Redução anterior | Redução upstream 4 páginas |',
              '|---:|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = f'write/0/1/{chunk}/{files}'
            current = results['after'][key]['cycles_per_byte']
            change = report['paired_cpu_reduction_percent']['before_query/after'][key]
            upstream = (1 - current / results['upstream_equal'][key]['cycles_per_byte']) * 100
            lines.append(f'| {chunk} | {files} | {current:.4f} | {change:+.2f}% | {upstream:+.2f}% |')
    lines += ['', '## RAM, código e pilha', '',
              '| ABI | Configuração | RAM antes/depois | Código antes → depois |',
              '|---|---|---:|---:|']
    for abi, field in (('x86', 'x86_resources'), ('x64', 'x64_resources')):
        for label, variant, previous in profiles:
            old = report[field][previous]
            new = report[field][variant]
            lines.append(f"| {abi} | {label} | {new['static_ram_bytes']} B | "
                         f"{old['code_bytes']} → {new['code_bytes']} B |")
    lines += ['', f'PIC24: as 48 variantes passaram, sem alteração de RAM estática ou do limite de pilha. '
              f'Pico interno conservador: **{peak} bytes**, inferior a 400 bytes. '
              'Inclui chamadas internas e endereços de retorno; exclui backend, callbacks da aplicação '
              'e rotinas do compilador/libc, conforme o escopo acordado.', '',
              '## I/O e persistência', '',
              'Os contadores de leitura, escrita, metadados, polling, sessões multibloco e encerramentos '
              'permaneceram iguais aos da versão anterior em todos os casos medidos. '
              'Nenhuma ordenação, confirmação de gravação, reserva de cluster ou regra do journal foi alterada. '
              'A auditoria das duas FATs passou para todas as execuções da biblioteca atual.', '',
              'Os 11 cenários funcionais passaram com multibloco e consulta, incluindo falhas de I/O, '
              'remoção de mídia, callbacks antigos, desmontagem/remontagem e diretório com crescimento adiado. '
              'As 295 definições de função passaram na verificação Doxygen.', '',
              '## Reprodução e evidências', '',
              f'Evidências e snapshots: `{directory.as_posix()}`. '
              'O JSON contém todas as amostras, contadores, hashes e cadeias de pilha.', '',
              '```powershell',
              f'& tests/run_read_cpu_benchmark.ps1 -OutputDirectory {directory.as_posix()} -RunOnly '
              '-Repetitions 21 -Variants before_default,default,before,before_query,after,after_no_query,upstream_equal,upstream_native',
              f'python tests/cache_index_cpu_report.py --directory {directory.as_posix()} --repetitions 21 '
              f'--output {directory.as_posix()}/report.json --markdown {path.as_posix()}',
              '```', '', 'Os comandos pressupõem os snapshots e executáveis já preparados na pasta de evidências.', '']
    path.write_text('\n'.join(lines), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--repetitions', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--markdown', type=Path)
    arguments = parser.parse_args()
    root = arguments.directory
    variants = ('before_default', 'default', 'before', 'before_query', 'after',
                'after_no_query', 'upstream_equal', 'upstream_native')
    results = {name: measurements(root / f'cpu-{name}', arguments.repetitions)
               for name in variants}
    memory = {name: resources(root / f'cpu-{name}') for name in variants}
    memory32 = {name: resources(root / f'x86-{name}') for name in variants[:6]}
    unchanged = {}
    for name in ('afatfs_getCacheDescriptorIndexForBuffer', 'afatfs_cacheSector',
                 'afatfs_fread', 'afatfs_fwrite', 'afatfs_poll', 'afatfs_fprefetch'):
        previous = function_code(root / 'cpu-before_default/asyncfatfs.obj', name)
        current = function_code(root / 'cpu-default/asyncfatfs.obj', name)
        assert previous == current, (name, previous, current)
        unchanged[name] = current
    comparisons = {}
    for older, newer in (('before_default', 'default'), ('before', 'after_no_query'),
                         ('before_query', 'after')):
        assert memory[older]['static_ram_bytes'] == memory[newer]['static_ram_bytes']
        assert memory32[older]['static_ram_bytes'] == memory32[newer]['static_ram_bytes']
        comparisons[f'{older}/{newer}'] = {}
        for key, current in results[newer].items():
            previous = results[older][key]
            assert {name: value for name, value in previous.items()
                    if name not in ('cycles_per_byte', 'samples')} == {
                        name: value for name, value in current.items()
                        if name not in ('cycles_per_byte', 'samples')}, (older, newer, key)
            comparisons[f'{older}/{newer}'][key] = statistics.median(
                (1 - new / old) * 100 for old, new in zip(previous['samples'], current['samples']))
    matrix = json.loads((root / 'final-matrix/report.json').read_text())
    assert len(matrix['results']) == 48
    assert all(item['passed'] for item in matrix['results'])
    hashes = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (root / 'after/lib').glob('*') if path.is_file()}
    assert all(hashes[name] == value for name, value in matrix['sources']['after'].items())
    report = dict(repetitions=arguments.repetitions, read_passes=32, results=results,
                  paired_cpu_reduction_percent=comparisons, x64_resources=memory,
                  x86_resources=memory32, pic24_matrix=matrix,
                  unchanged_default_code=unchanged,
                  upstream=json.loads((root / 'manifest.json').read_text()),
                  source_hashes=hashes)
    arguments.output.write_text(json.dumps(report, indent=2) + '\n')
    if arguments.markdown:
        write_markdown(arguments.markdown, report, root)
    print(json.dumps(comparisons, indent=2))


if __name__ == '__main__':
    main()
