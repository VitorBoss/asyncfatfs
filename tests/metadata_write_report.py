"""Summarize untimed metadata submissions and isolated allocation diagnostics."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from metadata_write_audit import VARIANTS
from mirror_profile_fix_report import digest


def load(directory, traced, expected_rows=16, allow_data_rewrites=False):
    with (directory / 'results.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = {'/'.join(row[field] for field in ('phase', 'profile', 'cluster_sectors', 'chunk_bytes', 'files')):
                {name: int(value) for name, value in row.items() if name not in ('phase', 'host_seconds')}
                for row in csv.DictReader(stream)}
    assert len(rows) == expected_rows
    result = {key: dict(counters=row, metadata={}, histogram={'0': {}, '1': {}},
                        origins={}, directory_origins={}, directory_aged_by_origin={},
                        directory_examples={}) for key, row in rows.items()}
    pending = []
    for line in (directory / 'events.log').read_text().splitlines():
        parts = line.split(',')
        if parts[0] == 'CASE':
            assert not pending
        elif parts[0] == 'MSUB':
            assert traced
            values = list(map(int, parts[1:10]))
            event = dict(zip(('kind', 'sector', 'background', 'excluded', 'pressure', 'age', 'owners', 'eligible', 'phases'), values))
            event['files'] = [dict(operation=parts[index], phase=int(parts[index + 1]),
                                   offset=int(parts[index + 2]), physical_size=int(parts[index + 3]))
                              for index in (10, 14, 18)]
            pending.append(event)
        elif parts[0] == 'META':
            key = '/'.join(parts[1:6])
            record = result[key]
            record['metadata'] = dict(zip(('primary', 'mirror', 'directory'), map(int, parts[7:])))
            if traced:
                counts = Counter(event['kind'] for event in pending)
                assert [counts[index] for index in (1, 2, 3)] == list(record['metadata'].values()), (directory, key, counts)
                origins = defaultdict(Counter)
                directory_origins = Counter()
                aged = Counter()
                examples = defaultdict(list)
                for event in pending:
                    if event['pressure']:
                        reason = 'cache_pressure_background' if event['background'] else 'cache_pressure_full'
                    elif event['background']:
                        reason = 'background'
                    elif event['excluded']:
                        reason = 'allocation_barrier'
                    else:
                        reason = 'unfiltered'
                    origins[str(event['kind'])][reason] += 1
                    if event['kind'] == 3:
                        assert event['sector'] != event['excluded']
                        if event['background']:
                            assert event['age'] >= 64 or not event['eligible']
                            detail = 'background_age' if event['age'] >= 64 else 'background_no_eligible_owner'
                        elif event['pressure']:
                            detail = 'cache_pressure'
                        elif any(file['operation'] in ('CLOSE', 'SYNC') for file in event['files']):
                            detail = 'close_or_sync'
                        elif any(file['operation'] == 'APPEND' and file['phase'] == 0 for file in event['files']):
                            detail = 'unfiltered_while_finding_reserved_cluster'
                        else:
                            detail = 'other_unfiltered'
                        directory_origins[detail] += 1
                        aged[detail] += event['age'] >= 64
                        if len(examples[detail]) < 3:
                            examples[detail].append(event)
                record['origins'] = {kind: dict(values) for kind, values in origins.items()}
                record['directory_origins'] = dict(directory_origins)
                record['directory_aged_by_origin'] = dict(aged)
                record['directory_examples'] = dict(examples)
            pending = []
        elif parts[0] == 'FAT_HIST':
            key = '/'.join(parts[1:6])
            copy_index, changed, count = map(int, parts[7:])
            assert str(changed) not in result[key]['histogram'][str(copy_index)]
            result[key]['histogram'][str(copy_index)][str(changed)] = count
        elif parts[0] == 'MIRROR' and traced:
            assert parts[-1] == '0'
    assert not pending
    for key, record in result.items():
        row = record['counters']
        assert sum(record['metadata'].values()) == row['metadata_writes']
        for copy_index, label in (('0', 'primary'), ('1', 'mirror')):
            histogram = record['histogram'][copy_index]
            assert sum(histogram.values()) == record['metadata'][label]
        if key.startswith('write/'):
            data_writes = row['writes'] - row['metadata_writes']
            minimum_data_writes = row['payload_bytes'] // 512
            if allow_data_rewrites:
                assert data_writes >= minimum_data_writes
                record['extra_data_writes'] = data_writes - minimum_data_writes
            else:
                assert data_writes == minimum_data_writes
    return result


def case(profile, chunk, files):
    return f'write/{profile}/1/{chunk}/{files}'


def mean_changes(record):
    histogram = record['histogram']['0']
    return sum(int(changed) * count for changed, count in histogram.items()) / sum(histogram.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/metadata-write-audit'))
    parser.add_argument('--output', type=Path, default=Path('tests/METADATA_WRITE_AUDIT.md'))
    args = parser.parse_args()
    root = args.directory
    manifest = json.loads((root / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['base_sha256']
    previous = json.loads(Path('tests/IO_CPU_RELATION.json').read_text())
    data = {schedule: {variant: load(root / f'{schedule}-{variant}', variant != 'upstream')
                       for variant in VARIANTS} for schedule in ('serial', 'interleaved')}
    for profile in (0, 1):
        record = data['interleaved']['base'][case(profile, 4096, 2)]
        assert record['directory_origins']['unfiltered_while_finding_reserved_cluster'] == 288
        assert record['directory_aged_by_origin']['unfiltered_while_finding_reserved_cluster'] == 0
    for schedule, prior in (('serial', 'serial'), ('interleaved', 'prepared')):
        for variant, version in (('base', 'current'), ('upstream', 'upstream')):
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = case(0, chunk, files)
                    row = data[schedule][variant][key]['counters']
                    expected = previous['experiments'][prior]['audit'][version][key]
                    for field in ('reads', 'writes', 'metadata_reads', 'metadata_writes', 'polls', 'virtual_us'):
                        assert row[field] == expected[field], (schedule, variant, key, field)
    ratios = []
    lines = ['# Auditoria do excesso de gravação de metadados', '',
             'FAT32 8.3, clusters de 512 bytes, cache de oito setores, API original, '
             'LFN/exFAT/journal desligados. Base = versão atual de WRITE_DISPATCH_CPU. '
             'Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. '
             'Esta investigação não modifica a biblioteca de produção.', '',
             '## Quantificação conferida', '',
             'O excesso é real em todos os cenários abaixo. Metadados contam '
             'transferências de 512 bytes para FAT primária, espelho e diretório. '
             'Perfil 0 usa uma conclusão por próximo poll; perfil 1 usa latências '
             'simuladas diferenciadas de leitura/escrita. São contagens, não timings '
             'de CPU ou medições de comandos físicos do cartão.', '',
             'Aumento = `(atual / upstream − 1) × 100`. Por exemplo, 557,76% **do** '
             'upstream significa 457,76% **a mais**, não 557,76% a mais.', '',
             '| Ritmo | Perfil | Bloco | Escritores | Metadados atual/upstream | Aumento |',
             '|---|---:|---:|---:|---:|---:|']
    for schedule in data:
        for profile in (0, 1):
            for chunk in (257, 4096):
                for files in (1, 2):
                    key = case(profile, chunk, files)
                    current = data[schedule]['base'][key]['counters']['metadata_writes']
                    upstream = data[schedule]['upstream'][key]['counters']['metadata_writes']
                    extra = (current / upstream - 1) * 100
                    ratios.append(extra)
                    lines.append(f'| {schedule} | {profile} | {chunk} | {files} | {current} / {upstream} | +{extra:.2f}% |')
    lines += ['', f'Nesta fonte/cache, a faixa conferida é **+{min(ratios):.2f}% a '
              f'+{max(ratios):.2f}%**. Faixas de relatórios anteriores podem usar '
              'outro cache/perfil; não misturar percentual do baseline com aumento.', '',
              '## Causa dominante: granularidade da confirmação FAT', '',
              'No serial, perfil 0:', '',
              '| Bloco | Escritores | FAT primária atual/up | Espelho atual/up | Diretório atual/up | Entradas diferentes por imagem primária atual/up |',
              '|---|---:|---:|---:|---:|---:|']
    for chunk in (257, 4096):
        for files in (1, 2):
            key = case(0, chunk, files)
            current = data['serial']['base'][key]
            upstream = data['serial']['upstream'][key]
            pairs = [f"{current['metadata'][field]} / {upstream['metadata'][field]}"
                     for field in ('primary', 'mirror', 'directory')]
            lines.append(f'| {chunk} | {files} | ' + ' | '.join(pairs) +
                         f' | {mean_changes(current):.3f} / {mean_changes(upstream):.3f} |')
    lines += ['', 'A auditoria compara cada imagem FAT com a armazenada no instante '
              'da conclusão. A atual muda aproximadamente duas entradas por imagem, '
              'e grava cada cópia aproximadamente uma vez por cluster. O upstream '
              'acumula mais entradas alteradas por imagem, especialmente com pedidos '
              'de 4096 bytes. Ele também não mantém o espelho nesse workload.', '',
              'Na atual, a sequência é: preparar EOC → confirmar ambas as FATs → '
              'ligar a cadeia → confirmar ambas → atualizar diretório. A reserva '
              'única adianta um EOC, compartilhando sua imagem com o vínculo '
              'anterior. Isso economiza uma rodada, mas ainda mantém a granularidade '
              'de um cluster na sequência serial. Há também imagens intermediárias '
              'EOC que ficam duráveis antes de serem substituídas pelo vínculo; '
              'o upstream pode coalescer essas alterações sem a mesma barreira.', '',
              'Isso explica por que a diferença não é apenas 2× por haver duas FATs. '
              'O grande excesso de 4096 bytes está sobretudo na primária gravada '
              'mais frequentemente, somada ao espelho. O diretório serial já '
              'está muito reduzido; otimizá-lo sozinho não elimina a diferença.', '',
              '## Gravações evitáveis encontradas na espera da reserva', '',
              'Para dois escritores/4096 intercalados, a base faz 291 gravações '
              'do diretório. O rastreamento encontrou 288 envios genéricos enquanto '
              'um escritor estava em FIND_FREESPACE, aguardando a FAT da reserva. '
              'As imagens eram recentes, com idade inferior a 64; não foi o '
              'timeout de adiamento do background nem a pressão de cache que '
              'provocou esses envios.', '',
              'O ponto é o for que espera as páginas FAT da reserva ficarem '
              'IN_SYNC em appendRegularFreeClusterContinue. Ele chama flushCache '
              'sem exclusão e pode selecionar o diretório antigo antes da FAT. '
              'As demais barreiras de alocação já usam flushAllocationCache para '
              'drenar FAT/dados deixando essa entrada antiga fora da submissão.', '',
              'A cópia diagnóstica reserve_wait troca somente essa chamada pelo '
              'filtro de alocação existente. Não altera a checagem de confirmação '
              'das duas FATs antes de consumir a reserva. Não foi aplicada à '
              'produção nesta auditoria.', '',
              '| Ritmo | Bloco | Escritores | Metadados base → diagnóstico | Diretório base → diagnóstico | FAT1/FAT2 base → diagnóstico |',
              '|---|---:|---:|---:|---:|---:|']
    for schedule in data:
        for chunk in (257, 4096):
            for files in (1, 2):
                key = case(0, chunk, files)
                base = data[schedule]['base'][key]
                fixed = data[schedule]['reserve_wait'][key]
                assert base['metadata']['primary'] == fixed['metadata']['primary']
                assert base['metadata']['mirror'] == fixed['metadata']['mirror']
                assert fixed['counters']['metadata_writes'] <= base['counters']['metadata_writes']
                lines.append(f"| {schedule} | {chunk} | {files} | {base['counters']['metadata_writes']} → "
                             f"{fixed['counters']['metadata_writes']} | {base['metadata']['directory']} → "
                             f"{fixed['metadata']['directory']} | {base['metadata']['primary']}/{base['metadata']['mirror']} → "
                             f"{fixed['metadata']['primary']}/{fixed['metadata']['mirror']} |")
    for schedule in data:
        for variant in ('base', 'reserve_wait'):
            for chunk in (257, 4096):
                for files in (1, 2):
                    assert data[schedule][variant][case(0, chunk, files)]['metadata'] == data[schedule][variant][case(1, chunk, files)]['metadata']
    lines += ['', 'O perfil 1 reproduziu as mesmas contagens de metadados da base e desse '
              'diagnóstico. No intercalado 2/4096: **8547 → 8259 metadados '
              '(−3,37%)**, diretório **291 → 3**, FAT1/FAT2 inalteradas em '
              '4128/4128. O excesso dominante da FAT continua existindo após '
              'eliminar esses 288 envios; não é uma solução para todo o intervalo.', '',
              '## Controles isolados', '',
              '| Variante | Serial 1/4096 | Intercalado 2/257 | Intercalado 2/4096 |',
              '|---|---:|---:|---:|']
    for variant in ('base', 'no_reservation', 'no_group', 'data_pressure', 'reserve_wait'):
        values = [data[schedule][variant][case(0, chunk, files)]['counters']['metadata_writes']
                  for schedule, chunk, files in (('serial', 4096, 1), ('interleaved', 257, 2), ('interleaved', 4096, 2))]
        lines.append(f'| {variant} | ' + ' | '.join(map(str, values)) + ' |')
    lines += ['', '- Sem reserva: a escrita serial passa de 4133 para 8193 metadados. '
              'A reserva já evita quase metade desse custo; removê-la globalmente '
              'é contraproducente.',
              '- Sem agrupamento: melhora um caso de 4096 ao evitar o envio '
              'intermediário do diretório, mas piora 2/257. Corrigir a espera '
              'da reserva preserva o agrupamento e supera esse controle.',
              '- Flush de background também para pressão de dados: não mudou '
              'as contagens. O excesso de diretório rastreado vem da espera '
              'da reserva, não desse fallback.', '',
              '## Interpretação e próximo alvo técnico', '',
              'Há uma ineficiência concreta de flush do diretório e um custo '
              'maior, estrutural ao protocolo atual: confirmação FAT em granularidade '
              'de cluster. Nenhuma imagem FAT atual já idêntica foi regravada nos '
              'workloads base auditados anteriormente. Portanto o alvo principal '
              'é compartilhar imagens de alocações já solicitadas dentro da '
              'mesma fase de durabilidade, preservando EOC confirmado antes do '
              'vínculo e vínculo confirmado antes da publicação. Isso exige '
              'investigar a granularidade da máquina de estados; não se resolve '
              'apenas aumentando o cache ou reduzindo memcpy.', '',
              'O diagnóstico reserve_wait tem integridade/ordenação verificadas '
              'nos workloads abaixo, mas ainda não recebeu avaliação completa '
              'de CPU, pilha, fragmentação, pressão extrema e falhas necessária '
              'para promovê-lo à produção. Não usar os timings desses binários '
              'instrumentados para afirmar ganho de desempenho.', '',
              '## Validação e reprodução', '',
              'Foram executados os dois ritmos, dois perfis de latência, um/dois '
              'escritores e 257/4096, nas seis variantes: **96 cenários de escrita**, '
              'com releitura integral, auditoria de conteúdo e, nas variantes atuais, '
              'igualdade das FATs e checagens por gravação de EOC/vínculo/diretório. '
              'As bases de perfil 0 reproduziram os contadores de IO_CPU_RELATION. '
              'Instrumentação e alterações experimentais ficam em cópias congeladas '
              'em tests/build; produção, RAM, cache e pilha da biblioteca não mudaram.', '',
              'MSUB registra a origem/contexto da submissão; FAT_HIST registra '
              'quantas entradas de quatro bytes diferiam na conclusão. As '
              'contagens de submissões conferem com as de conclusão. Não houve '
              'injeção de falhas nesses diagnósticos; os checks são do workload '
              'bem-sucedido e não substituem uma campanha de corte de energia.', '',
              f'Evidências: `{root.as_posix()}`. JSON ao lado inclui contadores, '
              'origens agregadas, exemplos de diretório e histogramas completos. '
              'manifest.json registra a fonte base, inalterada em produção.', '',
              '```powershell',
              f'python tests/metadata_write_audit.py --directory {root.as_posix()}',
              f'./tests/run_metadata_write_audit.ps1 -OutputDirectory {root.as_posix()}',
              f'python tests/metadata_write_report.py --directory {root.as_posix()} --output {args.output.as_posix()}',
              '```', '']
    result = dict(manifest=manifest, experiments=data, excess_percent_range=[min(ratios), max(ratios)])
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
