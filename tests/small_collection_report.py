"""Validate final small-request collection evidence and publish measured I/O gains."""
import hashlib
import json
from pathlib import Path
import re

from cache_index_cpu_report import resources
from metadata_write_audit import instrument
from metadata_write_report import load


root = Path('tests/build/small-write-collection')
manifest = json.loads((root / 'final-source.json').read_text())
for name, expected in manifest.items():
    assert hashlib.sha256((Path('lib') / name).read_bytes()).hexdigest() == expected, name
matrix = json.loads((root / 'adopted-matrix/report.json').read_text())
assert matrix['passed'] and len(matrix['results']) == 48
assert matrix['sources']['after']['asyncfatfs.c'] == manifest['asyncfatfs.c']
for row in matrix['results']:
    assert row['measurements']['before']['static_sections'] == row['measurements']['after']['static_sections']
    assert row['measurements']['after']['stack']['conservative_internal_bytes'] < 400
datasets = {}
for group, inventory in (('modes', 'variants.json'), ('modes-extra', 'extra-variants.json')):
    for variant in json.loads((root / inventory).read_text()):
        if variant.startswith('after_'):
            assert (root / group / variant / 'lib/asyncfatfs.c').read_text() == instrument(Path('lib/asyncfatfs.c').read_text())
        for schedule in ('serial', 'interleaved'):
            datasets[f'{schedule}-{variant}'] = load(root / group / f'{schedule}-{variant}', True,
                allow_data_rewrites=variant.startswith('after_'))
assert len(datasets) == 40
paired_257 = 0
for name, rows in datasets.items():
    if '-after_' not in name:
        continue
    before = datasets[name.replace('-after_', '-before_')]
    for key, row in rows.items():
        if key.startswith('write/') and row['counters']['chunk_bytes'] == 257:
            assert row['counters']['writes'] < before[key]['counters']['writes'] * 0.5, (name, key)
            paired_257 += 1
assert paired_257 == 80
footprints = {arch: {version: resources(root / f'resources-{arch}-{version}')
                    for version in ('before', 'after')} for arch in ('32', '64')}
assert all(value['before']['static_ram_bytes'] == value['after']['static_ram_bytes'] for value in footprints.values())
recovery = {}
for profile in ('mirror_cache', 'mirror_cache_multi', 'mirror_cache_query'):
    recovery[profile] = {}
    for campaign in ('requested_recovery', 'small_collection'):
        log = (root / f'checked-allocation/{campaign}_{profile}/results.log').read_text()
        match = re.search(r'(\d+) write cuts, (\d+) recovery cuts, 8 retry scenarios', log)
        assert match
        recovery[profile][campaign] = dict(write_cuts=int(match[1]), recovery_cuts=int(match[2]), retry_cases=8)
sync = {}
for version in ('before', 'adopted'):
    for profile in ('default', 'multi', 'query'):
        log = (root / f'sync-{version}-{profile}/results.log').read_text()
        sync[f'{version}-{profile}'] = [dict(zip(('writers', 'fat', 'directory', 'data'), map(int, values)))
            for values in re.findall(r'Small sync: writers=(\d+) requests=16 chunk=257 FAT=(\d+) directory=(\d+) data=(\d+)', log)]
        assert len(sync[f'{version}-{profile}']) == 2
peak = max(row['measurements']['after']['stack']['conservative_internal_bytes'] for row in matrix['results'])
lines = ['# Escritas pequenas: janela de alocações já solicitadas', '',
    'Validação final em FAT32 8.3, clusters de 512 bytes e cache de **oito setores**. Arquivos de 1 MiB '
    'por escritor, um/dois escritores, pedidos de 257/4096 bytes e duas latências simuladas. '
    'Base: versão imediatamente anterior, após os pequenos ajustes de fechamento (4132/8320 metadados '
    'nos casos intercalados 1/257 e 2/257).', '',
    '## Resultado principal', '',
    'Contagens de setores de 512 bytes, perfil de latência 0. Redução é calculada sobre **todas as escritas**, '
    'incluindo dados e metadados.', '',
    '| Ritmo | Escritores/bloco | Escritas antes → depois | Redução total | Metadados antes → depois | Leituras antes → depois |',
    '|---|---|---:|---:|---:|---:|']
for schedule in ('serial', 'interleaved'):
    for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
        key = f'write/0/1/{chunk}/{files}'
        before = datasets[f'{schedule}-before_buffered_default'][key]['counters']
        after = datasets[f'{schedule}-after_buffered_default'][key]['counters']
        reduction = (1 - after['writes'] / before['writes']) * 100
        if chunk == 257:
            assert reduction > 50
        lines.append(f"| {schedule} | {files}/{chunk} | {before['writes']} → {after['writes']} | {reduction:.2f}% | "
                     f"{before['metadata_writes']} → {after['metadata_writes']} | {before['reads']} → {after['reads']} |")
lines += ['', '## Como o ganho foi obtido', '',
    'Pedidos menores que um setor podem acrescentar EOCs a uma janela de até **16 clusters já solicitados**. '
    'O limite é `AFATFS_SMALL_WRITE_WINDOW_CLUSTERS=16`; zero desativa a coleção, preservando o caminho '
    'de alocação anterior. O lote original para pedidos grandes continua limitado por '
    '`AFATFS_REQUESTED_BATCH_CLUSTERS=8`.', '',
    'Cada novo cluster só é alocado quando um pedido realmente o exige. Com vários escritores, uma passada '
    'fixa dentro da página FAT permite intercalar suas alocações sem reservar os espaços intermediários. '
    'A busca mantém os espaços livres descobertos acessíveis e recorre a qualquer cluster livre da página '
    'quando a posição preferida está ocupada. A reserva especulativa global continua sendo, no máximo, um cluster.', '',
    'A janela usa as mesmas páginas FAT do cache; seu estado e callbacks de sync/close cabem na união '
    'de operações existente. Flags novos ocupam bits dos bytes já existentes. Não há heap ou buffer de setores novo. '
    'Dados podem sair do cache enquanto os EOCs são agrupados. A janela termina no limite, em sync/close '
    'ou após até 64 passagens internas ociosas, sem contar períodos em que o backend ainda grava.', '',
    '## Persistência', '',
    '- O marcador FAT32 de volume sujo é confirmado nas duas FATs antes de preparar a janela.',
    '- Todos os EOCs são confirmados nas duas cópias antes dos vínculos; os vínculos são confirmados '
    'antes da publicação do diretório. A imagem exata publicada pela coleção corresponde a dados já confirmados.',
    '- fsync/fclose podem ser enfileirados atrás da alocação usando o prefixo comum da união existente. '
    'Seus callbacks aguardam dados, ambas as FATs, diretório e término do backend.',
    '- fsync continua confirmando dados e tamanho exato. Uma sessão que usou coleção permanece '
    'conservadoramente marcada suja até o fechamento; isso evita regravar os cabeçalhos a cada fsync. '
    'Um reinício pode executar recuperação mesmo após fsync, preservando os dados confirmados. '
    'O desligamento limpo continua concluindo os ACKs antes de permitir power-off.', '',
    'A reserva especulativa não foi ampliada. Os até 16 clusters por escritor são propriedade de pedidos '
    'já recebidos. Não se antecipa dado futuro. A recuperação anterior continua removendo cabeças órfãs '
    'com memória limitada e protegendo referências vivas.', '',
    '## Memória e testes', '',
    f'RAM/código dos objetos MSVC /O2: `{json.dumps(footprints)}`. RAM estática igual em x86/x64 e em '
    f'todas as 48 configurações/perfis/ABIs PIC24. Máximo interno **{peak} bytes**; inclui PCs internos '
    'e exclui backend/aplicação/libc, conforme o orçamento estabelecido.', '',
    'As 40 execuções de modos/backends passaram em **320 cenários de escrita e 320 releituras**. '
    'Incluem w/a/w+/x/a+/r+, fputc, backends padrão, leitura multibloco, multibloco com consulta e escrita monobloco. '
    'Cada gravação foi auditada quanto à ordem EOC/vínculo/diretório; conteúdo e ambas as FATs foram conferidos. '
    'Os 80 pares de 257 bytes superaram 50% de redução nas escritas totais, incluindo as duas latências. '
    'As contagens completas, incluindo as duas latências, estão no JSON.', '',
    'Passaram as sete suítes funcionais e os testes de alocação/cache, incluindo disco quase cheio com '
    'outro escritor aberto e inativo. Esse caso consome espaços de ambas as posições e verifica a cadeia '
    'do arquivo que ocupa o restante do volume. Testes de falha agora deixam explicitamente um byte sujo '
    'quando precisam bloquear uma transferência: a coleção pode já ter confirmado os dados após uma espera.', '',
    f'Campanhas por perfil: `{json.dumps(recovery)}`. Incluem meio setor, falhas de submissão/ACK, '
    'recuperação interrompida, criação e extensão, preservação de prefixo confirmado, raiz/subdiretório '
    'e imutabilidade dos buffers em trânsito.', '',
    '## fsync a cada pedido', '',
    'Este padrão encerra a janela em cada sincronização e não recebe a mesma redução da escrita contínua. '
    'O teste usa 16 pedidos de 257 bytes por escritor e solicita fsync imediatamente após aceitar os bytes, '
    'sem aguardar a coleção ficar ociosa. Confere a mídia após cada callback, injeta três falhas de ACK e '
    'reinicia sem fclose para validar os dados confirmados.', '',
    '| Escritores | FAT antes → depois | Diretório antes → depois | Escritas duráveis antes → depois |',
    '|---|---:|---:|---:|']
for before, after in zip(sync['before-default'], sync['adopted-default']):
    total_before = sum(before[field] for field in ('fat', 'directory', 'data'))
    total_after = sum(after[field] for field in ('fat', 'directory', 'data'))
    lines.append(f"| {before['writers']} | {before['fat']} → {after['fat']} | {before['directory']} → {after['directory']} | {total_before} → {total_after} |")
lines += ['', 'As três tentativas de ACK que falharam são adicionais em ambas as versões e não entram '
    'nessa tabela de conclusões duráveis.', '',
    '## Custos e limites', '',
    'Nos pedidos de 257 bytes, o número de leituras pode aumentar por causa de cabeçalhos e releituras de setores parciais. '
    'O relatório mostra esse custo; a meta de mais de 50% se refere às escritas totais contínuas. '
    'O caso 2/4096 preserva 35 leituras e 1560 metadados na latência 0. '
    'Não foram medidos CPU nem throughput em cartão físico.', '',
    'Permanecem os limites anteriores da recuperação: montagem de volume sujo pode ser demorada em '
    'volumes grandes/fragmentados e não substitui fsck para ciclos órfãos ou corrupção arbitrária. '
    'A campanha sintética não prova todos os padrões possíveis de falha física.', '',
    'Evidências congeladas: tests/build/small-write-collection. O harness e seus snapshots não pertencem '
    'ao orçamento de RAM/pilha da biblioteca.']
output = Path('tests/SMALL_WRITE_COLLECTION.md')
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, datasets=datasets, matrix=matrix,
    footprints=footprints, recovery=recovery, sync=sync), indent=2) + '\n', encoding='utf-8')
print(f'Validated {len(datasets)} runs; PIC24 peak {peak} bytes; report {output}')
