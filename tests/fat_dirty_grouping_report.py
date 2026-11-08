"""Validate the adopted dirty-pair grouping against frozen before/after evidence."""
import argparse
import json
from pathlib import Path
import shutil

from cache_index_cpu_report import resources
from metadata_write_report import load
from mirror_profile_fix_report import digest
from poll_metadata_audit import trace_poll


TESTED_BRIEF = '/** @brief Share only an entirely dirty FAT pair, without redirtying an already acknowledged copy. */'
FINAL_DOC = '''/**
 * @brief Test whether both FAT copies can share another pending allocation before submission.
 * @param sector Relative FAT sector containing the unique look-ahead reservation.
 * @return True when both cached copies are dirty; false if either copy is missing or in another state.
 */'''
OLD_FAILURE = '''                case AFATFS_FIND_CLUSTER_FATAL:
                case AFATFS_FIND_CLUSTER_NOT_FOUND:'''
FINAL_FAILURE = '''                case AFATFS_FIND_CLUSTER_NOT_FOUND:
#ifdef AFATFS_PIPELINE_ALLOCATION
                    if (afatfs.lastClusterAllocated & AFATFS_RESERVED_CLUSTER) {
                        // The speculative reservation is still available after its ACKs.
                        // A failed fresh-peer search must not turn that space into ENOSPC.
                        opState->searchCluster = afatfs.lastClusterAllocated & AFATFS_CLUSTER_HINT_MASK;
                        afatfs_flushAllocationCache(file);
                        return AFATFS_OPERATION_IN_PROGRESS;
                    }
#endif
                    // Fall through
                case AFATFS_FIND_CLUSTER_FATAL:'''


def prepare_final(root):
    """Prepare final-source I/O and immutable-buffer controls, preserving prior evidence."""
    source = Path('lib/asyncfatfs.c').read_text()
    traced = trace_poll(source)
    for name, snapshot, three in (('adopted', False, False), ('adopted-three', False, True),
                                   ('adopted-snapshot', True, False), ('adopted-snapshot-three', True, True)):
        directory = root / name
        library = directory / 'fresh_pair_dirty/lib'
        shutil.copytree('lib', library, dirs_exist_ok=True)
        (library / 'asyncfatfs.c').write_text(traced)
        original = root / 'snapshot/harness.c' if snapshot else root / 'harness.c'
        (directory / 'harness.c').write_text(('#define COMPARISON_THREE_FILES\n' if three else '') + original.read_text())
        shutil.copyfile(root / 'portability.h', directory / 'portability.h')
    (root / 'final-manifest.json').write_text(json.dumps(dict(production=digest(Path('lib/asyncfatfs.c')),
        sources={path.name: digest(path) for path in Path('lib').iterdir() if path.is_file()},
        traced_source=digest(root / 'adopted/fresh_pair_dirty/lib/asyncfatfs.c')), indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/fat-batch'))
    parser.add_argument('--output', type=Path, default=Path('tests/FAT_DIRTY_GROUPING.md'))
    parser.add_argument('--prepare', action='store_true')
    args = parser.parse_args()
    root = args.directory
    if args.prepare:
        prepare_final(root)
        return
    production = Path('lib/asyncfatfs.c')
    before = root / 'raw/base/lib'
    tested = root / 'raw/fresh_pair_dirty/lib'
    normal = root / 'normal/lib'
    assert production.read_bytes() == (normal / production.name).read_bytes()
    assert production.read_text().count(FINAL_DOC) == 1
    expected = (tested / production.name).read_text().replace(TESTED_BRIEF, FINAL_DOC).replace(OLD_FAILURE, FINAL_FAILURE)
    assert production.read_text() == expected
    for path in before.iterdir():
        if path.is_file() and path.name != production.name:
            assert digest(path) == digest(tested / path.name) == digest(normal / path.name) == digest(Path('lib') / path.name), path.name
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in (normal / 'asyncfatfs_config.h').read_text()
    matrix = json.loads((root / 'matrix-production/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after'][production.name] == digest(production)
    assert matrix['sources']['before'][production.name] == digest(before / production.name)
    peak = max(row['measurements']['after']['stack']['conservative_internal_bytes'] for row in matrix['results'])
    for row in matrix['results']:
        assert row['measurements']['before']['static_sections'] == row['measurements']['after']['static_sections']
        assert row['measurements']['after']['stack']['conservative_internal_bytes'] < 400
    footprints = {architecture: {version: resources(root / (f'resources-{architecture}-base' if version == 'before' else f'final-resources-{architecture}-after'))
                                 for version, variant in (('before', 'base'), ('after', 'fresh_pair_dirty'))}
                  for architecture in ('32', '64')}
    for versions in footprints.values():
        assert versions['before']['static_ram_bytes'] == versions['after']['static_ram_bytes']
    selected = {}
    checks = {}
    for files, prefix, expected_rows in (('one_two', '', 16), ('three', 'three/', 8)):
        for schedule in ('serial', 'interleaved'):
            group = f'{files}/{schedule}'
            final_prefix = 'adopted' if files == 'one_two' else 'adopted-three'
            data = {version: load(root / (f'{prefix}{schedule}-{variant}' if version == 'before' else f'{final_prefix}/{schedule}-{variant}'), True, expected_rows)
                    for version, variant in (('before', 'base'), ('after', 'fresh_pair_dirty'))}
            selected[group] = data
            snapshot = 'snapshot' if files == 'one_two' else 'snapshot-three'
            for version, variant in (('before', 'base'), ('after', 'fresh_pair_dirty')):
                final_snapshot = 'adopted-snapshot' if files == 'one_two' else 'adopted-snapshot-three'
                directory = root / f'{snapshot if version == "before" else final_snapshot}/{schedule}-{variant}'
                captured = load(directory, True, expected_rows)
                mutations = []
                for line in (directory / 'events.log').read_text().splitlines():
                    parts = line.split(',')
                    if parts[0] == 'INFLIGHT_MUTATIONS':
                        mutations.extend(map(int, parts[7:]))
                assert len(mutations) == expected_rows * 4
                assert not any(mutations), (group, version)
                for key, row in data[version].items():
                    assert row['metadata'] == captured[key]['metadata']
                    assert row['counters'] == captured[key]['counters']
            for key, row in data['after'].items():
                old = data['before'][key]
                assert row['counters']['metadata_writes'] <= old['counters']['metadata_writes']
                if key.startswith('write/'):
                    assert row['metadata']['primary'] == row['metadata']['mirror']
                    assert row['metadata']['directory'] == old['metadata']['directory']
    for profile in ('default', 'multi', 'query'):
        log = root / f'production-targeted/{profile}/results.log'
        assert 'unique reserve, owned EOCs, both ACK barriers, immutable I/O, cleanup and remount passed.' in log.read_text()
        checks[f'guards_ack_remount_{profile}'] = digest(log)
    allocation = root / 'production-allocation.log'
    assert allocation.read_text().count(': passed.') == 18
    checks['production_allocation'] = digest(allocation)
    general = root / 'fresh-general.log'
    assert general.read_text().count('global idle and clean power-off status: passed') == 7
    checks['functional_default_optional'] = digest(general)
    for variant in ('base', 'fresh_pair_dirty'):
        for files in (2, 3):
            log = root / f'bounds/{variant}/files-{files}/results.log'
            assert f'{files} pending writers plus one unique reserve = {files + 1} durable unlinked EOCs' in log.read_text()
            checks[f'ownership_bound_{variant}_{files}'] = digest(log)
    report = dict(sources={'before': digest(before / production.name), 'tested': digest(tested / production.name),
                           'production': digest(production)}, cache_sectors=8, matrix=matrix,
                  footprints=footprints, datasets=selected, checks=checks)
    lines = ['# Agrupamento de alocações com as duas páginas FAT ainda sujas', '',
             'Implementado em lib/asyncfatfs.c, no pipeline nativo FAT32 8.3. Cache **oito setores**, clusters 512 bytes, arquivos de 1 MiB por escritor. A fonte final incorpora a candidata, sua documentação e o fallback ao esgotar a busca alternativa; o I/O e a matriz foram novamente verificados nessa fonte final.', '',
             'Quando uma reserva ainda suja bloqueia um escritor com pedido já pendente, a biblioteca preserva a reserva global e procura um cluster livre para esse pedido. Isso só ocorre com outro escritor pendente, sem flush em andamento, e com **ambas as cópias da página FAT em DIRTY**. O novo EOC compartilha o par ainda não enviado e continua passando por NEW_CLUSTER_DURABLE antes do vínculo; FAT_DURABLE continua protegendo a publicação do diretório.', '',
             'Se uma cópia já está IN_SYNC, ausente, em leitura ou em trânsito, a biblioteca mantém a espera anterior. Não amplia a reserva especulativa: permanece um cluster global, e cada escritor conserva um único alvo de alocação pendente. Nenhum campo/array/buffer foi acrescentado à biblioteca.', '',
             'Quando a busca alternativa não encontra espaço após o hint, ela volta a aguardar a reserva existente em vez de reportar ENOSPC. Falhas fatais de leitura mantêm o tratamento anterior. O teste específico cobre esse limite no fim da FAT, seguido de escrita e remontagem.', '',
             '## Metadados: antes → depois', '',
             'Resultados dos dois perfis de latência são idênticos para a biblioteca atual. Valores abaixo são setores de 512 bytes, na latência 0; as duas latências estão no JSON. Serial termina cada pedido antes de tentar o próximo; intercalado atende os escritores antes de avançar o backend.', '',
             '| Ritmo | Escritores/bloco | FAT1 antes → depois | FAT2 antes → depois | Diretório | Metadados antes → depois | Redução |',
             '|---|---|---:|---:|---:|---:|---:|']
    for group, data in selected.items():
        schedule = group.split('/')[1]
        for key, row in data['after'].items():
            phase, profile, cluster, chunk, files = key.split('/')
            if phase != 'write' or profile != '0':
                continue
            old = data['before'][key]
            first, second = old['metadata'], row['metadata']
            total_old, total_new = old['counters']['metadata_writes'], row['counters']['metadata_writes']
            lines.append(f"| {schedule} | {files}/{chunk} | {first['primary']} → {second['primary']} | {first['mirror']} → {second['mirror']} | {second['directory']} | {total_old} → {total_new} | {(1-total_new/total_old)*100:.3f}% |")
    lines += ['', 'Os ganhos medidos ficam nos pedidos intercalados: **2/4096 −0,799%**, **3/257 −0,793%**, **3/4096 −0,878%** dos metadados. Os demais casos mantêm a contagem. A diferença grande para upstream ainda permanece; esta alteração elimina separações evitáveis de imagens, sem retirar os pontos de confirmação.', '',
              '## RAM e pilha', '',
              '| ABI | RAM antes → depois |', '|---|---:|']
    for architecture in ('32', '64'):
        row = footprints[architecture]
        label = 'x86 (32 bits)' if architecture == '32' else 'x64'
        lines.append(f"| {label} | {row['before']['static_ram_bytes']} → {row['after']['static_ram_bytes']} B |")
    for mode in ('normal', 'short'):
        row = next(item['measurements'] for item in matrix['results'] if item['configuration'] == 'default' and item['profile'] == 'single' and item['enums'] == mode)
        lines.append(f"| PIC24 {mode} | {row['before']['static_bytes']} → {row['after']['static_bytes']} B |")
    lines += ['', f'As **48 configurações/perfis/ABIs PIC24** passaram com RAM estática inalterada. Pilha interna padrão 270/264 bytes (normal/short), leitura multibloco 274/268; máximo entre opcionais **{peak} bytes**, abaixo de 400. Inclui chamadas internas e PCs de retorno; exclui backend, aplicação e libc conforme o orçamento estabelecido.', '',
              '## Persistência e validação', '',
              '- 48 cenários de escrita e 48 releituras antes/depois, com um, dois e três escritores, 257/4096 bytes, dois ritmos e duas latências. Auditoria por gravação de EOC/vínculo/diretório, conteúdo completo, igualdade das FATs e reconciliação das categorias/histogramas.',
              '- Outros 48 cenários de escrita/releitura com o backend congelando a imagem na submissão. Mesmo I/O e **zero alterações dos buffers em trânsito** nas duas versões.',
              '- 18 testes de alocação/cache na produção e sete suítes funcionais padrão/opcionais. Teste específico nos três perfis (padrão, multibloco, consulta): espera enquanto a página está em trânsito ou o par está parcialmente confirmado; reserva preservada; EOCs de pedidos reais agrupados; três ACKs falhos/retry em cada FAT; vínculos e diretório retidos até confirmação; busca alternativa esgotada sem falso ENOSPC; conteúdo após remontagem e reserva liberada.',
              '- O limite original de propriedade foi reproduzido por fragmentação válida e chamadas públicas: dois escritores pendentes + uma reserva = três EOCs, três escritores + uma reserva = quatro. A candidata mantém um alvo por escritor e uma reserva global. Não confundir isso com lotes de vários alvos por escritor.',
              '- A campanha não é uma prova de todos os pontos de corte físico. As barreiras não foram removidas; os testes incluem ACKs falhos com escrita parcial e reinicialização nos diagnósticos de propriedade. Não houve medição ou alegação de ganho de CPU.', '',
              '## Alternativas rejeitadas na investigação', '',
              'Priorizar passagens ou apenas adiar a drenagem não reduziu as contagens nos casos principais. Permitir novas alterações quando apenas a FAT2 ainda estava suja causou regravações adicionais da FAT1. Lotes de até oito clusters reduziram muito mais os metadados (2/4096 intercalado: 8259 → 1985), mas um corte após EOCs duráveis e antes dos vínculos deixou até 16 clusters sem ligação para dois escritores. Eles não foram incorporados: ampliar o lote sem registrar propriedade durável aumenta a alocação perdida após reiniciar.', '',
              '## Evidências e reprodução', '',
              'Evidências congeladas em tests/build/fat-batch. raw/base/lib é a versão anterior; raw/fresh_pair_dirty/lib é a candidata inicial; normal/lib é a produção final com documentação e fallback. I/O final em adopted/adopted-three, backend congelado final em adopted-snapshot/adopted-snapshot-three; controles anteriores em serial/interleaved, three e snapshot/snapshot-three. Matriz final em matrix-production; falhas e guardas em production-targeted; runner integrado em tests/run_fat32_allocation_checks.ps1. fat_batch_investigation.py reconstrói variantes a partir da base congelada, sem modificar produção. Este relatório valida os hashes, contadores, resultados e matriz.', '',
              '```powershell', 'python tests/fat_dirty_grouping_report.py --prepare',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted -Variants fresh_pair_dirty',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-three -Variants fresh_pair_dirty',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-snapshot -Variants fresh_pair_dirty',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-snapshot-three -Variants fresh_pair_dirty',
              'python tests/fat_dirty_grouping_report.py',
              './tests/run_fat32_allocation_checks.ps1 -OutputDirectory tests/build/fat-dirty-grouping-checks', '```']
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
