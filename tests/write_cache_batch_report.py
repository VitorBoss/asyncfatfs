"""Prepare repeatable write-cache policy comparisons without mutating the frozen baseline."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from metadata_write_audit import diagnostic_harness, instrument, replace_once
from metadata_write_report import load
from cache_index_cpu_report import resources


def publish(root, output):
    """Validate frozen evidence and report I/O separately from host timing noise."""
    manifest = json.loads((root / 'final-source.json').read_text())
    for filename, expected in manifest.items():
        assert hashlib.sha256((Path('lib') / filename).read_bytes()).hexdigest() == expected, filename
    matrix = json.loads((root / 'matrix/report.json').read_text())
    assert matrix['passed'] and len(matrix['results']) == 48
    assert matrix['sources']['after']['asyncfatfs.c'] == manifest['asyncfatfs.c']
    peak = 0
    for entry in matrix['results']:
        before = entry['measurements']['before']
        after = entry['measurements']['after']
        assert before['static_sections'] == after['static_sections']
        peak = max(peak, after['stack']['conservative_internal_bytes'])
    assert peak < 400
    datasets = {}
    for group, inventory in (('modes', 'variants.json'), ('modes-extra', 'extra-variants.json')):
        for variant in json.loads((root / inventory).read_text()):
            for schedule in ('serial', 'interleaved'):
                datasets[f'{schedule}-{variant}'] = load(root / group / f'{schedule}-{variant}', True)
    assert len(datasets) == 40
    paired = 0
    improved = 0
    for name, rows in datasets.items():
        if '-after_' not in name:
            continue
        baseline = datasets[name.replace('-after_', '-before_')]
        for key, row in rows.items():
            after = row['counters']
            before = baseline[key]['counters']
            assert after['reads'] <= before['reads'], (name, key)
            assert after['metadata_writes'] <= before['metadata_writes'], (name, key)
            if key.startswith('write/'):
                paired += 1
                improved += after['reads'] < before['reads'] or after['metadata_writes'] < before['metadata_writes']
    assert paired == 160
    footprints = {architecture: {version: resources(root / f'resources-{architecture}-{version}')
                                for version in ('before', 'after')} for architecture in ('32', '64')}
    for values in footprints.values():
        assert values['before']['static_ram_bytes'] == values['after']['static_ram_bytes']
    recovery = {}
    for profile in ('mirror_cache', 'mirror_cache_multi', 'mirror_cache_query'):
        recovery[profile] = (root / f'allocation/requested_recovery_{profile}/results.log').read_text().strip()
        assert '82 write cuts, 58 recovery cuts, 8 retry scenarios' in recovery[profile]
    lines = ['# Cache de escrita: FAT preservada sob pressão de dados', '',
        'FAT32 8.3, clusters de 512 bytes, cache de oito setores. A reserva especulativa continua sendo '
        'um único cluster. A política compartilhada atende fwrite/fputc e os modos w, a, w+, x, a+ e r+.', '',
        '## Alteração', '',
        'Uma página liberada pelo escritor recebe a preferência de descarte já existente. Ela só pode ser '
        'reutilizada depois do ACK, quando estiver IN_SYNC, sem lock nem retenção. Durante uma sessão de lotes '
        'marcada suja, a FAT limpa recebe preferência temporária sobre dados sujos que ainda podem ser gravados. '
        'Sob pressão, grava-se primeiro o dado liberado mais antigo, em vez de expulsar a FAT ou publicar um '
        'diretório antigo para abrir espaço. Sem página de dados liberável, mantém-se o fallback normal.', '',
        'As barreiras explícitas continuam drenando dados e ambas as FATs antes da publicação do diretório. '
        'EOCs, vínculos, marcador de volume sujo/limpo e recuperação mantêm seus ACKs. Nenhuma nova reserva, '
        'buffer ou membro de estrutura foi acrescentado.', '',
        '## Comparação principal', '',
        'Arquivos de 1 MiB, latência simulada 0. Números são setores de 512 bytes, sem medição de CPU. '
        'A base é a versão anterior de 1799 metadados.', '',
        '| Ritmo | Escritores/bloco | Leituras antes → depois | Metadados antes → depois | Escritas antes → depois |',
        '|---|---|---:|---:|---:|']
    for schedule in ('serial', 'interleaved'):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/0/1/{chunk}/{files}'
            before = datasets[f'{schedule}-before_buffered_default'][key]['counters']
            after = datasets[f'{schedule}-after_buffered_default'][key]['counters']
            assert after['metadata_writes'] <= before['metadata_writes']
            lines.append(f"| {schedule} | {files}/{chunk} | {before['reads']} → {after['reads']} | "
                         f"{before['metadata_writes']} → {after['metadata_writes']} | {before['writes']} → {after['writes']} |")
    lines += ['', 'Caso intercalado 2/4096: **771 →35 leituras (−95,46%)**, '
        '**1799 →1560 metadados (−13,29%)**, **5895 →5656 escritas (−4,05%)**. '
        'FAT1/FAT2: 643 gravações cada; diretório: 274. As 35 leituras são 33 da FAT primária, '
        'uma da secundária e uma de diretório. O diretório sobe de 259 para 274 gravações; '
        'a redução de FAT compensa esse custo.', '',
        'Upstream/master local 60c5f3580152410cb08342942cbbe3a30a449c24, conforme comparação anterior: '
        '453 leituras, 5634 escritas, 1538 metadados nesse caso. Restam 22 escritas a mais (+0,39%), '
        'com duas FATs atualizadas; o upstream não grava a segunda FAT nesse workload.', '',
        '## Modos e backends', '',
        'Caso intercalado 2/4096; cada linha também foi validada com um escritor, pedidos de 257 bytes, '
        'ritmo serial e duas latências. r+ inclui a criação e o fechamento inicial dos arquivos vazios '
        'antes da reabertura; esse custo é idêntico nas duas versões.', '',
        '| Modo/perfil | Leituras antes → depois | Metadados antes → depois |',
        '|---|---:|---:|']
    cases = [variant.removeprefix('before_') for variant in json.loads((root / 'variants.json').read_text())
             + json.loads((root / 'extra-variants.json').read_text()) if variant.startswith('before_')]
    for case in cases:
        key = 'write/0/1/4096/2'
        before = datasets[f'interleaved-before_{case}'][key]['counters']
        after = datasets[f'interleaved-after_{case}'][key]['counters']
        lines.append(f"| {case} | {before['reads']} → {after['reads']} | {before['metadata_writes']} → {after['metadata_writes']} |")
    lines += ['', 'Os pedidos pequenos e fputc preservam as contagens anteriores. Não passam a reservar '
        'oito clusters: somente pedidos já cobrindo vários setores permitem lotes. Isso respeita a escolha '
        'do usuário de manter uma única reserva especulativa, mesmo quando o ganho varia.', '',
        'A quantidade de dados enviada em sessões multibloco também muda: no caso principal, 3585 →253 '
        'setores. A pressão é resolvida mais cedo e o cache conserva metadados. O suporte multibloco '
        'continua ativo; não se afirma ganho de throughput em cartão físico a partir dessas contagens.', '',
        '## Memória e validação', '',
        f'RAM estática idêntica antes/depois: `{json.dumps(footprints)}`. '
        f'Matriz PIC24 de 48 configurações/perfis/ABIs aprovada, máximo interno **{peak} bytes** '
        '(inclui PCs internos; exclui backend/aplicação/libc conforme o orçamento estabelecido).', '',
        'As 40 execuções de modos/backends validaram **320 cenários de escrita e 320 releituras**, '
        'com conteúdo completo, FATs iguais e auditoria EOC/vínculo/diretório a cada gravação. '
        f'Das {paired} comparações pareadas de escrita, {improved} melhoraram leituras ou metadados; '
        'as demais mantiveram as contagens, sem regressões de I/O medidas. '
        'Passaram 21 testes de alocação/cache, incluindo três campanhas de 82 cortes de escrita, '
        '58 cortes durante recuperação e oito falhas/retry. Incluem buffers imutáveis, dados confirmados '
        'na raiz/subdiretório, escritores mistos e novo lote durante a publicação do marcador limpo. '
        'Sete suítes funcionais padrão/opcionais também passaram.', '',
        'Permanecem os limites da recuperação da versão anterior: varredura potencialmente lenta em '
        'volumes sujos grandes e ausência de reparo geral de ciclos/corrupção arbitrária. '
        'Evidências: tests/build/write-cache-batch; medições no simulador, sem cartão físico.']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(dict(manifest=manifest, matrix=matrix,
        footprints=footprints, datasets=datasets, recovery=recovery, paired=paired, improved=improved), indent=2) + '\n')


def prepare_final(root, extra=False):
    """Exercise every native opening mode plus fputc, and each backend configuration."""
    cases = [('buffered', 'default', 'w', False), ('append', 'default', 'a', False),
             ('update', 'default', 'w+', False), ('exclusive', 'default', 'x', False),
             ('byte', 'default', 'w', True), ('buffered', 'multi', 'w', False),
             ('buffered', 'query', 'w', False), ('buffered', 'single', 'w', False)]
    if extra:
        cases = [('append_read', 'default', 'a+', False), ('reopen', 'default', 'r+', False)]
    harness = diagnostic_harness(Path('tests/upstream_comparison.c').read_text())
    harness = harness.replace('openFile(names[index], "w")', 'openFile(names[index], COMPARISON_WRITE_MODE)')
    harness = replace_once(harness,
        '        files[index] = openFile(names[index], COMPARISON_WRITE_MODE);',
        '''#ifdef COMPARISON_REOPEN_WRITE
        files[index] = openFile(names[index], "w");
        closeFile(files[index]);
#endif
        files[index] = openFile(names[index], COMPARISON_WRITE_MODE);''')
    harness = harness.replace('afatfs_fwrite(', 'comparisonWrite(')
    marker = 'static void runCase('
    offset = harness.index(marker)
    wrapper = '''/** @brief Write through the selected public entry point, preserving partial acceptance. */
static uint32_t comparisonWrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t length)
{
#ifdef COMPARISON_PUTC_MODE
    uint32_t count = 0;
    while (count < length) {
        if (afatfs_fputc(file, buffer[count]) != 1) {
            break;
        }
        count++;
    }
    return count;
#else
    return afatfs_fwrite(file, buffer, length);
#endif
}

'''
    harness = harness[:offset] + wrapper + harness[offset:]
    destination = root / ('modes-extra' if extra else 'modes')
    destination.mkdir(exist_ok=True)
    (destination / 'harness.c').write_text(harness)
    shutil.copyfile(root / 'audit/portability.h', destination / 'portability.h')
    variants = []
    for mode, profile, opening, byte in cases:
        for version, original in (('before', root / 'before/lib'), ('after', Path('lib'))):
            variant = f'{version}_{mode}_{profile}'
            variants.append(variant)
            library = destination / variant / 'lib'
            shutil.copytree(original, library, dirs_exist_ok=True)
            (library / 'asyncfatfs.c').write_text(instrument((original / 'asyncfatfs.c').read_text()))
            configuration = (library / 'asyncfatfs_config.h').read_text()
            definitions = f'#define COMPARISON_WRITE_MODE "{opening}"\n'
            if byte:
                definitions += '#define COMPARISON_PUTC_MODE\n'
            if mode == 'reopen':
                definitions += '#define COMPARISON_REOPEN_WRITE\n'
            if profile in ('multi', 'query'):
                definitions += '#define AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT 2\n'
            if profile == 'query':
                definitions += '#define AFATFS_READ_SESSION_QUERY 1\n'
            if profile == 'single':
                configuration = configuration.replace('#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2',
                                                      '// Single-block backend control.')
            (library / 'asyncfatfs_config.h').write_text(definitions + configuration)
    (root / ('extra-variants.json' if extra else 'variants.json')).write_text(json.dumps(variants))
    (root / 'final-source.json').write_text(json.dumps({path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in Path('lib').iterdir() if path.is_file()}, indent=2))


def prepare(root):
    baseline = (root / 'before/lib/asyncfatfs.c').read_text()
    released = replace_once(baseline,
        '        afatfs.cacheDescriptor[file->writeLockedCacheIndex].locked = 0;',
        '''        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            file->type == AFATFS_FILE_TYPE_NORMAL) {
            afatfs.cacheDescriptor[file->writeLockedCacheIndex].discardable = true;
        }
        afatfs.cacheDescriptor[file->writeLockedCacheIndex].locked = 0;''')
    pinned = replace_once(released,
        '    if (allocateIndex > -1) {\n#ifdef AFATFS_PIPELINE_ALLOCATION',
        '''#ifdef AFATFS_PIPELINE_ALLOCATION
    if (allocateIndex >= 0 && emptyIndex < 0 && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&
        afatfs.sectorsPerCluster == 1 &&
        afatfs.cacheDescriptor[allocateIndex].sectorIndex >= afatfs.fatStartSector &&
        afatfs.cacheDescriptor[allocateIndex].sectorIndex < afatfs.clusterStartSector) {
        for (unsigned index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
            const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
            if (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked &&
                entry->sectorIndex >= afatfs.clusterStartSector) {
                return -1;
            }
        }
    }
#endif
    if (allocateIndex > -1) {
#ifdef AFATFS_PIPELINE_ALLOCATION''')
    pressure = replace_once(pinned,
        '    if (cacheSectorIndex == -1) {\n',
        '''    if (cacheSectorIndex == -1) {
#ifdef AFATFS_PIPELINE_ALLOCATION
        if (afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 && afatfs.sectorsPerCluster == 1 &&
            !afatfs.cacheFlushInProgress) {
            int dataIndex = -1;
            for (int index = 0; index < AFATFS_NUM_CACHE_SECTORS; index++) {
                const afatfsCacheBlockDescriptor_t *entry = &afatfs.cacheDescriptor[index];
                if (entry->state == AFATFS_CACHE_STATE_DIRTY && !entry->locked && entry->discardable &&
                    (dataIndex < 0 || entry->writeTimestamp < afatfs.cacheDescriptor[dataIndex].writeTimestamp)) {
                    dataIndex = index;
                }
            }
            if (dataIndex >= 0) {
                afatfs_cacheFlushSector(dataIndex);
                return AFATFS_OPERATION_IN_PROGRESS;
            }
        }
#endif
''')
    bounded = replace_once(pressure,
        '    if (allocateIndex >= 0 && emptyIndex < 0 && afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&',
        '    if (allocateIndex >= 0 && emptyIndex < 0 && afatfs.batchDirtyMarked &&\n        afatfs.fileSystemType == FAT_FILESYSTEM_TYPE_FAT32 &&')
    for variant, source in (('before', baseline), ('released', released), ('pinned', pinned), ('pressure', pressure), ('bounded', bounded)):
        library = root / f'audit/{variant}/lib'
        shutil.copytree(root / 'before/lib', library, dirs_exist_ok=True)
        (library / 'asyncfatfs.c').write_text(instrument(source))
        raw = root / f'{variant}-raw/lib'
        shutil.copytree(root / 'before/lib', raw, dirs_exist_ok=True)
        (raw / 'asyncfatfs.c').write_text(source)
    (root / 'audit/harness.c').write_text(diagnostic_harness(Path('tests/upstream_comparison.c').read_text()))
    shutil.copyfile('tests/build/requested-batch-safe/audit/portability.h', root / 'audit/portability.h')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/write-cache-batch'))
    parser.add_argument('--prepare-final', action='store_true')
    parser.add_argument('--prepare-extra', action='store_true')
    parser.add_argument('--publish', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('tests/WRITE_CACHE_BATCH.md'))
    arguments = parser.parse_args()
    if arguments.publish:
        publish(arguments.directory, arguments.output)
    elif arguments.prepare_extra:
        prepare_final(arguments.directory, extra=True)
    elif arguments.prepare_final:
        prepare_final(arguments.directory)
    else:
        prepare(arguments.directory)
