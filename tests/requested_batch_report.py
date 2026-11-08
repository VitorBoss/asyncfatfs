"""Prepare and validate safe requested-allocation batching against prior and upstream sources."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

from metadata_write_audit import diagnostic_harness, instrument
from metadata_write_report import load
from mirror_profile_fix_report import digest
from cache_index_cpu_report import resources


VARIANTS = ('before', 'prototype', 'after', 'upstream')


def prepare(root):
    assert (root / 'before/lib').is_dir()
    shutil.copytree('lib', root / 'normal/lib', dirs_exist_ok=True)
    for variant, original in (('prototype', Path('tests/build/fat-batch/raw/requested_8/lib')),
                              ('after', root / 'normal/lib'),
                              ('upstream', Path('tests/build/reserve-flush/upstream/lib'))):
        shutil.copytree(original, root / variant / 'lib', dirs_exist_ok=True)
    shutil.copytree(root / 'before/lib', root / 'before-traced/lib', dirs_exist_ok=True)
    # Keep the baseline immutable; the runner's before directory receives its own traced copy.
    for variant in ('before', 'prototype', 'after'):
        original = root / ('before-traced/lib' if variant == 'before' else f'{variant}/lib')
        destination = root / f'audit/{variant}/lib'
        shutil.copytree(original, destination, dirs_exist_ok=True)
        (destination / 'asyncfatfs.c').write_text(instrument((original / 'asyncfatfs.c').read_text()))
    shutil.copytree(root / 'upstream/lib', root / 'audit/upstream/lib', dirs_exist_ok=True)
    harness = diagnostic_harness(Path('tests/upstream_comparison.c').read_text())
    (root / 'audit/harness.c').write_text(harness)
    shutil.copyfile('tests/build/poll-metadata/portability.h', root / 'audit/portability.h')
    three = root / 'three'
    for variant in ('before', 'after'):
        shutil.copytree(root / f'audit/{variant}/lib', three / variant / 'lib', dirs_exist_ok=True)
    (three / 'harness.c').write_text('#define COMPARISON_THREE_FILES\n' + harness)
    shutil.copyfile('tests/build/poll-metadata/portability.h', three / 'portability.h')
    work = root / 'matrix-work/tests'
    shutil.copytree('tests/config', work / 'config', dirs_exist_ok=True)
    (work / 'config_common.h').write_text(Path('tests/config_common.h').read_text().replace('AFATFS_NUM_CACHE_SECTORS 4', 'AFATFS_NUM_CACHE_SECTORS 8'))
    (root / 'manifest.json').write_text(json.dumps(dict(upstream=subprocess.check_output(
        ['git', 'rev-parse', 'upstream/master'], text=True).strip(),
        sources={version: {path.name: digest(path) for path in (root / f'{version}/lib').iterdir() if path.is_file()}
                 for version in ('before', 'normal', 'prototype', 'upstream')}), indent=2))


def publish(root, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['sources']['normal']['asyncfatfs.c']
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['sources']['normal']['asyncfatfs.c']
    for row in matrix['results']:
        assert row['measurements']['before']['static_sections'] == row['measurements']['after']['static_sections']
        assert row['measurements']['after']['stack']['conservative_internal_bytes'] < 400
    datasets = {}
    for schedule in ('serial', 'interleaved'):
        for version in VARIANTS:
            datasets[f'{schedule}-{version}'] = load(root / f'audit/{schedule}-{version}', version != 'upstream')
        for version in ('before', 'after'):
            datasets[f'three-{schedule}-{version}'] = load(root / f'three/{schedule}-{version}', True, 8)
    peak = max(row['measurements']['after']['stack']['conservative_internal_bytes'] for row in matrix['results'])
    footprints = {architecture: {version: resources(root / f'resources-{architecture}-{version}') for version in ('before', 'after')}
                  for architecture in ('32', '64')}
    for values in footprints.values():
        assert values['before']['static_ram_bytes'] == values['after']['static_ram_bytes']
    recovery = (root / 'recovery-default/results.log').read_text()
    assert 'recovery cuts, 8 retry scenarios' in recovery
    record = re.search(r'(\d+) write cuts, (\d+) recovery cuts', recovery)
    assert record
    for profile in ('multi', 'query'):
        assert (root / f'recovery-{profile}/results.log').read_text() == recovery
    report = dict(manifest=manifest, matrix=matrix, footprints=footprints, datasets=datasets,
                  recovery={'write_cuts': int(record[1]), 'recovery_cuts': int(record[2]), 'retry_cases': 8})
    lines = ['# Alocação de até oito clusters por pedido e recuperação de volume sujo', '',
             'Implementado no pipeline nativo FAT32 8.3, clusters de 512 bytes, cache oito setores. '
             'AFATFS_REQUESTED_BATCH_CLUSTERS=8 por padrão; um desativa lotes. Apenas clusters cobertos pelo pedido '
             'atual são alocados, limitados a uma página FAT e ao espaço livre contíguo. Reserva especulativa global '
             'continua única; o lote é propriedade do escritor, registrado no estado já existente.', '',
             '## Mitigações', '',
             '- O bit padrão de encerramento limpo do FAT32 é limpo e confirmado nas duas FATs antes dos EOCs do primeiro lote. '
             'A sincronização só publica o bit limpo depois de dados, vínculos e diretório confirmados, sem outro '
             'lote pendente. A preparação de novos lotes é bloqueada enquanto essa publicação está em andamento.',
             '- Um volume sujo passa por recuperação antes de ficar READY. As referências das duas FATs e os primeiros '
             'clusters de todos os arquivos/diretórios protegem alocações vivas. Só cabeças sem referência são liberadas; '
             'ambas as FATs são confirmadas antes de uma nova varredura poder liberar os sucessores.',
             '- A recuperação usa um setor do cache como mapa de 4096 clusters, liberado ao terminar, e o espaço da união '
             'de operações do diretório para os cursores. A árvore é percorrida sem recursão. Não há buffer estático novo.',
             '- Páginas em trânsito não podem ser alteradas; falhas de leitura/envio/ACK são repetidas. '
             'A FAT primária é a autoridade na reconciliação: alocações faltantes da cópia secundária são inicializadas '
             'antes da publicação dos vínculos. Entradas reservadas/bad e os nibbles altos são preservados.', '',
             '## Contagem de metadados', '', f"Upstream local: `{manifest['upstream']}`. Arquivos de 1 MiB; "
             'nenhuma medição de CPU nesta rodada. Valores são setores de 512 bytes, perfil de latência 0.', '',
             '| Ritmo | Escritores/bloco | Antes | Protótipo sem recuperação | Final mitigado | Upstream | Redução final/antes |',
             '|---|---|---:|---:|---:|---:|---:|']
    for schedule in ('serial', 'interleaved'):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/0/1/{chunk}/{files}'
            values = {version: datasets[f'{schedule}-{version}'][key]['counters']['metadata_writes'] for version in VARIANTS}
            lines.append(f"| {schedule} | {files}/{chunk} | {values['before']} | {values['prototype']} | {values['after']} | {values['upstream']} | {(1-values['after']/values['before'])*100:.2f}% |")
    lines += ['', 'O valor histórico 8259 →1985 é do protótipo anterior à melhoria de pares sujos. '
              'A coluna Antes usa a versão segura imediatamente anterior. O bit de volume sujo e sua confirmação '
              'acrescentam metadados; o número final é o medido acima, não uma promessa de manter exatamente 1985.', '',
              '## I/O total do caso intercalado 2/4096', '',
              '| Versão | Leituras | Escritas | FAT1 | FAT2 | Diretório |',
              '|---|---:|---:|---:|---:|---:|']
    for version in VARIANTS:
        entry = datasets[f'interleaved-{version}']['write/0/1/4096/2']
        counts = entry['counters']
        metadata = entry['metadata']
        lines.append(f"| {version} | {counts['reads']} | {counts['writes']} | {metadata['primary']} | {metadata['mirror']} | {metadata['directory']} |")
    lines += ['', 'As leituras aumentam porque os lotes e os dados competem pelas oito páginas de cache. '
              'O upstream não grava a segunda FAT neste workload, portanto não oferece a mesma garantia de espelhamento. '
              'A versão final também impede alterações em páginas em trânsito; essa mudança de escalonamento '
              'e a base posterior ao protótipo permitem obter menos gravações que o protótipo, apesar dos marcadores adicionais.', '',
              '## RAM e pilha', '', f'Máximo interno da matriz de 48 configurações: **{peak} bytes**. '
              f'Recursos dos objetos de biblioteca: `{json.dumps(footprints)}`.', '',
              '## Recuperação e limites', '',
              f"Passaram {record[1]} cortes nas gravações e {record[2]} cortes durante a recuperação, incluindo "
              'ambas as metades dos setores, mais oito cenários de falha de envio/leitura/ACK com retries. '
              'Arquivos confirmados na raiz e em subdiretório foram preservados; FATs iguais e ausência de '
              'alocações órfãs foram auditadas no volume sintético. A campanha passou nos três perfis de leitura '
              '(padrão, multibloco e multibloco com consulta), incluindo escritores mistos 257/4096 e novo lote '
              'enfileirado durante a confirmação do marcador limpo.', '',
              'A recuperação libera alocações sem dono, não tenta salvar seu conteúdo como arquivos perdidos. '
              'Cadeias ainda ligadas a arquivos são preservadas, incluindo espaço pré-alocado além do tamanho lógico. '
              'fwrite continua sendo aceitação em cache; fsync/fclose confirmam o tamanho real e os dados.', '',
              'Esta recuperação não substitui um fsck completo: não resolve ciclos órfãos ou corrupção arbitrária '
              'de diretórios/FAT. Estruturas inválidas interrompem a montagem em vez de liberar dados ambíguos. '
              'A varredura usa memória limitada e pode ser demorada em volumes grandes/fragmentados; somente volumes '
              'sujos a executam. Não foi medido tempo de recuperação em cartão físico.', '',
              'Evidências: tests/build/requested-batch-safe. Os snapshots e os contadores instrumentados pertencem ao '
              'harness e não ao orçamento de RAM/pilha da biblioteca.']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/requested-batch-safe'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('tests/REQUESTED_BATCH_RECOVERY.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    else:
        publish(arguments.directory, arguments.output)
