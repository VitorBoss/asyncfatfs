"""Freeze, audit and publish the small-write allocation and sync comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from cache_index_cpu_report import resources
from metadata_write_audit import diagnostic_harness, instrument
from metadata_write_report import load


def prepare(root):
    """Keep backend-profile controls independent of production configuration."""
    manifest = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in Path('lib').iterdir() if path.is_file()}
    for version, original in (('before', root / 'before/lib'), ('after', Path('lib'))):
        for profile in ('default', 'multi', 'query', 'single'):
            name = version if profile == 'default' else f'{version}_{profile}'
            library = root / f'audit/{name}/lib'
            shutil.copytree(original, library, dirs_exist_ok=True)
            (library / 'asyncfatfs.c').write_text(instrument((original / 'asyncfatfs.c').read_text()))
            configuration = (library / 'asyncfatfs_config.h').read_text()
            prefix = ''
            if profile in ('multi', 'query'):
                prefix += '#define AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT 2\n'
            if profile == 'query':
                prefix += '#define AFATFS_READ_SESSION_QUERY 1\n'
            if profile == 'single':
                configuration = configuration.replace('#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2',
                    '// Single-block control.')
            (library / 'asyncfatfs_config.h').write_text(prefix + configuration)
    harness = diagnostic_harness(Path('tests/upstream_comparison.c').read_text())
    (root / 'audit/harness.c').write_text(harness)
    for version in ('before', 'after'):
        shutil.copytree(root / f'audit/{version}/lib', root / f'three/{version}/lib', dirs_exist_ok=True)
    (root / 'three/harness.c').write_text('#define COMPARISON_THREE_FILES\n' + harness)
    shutil.copyfile(root / 'audit/portability.h', root / 'three/portability.h')
    (root / 'final-source.json').write_text(json.dumps(manifest, indent=2))


def publish(root, output):
    """Assert RAM, stack, persistence campaigns and reconciled per-sector counts."""
    manifest = json.loads((root / 'final-source.json').read_text())
    assert all(hashlib.sha256((Path('lib') / name).read_bytes()).hexdigest() == value
               for name, value in manifest.items())
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['asyncfatfs.c']
    assert all(row['measurements']['before']['static_sections'] == row['measurements']['after']['static_sections']
               for row in matrix['results'])
    peak = max(row['measurements']['after']['stack']['conservative_internal_bytes'] for row in matrix['results'])
    assert peak < 400
    datasets = {}
    for profile in ('default', 'multi', 'query', 'single'):
        for version in ('before', 'after'):
            variant = version if profile == 'default' else f'{version}_{profile}'
            for schedule in ('serial', 'interleaved'):
                datasets[f'{schedule}-{variant}'] = load(root / f'audit/{schedule}-{variant}', True)
    for version in ('before', 'after'):
        for schedule in ('serial', 'interleaved'):
            datasets[f'three-{schedule}-{version}'] = load(root / f'three/{schedule}-{version}', True, 8)
    footprints = {arch: {version: resources(root / f'resources-{arch}-{version}')
                        for version in ('before', 'after')} for arch in ('32', '64')}
    assert all(values['before']['static_ram_bytes'] == values['after']['static_ram_bytes'] for values in footprints.values())
    sync = {}
    for version in ('before', 'normal'):
        for profile in ('default', 'multi', 'query'):
            log = (root / f'sync-{version}-{profile}/results.log').read_text()
            sync[f'{version}-{profile}'] = [dict(zip(('writers', 'fat', 'directory', 'data'), map(int, row)))
                for row in re.findall(r'Small sync: writers=(\d+) requests=16 chunk=257 FAT=(\d+) directory=(\d+) data=(\d+)', log)]
            assert len(sync[f'{version}-{profile}']) == 2
    for profile in ('mirror_cache', 'mirror_cache_multi', 'mirror_cache_query'):
        log = (root / f'allocation-final/requested_recovery_{profile}/results.log').read_text()
        assert '82 write cuts, 58 recovery cuts, 8 retry scenarios' in log
    lines = ['# Pedidos de 257 bytes: reserva e sincronização', '',
        'FAT32 8.3, clusters de 512 bytes, cache oito setores. Duas FATs, reserva especulativa única, '
        'RAM estática e ordem de persistência mantidas. Não há lote especulativo de oito clusters para pedidos pequenos.', '',
        '## Mudanças', '',
        '- Uma alocação já solicitada por outro escritor tem prioridade sobre criar uma reserva opcional. '
        'A reserva existente permanece válida; não são criados novos estados ou buffers.',
        '- fsync/fclose drenam dados e ambas as FATs mantendo a imagem antiga do próprio diretório em cache. '
        'Depois publicam o tamanho lógico final e aguardam seu ACK. O background também reconhece os estágios '
        'iniciais de sync/close dentro do limite de idade já existente. Isso evita gravar a imagem antiga '
        'imediatamente antes de substituí-la.', '',
        '## Escrita contínua', '',
        'Arquivos de 1 MiB. Setores de 512 bytes; latência 0. Não houve medição de CPU.', '',
        '| Ritmo | Escritores/bloco | Metadados antes → depois | Leituras antes → depois | Escritas antes → depois |',
        '|---|---|---:|---:|---:|']
    for schedule in ('serial', 'interleaved'):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/0/1/{chunk}/{files}'
            before = datasets[f'{schedule}-before'][key]['counters']
            after = datasets[f'{schedule}-after'][key]['counters']
            assert after['metadata_writes'] <= before['metadata_writes']
            lines.append(f"| {schedule} | {files}/{chunk} | {before['metadata_writes']} → {after['metadata_writes']} | "
                         f"{before['reads']} → {after['reads']} | {before['writes']} → {after['writes']} |")
    lines += ['', 'O ganho contínuo de 257 bytes é pequeno: um setor com um escritor e três com dois '
        'intercalados. Com um escritor, as FATs já somam 4130 setores: um par por cluster, mais as transições '
        'entre páginas FAT e a limpeza da reserva final. O diretório cai de três para dois setores. '
        'Não foi encontrada redução da mesma escala dos pedidos de 4096 preservando o caminho atual '
        'de confirmação por cluster. Acumular pedidos futuros, ampliar reservas ou remover barreiras '
        'não foi adotado nesta mudança.', '',
        '## Sincronização a cada pedido de 257 bytes', '',
        'Dezesseis pedidos por escritor. Dados e tamanho foram conferidos diretamente na mídia após cada fsync; '
        'houve três falhas de ACK do diretório com retry, bloqueio de conclusão/desligamento e verificação '
        'do buffer em trânsito. Releitura após remontagem passou. Escritas abaixo são conclusões duráveis, '
        'sem contar tentativas que falharam.', '',
        '| Escritores | FAT antes → depois | Diretório antes → depois | Dados antes → depois | Metadados antes → depois |',
        '|---|---:|---:|---:|---:|']
    for before, after in zip(sync['before-default'], sync['normal-default']):
        assert before['writers'] == after['writers']
        lines.append(f"| {before['writers']} | {before['fat']} → {after['fat']} | {before['directory']} → {after['directory']} | "
                     f"{before['data']} → {after['data']} | {before['fat']+before['directory']} → {after['fat']+after['directory']} |")
    lines += ['', '## Validação e memória', '',
        'Quatro perfis de backend (padrão, multibloco de leitura, multibloco com consulta e escrita monobloco), '
        'duas latências e dois ritmos; um/dois escritores e 257/4096. Três escritores são controles adicionais. '
        'Conteúdo, ambas as FATs e auditoria EOC/vínculo/diretório por gravação foram conferidos. '
        'As contagens completas estão no JSON.', '',
        f'RAM e código dos objetos: `{json.dumps(footprints)}`. Matriz PIC24 de 48 configurações aprovada, '
        f'máximo de pilha interna **{peak} bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.', '',
        'Passaram 24 testes de alocação/cache e sete suítes funcionais. As campanhas nos três perfis '
        'incluem 82 cortes de escrita, 58 cortes durante recuperação e oito cenários de falha/retry por perfil. '
        'O teste de páginas FAT sujas agora monta explicitamente a reserva preexistente, porque a nova '
        'política evita criá-la quando há pedidos de pares já pendentes; a cobertura de ambos os ACKs e '
        'do fallback de disco quase cheio permanece.', '',
        'As variantes experimentais staggered/durable não deram ganho relevante, e a remoção isolada '
        'da espera chegou a acrescentar duas gravações. Não foram incorporadas. '
        'Evidências: tests/build/small-write-pipeline. Persistem os limites anteriores da recuperação de volumes sujos.']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, datasets=datasets, matrix=matrix,
        footprints=footprints, sync=sync), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/small-write-pipeline'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('tests/SMALL_WRITE_PIPELINE.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    else:
        publish(arguments.directory, arguments.output)
