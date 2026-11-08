"""Attribute successful metadata submissions to poll stages in isolated sources."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from metadata_write_audit import block_end, diagnostic_harness, instrument, replace_once
from metadata_write_report import load as read_dataset


VARIANTS = ('base', 'no_pre_background', 'no_background', 'single_pass',
            'second_unconditional', 'extra_passes', 'sync_filter', 'sync_filter_defer',
            'adopt_pending_reservation', 'upstream')
STAGES = ('outside_poll', 'release_idle_reservation', 'inflight_operations',
          'pre_background', 'first_operations', 'second_operations', 'post_background')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def poll_body(source):
    start = source.index('void afatfs_poll()')
    opening = source.index('{', start)
    return start, opening, block_end(source, opening)


def modify_poll(source, variant):
    if variant == 'adopt_pending_reservation':
        marker = '                uint32_t sector = reserved / AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR;'
        source = replace_once(source, marker, marker + '\n                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY;')
        source = replace_once(source, '''                        // Keep the old directory image buffered while both reservation FAT pages drain.
                        afatfs_flushAllocationCache(file);
                        return AFATFS_OPERATION_IN_PROGRESS;''', '''                        // Diagnostic: transfer the pending EOC to this requested allocation.
                        // It must cross NEW_CLUSTER_DURABLE before linking or publishing.
                        opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_NEW_CLUSTER_DURABLE;
                        break;''')
        source = replace_once(source, '''                // This EOC is already durable in both copies. Retain the link and
                // directory barriers; only the redundant EOC write is skipped.
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_ZERO_DIRECTORY;
                goto doMore;''', '''                // A pending EOC still crosses the original two-copy durability barrier.
                goto doMore;''')
    if variant in ('sync_filter', 'sync_filter_defer'):
        marker = 'static afatfsOperationStatus_e afatfs_syncFileContinue(afatfsFilePtr_t file, uint8_t *phase)'
        beginning = source.index(marker)
        ending = block_end(source, source.index('{', beginning))
        body = source[beginning:ending]
        body = replace_once(body, '''#endif
        if (!afatfs_flushCache()) {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;''', '''        if (!afatfs_flushAllocationCache(file)) {
#else
        if (!afatfs_flushCache()) {
#endif
            return AFATFS_OPERATION_IN_PROGRESS;
        }
        *phase = 2;''')
        source = source[:beginning] + body + source[ending:]
    if variant == 'sync_filter_defer':
        source = replace_once(source, '''            (file->operation.active == AFATFS_FILE_OPERATION_NONE ||
                file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER)) {''', '''            (file->operation.active == AFATFS_FILE_OPERATION_NONE ||
                file->operation.active == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER ||
                (file->operation.active == AFATFS_FILE_OPERATION_CLOSE && file->operation.state.closeFile.phase < 2) ||
                (file->operation.active == AFATFS_FILE_OPERATION_SYNC && file->operation.state.syncFile.phase < 2))) {''')
    start, opening, ending = poll_body(source)
    body = source[opening + 1:ending - 1]
    if variant in ('no_pre_background', 'no_background'):
        body = replace_once(body, '''        if (afatfs.cacheDirtyEntries && !afatfs_pendingAllocationGroup()) {
            afatfs_flushCacheFiltered(0, true);
        }''', '        /* Diagnostic: omit the pre-operation background flush. */')
    if variant == 'no_background':
        body = replace_once(body, '''        if (afatfs.cacheDirtyEntries && afatfs_pendingAllocationGroup()) {
            // Let already pending writers prepare a common FAT image first.
            afatfs_flushCacheFiltered(0, true);
        }''', '        /* Diagnostic: omit the post-operation background flush. */')
    second = '''                    afatfs_fileOperationsPoll();
                } else {
                    // No operation runs before the final identical group test.
                    // Its false result cannot change, so avoid scanning twice.
                    return;
                }'''
    if variant == 'single_pass':
        body = replace_once(body, second, second.replace('                    afatfs_fileOperationsPoll();', '                    /* Diagnostic: no second operation pass. */'))
    elif variant == 'extra_passes':
        body = replace_once(body, second, second.replace('                    afatfs_fileOperationsPoll();', '                    afatfs_fileOperationsPoll();\n                    afatfs_fileOperationsPoll();\n                    afatfs_fileOperationsPoll();'))
    elif variant == 'second_unconditional':
        beginning = body.index('                if (afatfs.cacheDirtyEntries && afatfs_pendingAllocationGroup()) {')
        closing = body.index(second, beginning) + len(second)
        body = body[:beginning] + '                afatfs_fileOperationsPoll();' + body[closing:]
    return source[:opening + 1] + body + source[ending - 1:]


def trace_poll(source):
    source = instrument(source)
    source = replace_once(source, 'static bool comparisonFlushBackground;', '''static unsigned comparisonPollStage;
static unsigned long long comparisonPollCalls[7];
static unsigned long long comparisonAllocationPhases[7][32];
static bool comparisonFlushBackground;''')
    source = replace_once(source, '    fprintf(stderr, "MSUB,%u,%u,%u,%u,%u,%u,%u,%u,%u",', '''    fprintf(stderr, "PSUB,%u,%u\\n", kind, comparisonPollStage);
    fprintf(stderr, "MSUB,%u,%u,%u,%u,%u,%u,%u,%u,%u",''')
    source = replace_once(source, '    afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;',
                          '    afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;\n'
                          '    comparisonAllocationPhases[comparisonPollStage][opState->phase]++;')
    start, opening, ending = poll_body(source)
    body = source[opening + 1:ending - 1]
    # Classify each direct call site, not just the currently active file phase.
    body = replace_once(body, '        afatfs_releaseIdleReservation();',
                        '        comparisonPollStage = 1;\n        comparisonPollCalls[1]++;\n        afatfs_releaseIdleReservation();')
    # Native code contains one early, one first, and zero/one/three second passes.
    body = replace_once(body, '            afatfs_fileOperationsPoll();\n            return;',
                        '            comparisonPollStage = 2;\n            comparisonPollCalls[2]++;\n            afatfs_fileOperationsPoll();\n            return;')
    body = replace_once(body, '                afatfs_fileOperationsPoll();\n#ifdef AFATFS_PIPELINE_ALLOCATION',
                        '                comparisonPollStage = 4;\n                comparisonPollCalls[4]++;\n                afatfs_fileOperationsPoll();\n#ifdef AFATFS_PIPELINE_ALLOCATION')
    # Remaining calls have exactly 16 or 20 spaces and belong to the second stage.
    body = re.sub(r'(?m)^( {16,20})afatfs_fileOperationsPoll\(\);(?!\n#ifdef AFATFS_PIPELINE_ALLOCATION)',
                  lambda match: match[1] + 'comparisonPollStage = 5;\n' + match[1] + 'comparisonPollCalls[5]++;\n' + match[0], body)
    # The first pass was followed by #ifdef and is deliberately excluded above.
    calls = list(re.finditer(r'(?m)^            afatfs_flushCacheFiltered\(0, true\);', body))
    for match in reversed(calls):
        position = match.start()
        stage = 3 if '!afatfs_pendingAllocationGroup()' in body[max(0, position - 160):position] else 6
        body = body[:position] + f'            comparisonPollStage = {stage};\n            comparisonPollCalls[{stage}]++;\n' + body[position:]
    body = re.sub(r'(?m)^([ \t]*)return;', lambda match: match[1] + 'comparisonPollStage = 0;\n' + match[0], body)
    source = source[:opening + 1] + '\n    comparisonPollStage = 0;' + body + '\n    comparisonPollStage = 0;\n' + source[ending - 1:]
    source += '''
/** @brief Export diagnostic-only per-phase poll counts, then reset them. */
void comparisonPollAuditReport(const char *phase, unsigned profile, unsigned clusters, unsigned chunk, unsigned files)
{
    for (unsigned stage = 0; stage < 7; stage++) {
        fprintf(stderr, "PCALL,%s,%u,%u,%u,%u,%u,%llu\\n", phase, profile, clusters, chunk, files, stage, comparisonPollCalls[stage]);
        for (unsigned index = 0; index < 32; index++) {
            if (comparisonAllocationPhases[stage][index]) {
                fprintf(stderr, "PPHASE,%s,%u,%u,%u,%u,%u,%u,%llu\\n", phase, profile, clusters, chunk, files,
                    stage, index, comparisonAllocationPhases[stage][index]);
            }
        }
    }
    memset(comparisonPollCalls, 0, sizeof(comparisonPollCalls));
    memset(comparisonAllocationPhases, 0, sizeof(comparisonAllocationPhases));
}
'''
    return source


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    source = Path('lib/asyncfatfs.c').read_text()
    upstream_commit = subprocess.check_output(['git', 'rev-parse', 'upstream/master'], text=True).strip()
    upstream_source = subprocess.check_output(['git', 'show', 'upstream/master:lib/asyncfatfs.c']).decode('utf-8').replace('\r\n', '\n')
    upstream_source = upstream_source.replace('COMPUTE_BUILD_HOUR', '12').replace('COMPUTE_BUILD_MIN', '0').replace('COMPUTE_BUILD_SEC', '0')
    assert upstream_source == Path('tests/build/reserve-flush/upstream/lib/asyncfatfs.c').read_text(encoding='utf-8')
    assert '#define AFATFS_NUM_CACHE_SECTORS 8' in Path('lib/asyncfatfs_config.h').read_text()
    for variant in VARIANTS:
        destination = root / variant / 'lib'
        original = Path('tests/build/reserve-flush/upstream/lib') if variant == 'upstream' else Path('lib')
        shutil.copytree(original, destination, dirs_exist_ok=True)
        if variant != 'upstream':
            (destination / 'asyncfatfs.c').write_text(trace_poll(modify_poll(source, variant)))
    shutil.copyfile('tests/build/reserve-flush/portability.h', root / 'portability.h')
    harness = diagnostic_harness(Path('tests/upstream_comparison.c').read_text())
    harness = replace_once(harness, 'static benchmarkStats_t statistics;', '''#ifdef COMPARISON_CURRENT
void comparisonPollAuditReport(const char *phase, unsigned profile, unsigned clusters, unsigned chunk, unsigned files);
#endif
static benchmarkStats_t statistics;''')
    harness = replace_once(harness, '    printf("%s,%u,%u,%u,%u,%u,%u,%llu', '''#ifdef COMPARISON_CURRENT
    comparisonPollAuditReport(phase, latencyProfile, clusterSectors, chunkBytes, fileCount);
#endif
    printf("%s,%u,%u,%u,%u,%u,%u,%llu''')
    (root / 'harness.c').write_text(harness)
    (root / 'manifest.json').write_text(json.dumps(dict(upstream_commit=upstream_commit,
        production_sha256=digest(Path('lib/asyncfatfs.c')),
        variants=VARIANTS, sources={variant: digest(root / variant / 'lib/asyncfatfs.c') for variant in VARIANTS}), indent=2))


def read_poll(directory):
    result = {}
    pending = Counter()
    for line in (directory / 'events.log').read_text().splitlines():
        parts = line.split(',')
        if parts[0] == 'PSUB':
            pending[(int(parts[1]), int(parts[2]))] += 1
        elif parts[0] == 'META':
            key = '/'.join(parts[1:6])
            result.setdefault(key, {})['submissions'] = {f'{kind}/{stage}': count for (kind, stage), count in pending.items()}
            assert sum(pending.values()) == sum(map(int, parts[7:]))
            pending.clear()
        elif parts[0] == 'PCALL':
            key = '/'.join(parts[1:6])
            result.setdefault(key, {}).setdefault('calls', {})[STAGES[int(parts[6])]] = int(parts[7])
        elif parts[0] == 'PPHASE':
            key = '/'.join(parts[1:6])
            result.setdefault(key, {}).setdefault('allocation_phases', {})[f'{parts[6]}/{parts[7]}'] = int(parts[8])
    assert not pending
    return result


def publish(root, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['production_sha256']
    datasets = {}
    for variant in VARIANTS:
        assert digest(root / variant / 'lib/asyncfatfs.c') == manifest['sources'][variant]
        for schedule in ('serial', 'interleaved'):
            directory = root / f'{schedule}-{variant}'
            dataset = read_dataset(directory, variant != 'upstream')
            if variant != 'upstream':
                attribution = read_poll(directory)
                for key, row in dataset.items():
                    row['poll'] = attribution[key]
                    submissions = row['poll']['submissions']
                    for kind, label in ((1, 'primary'), (2, 'mirror'), (3, 'directory')):
                        assert sum(count for pair, count in submissions.items() if pair.startswith(f'{kind}/')) == row['metadata'][label]
            datasets[f'{schedule}-{variant}'] = dataset
    report = dict(manifest=manifest, stages=STAGES, datasets=datasets)
    previous = json.loads(Path('tests/RESERVE_FLUSH.json').read_text())
    for schedule, original in (('serial', 'serial-default'), ('interleaved', 'prepared-default')):
        for key, row in datasets[f'{schedule}-base'].items():
            if key.startswith('write/0/'):
                expected = previous['experiments'][original]['cpu']['after'][key]
                assert all(row['counters'][field] == expected[field] for field in ('payload_bytes', 'reads', 'writes', 'metadata_reads', 'metadata_writes', 'polls', 'virtual_us', 'multi_sessions', 'multi_blocks'))
    lines = ['# Origem das gravações de metadados em afatfs_poll()', '',
             'Diagnóstico isolado: FAT32 8.3, cache oito setores, clusters 512 bytes, arquivos de 1 MiB. Dois ritmos de pedidos, duas latências, um/dois escritores e blocos de 257/4096 bytes. Timings instrumentados descartados; produção não modificada.', '',
             'As variantes no_pre_background/no_background omitem flushes oportunistas do poll, mantendo flushes de barreiras, cache cheio, fsync e fechamento. São controles causais, não políticas aprovadas de persistência/flush em idle. single_pass retira apenas a segunda passagem condicional; second_unconditional a executa sem a condição; extra_passes executa três passagens naquele ponto.', '',
             'sync_filter deixa o diretório antigo fora da drenagem de dados/FAT da fase 1 de fsync/close. sync_filter_defer também permite a deferral de background para esse diretório nas fases 0/1 de fsync/close, mantendo o limite de idade 64. A fase 2 escreve a entrada final e a fase 3 mantém o flush explícito e o ACK antes do callback. São candidatas diagnósticas, sem promoção à produção ou qualificação completa de CPU/pilha/falhas.', '']
    lines += ['adopt_pending_reservation transfere a reserva única com EOC ainda pendente ao escritor que já pediu alocação. Usa NEW_CLUSTER_DURABLE para confirmar as duas FATs antes do vínculo; a reserva global é consumida, sem criar uma segunda reserva. Diagnóstico de agrupamento, não implementação aprovada.', '']
    for schedule in ('serial', 'interleaved'):
        for key, base in datasets[f'{schedule}-base'].items():
            if not key.startswith('write/'):
                continue
            assert not any(count for pair, count in base['poll']['submissions'].items() if pair.endswith(('/2', '/6')))
            for variant in ('no_pre_background', 'no_background'):
                candidate = datasets[f'{schedule}-{variant}'][key]
                assert candidate['metadata']['primary'] == base['metadata']['primary']
                assert candidate['metadata']['mirror'] == base['metadata']['mirror']
            for variant in ('second_unconditional', 'extra_passes'):
                assert datasets[f'{schedule}-{variant}'][key]['metadata'] == base['metadata']
            candidate = datasets[f'{schedule}-sync_filter_defer'][key]
            assert candidate['metadata']['primary'] == base['metadata']['primary']
            assert candidate['metadata']['mirror'] == base['metadata']['mirror']
            assert candidate['counters']['metadata_writes'] == base['counters']['metadata_writes'] - 1
    summary = ['## Conclusões', '',
               f"Upstream/master local verificado: `{manifest['upstream_commit']}`. Passaram **160 cenários de escrita e 160 releituras**, com a base reproduzindo o I/O da seção RESERVE_FLUSH. Nenhum timing instrumentado foi usado para afirmar ganho de CPU.", '',
               '- No padrão, o ramo AFATFS_PIPELINE_ALLOCATION chama flushCacheFiltered(0, true). As chamadas literais a flushCache() no poll estão nos ramos de journal ou sem pipeline, inativos nesta configuração.',
               '- O flush periódico inicia muitas gravações, mas retirá-lo não elimina nenhuma gravação FAT nos 16 casos da base: as barreiras em fileOperationsPoll assumem o envio. A economia fica em zero/um setor de diretório. Omitir flush periódico em produção prejudicaria o escoamento em idle.',
               '- A segunda passagem condicional reduz I/O. Em 2/4096 intercalado, retirá-la leva FAT1 de 4128 a 4160 e metadados de 8259 a 8291; FAT2 fica em 4128. O histograma mostra 32 imagens de duas entradas substituídas por 64 imagens de uma entrada, sem no-ops. Em 2/257, a remoção adiciona um setor de metadados. Torná-la incondicional ou executar passagens extras não reduz I/O nos 16 casos.',
               '- Em 2/4096 intercalado, a base envia FAT1/FAT2: 256/288 no background anterior às operações, 3839/3840 na primeira passagem e 33/0 na segunda; o diretório tem duas submissões na primeira passagem e uma fora do poll. A chamada final de background não submeteu metadados nesse caso. Isso é atribuição de envio; o A/B mostra que suprimir o background transfere esse trabalho para as barreiras.',
               '- O novo desperdício localizado é a imagem antiga do diretório no início de fsync/close. Filtrar apenas a fase 1 ajuda alguns casos; também deferir o background nas fases 0/1 elimina um setor de diretório em cada um dos 16 casos, mantendo as contagens das FATs. É uma redução de apenas 0,012–0,024% dos metadados, ainda sem qualificação completa para produção.',
               '- Transferir a reserva ainda pendente ao escritor, preservando a barreira NEW_CLUSTER_DURABLE, não melhorou I/O: em 2/4096 intercalado também resultou em 8291 metadados. Não adotar essa variante com base nestes resultados.',
               '- O alvo dominante continua sendo a granularidade de publicação das alocações: reunir alterações já solicitadas dentro da mesma fase EOC/vínculo. Aumentar o número de chamadas do escalonador, por si só, não reúne mais alterações nos casos medidos.', '']
    lines[4:4] = summary
    for schedule in ('serial', 'interleaved'):
        lines += [f'## {schedule}: contagens de metadados (latência 0 / 1)', '',
                  '| Variante | 1/257 | 2/257 | 1/4096 | 2/4096 |', '|---|---:|---:|---:|---:|']
        for variant in VARIANTS:
            values = []
            for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
                values.append(' / '.join(str(datasets[f'{schedule}-{variant}'][f'write/{profile}/1/{chunk}/{files}']['counters']['metadata_writes']) for profile in (0, 1)))
            lines.append('| ' + variant + ' | ' + ' | '.join(values) + ' |')
        lines += ['', '### Origem direta das submissões na base (latência 0)', '',
                  '| Arquivos/bloco | Etapa | FAT1 | FAT2 | Diretório | Chamadas |', '|---|---|---:|---:|---:|---:|']
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            row = datasets[f'{schedule}-base'][f'write/0/1/{chunk}/{files}']['poll']
            for stage, label in enumerate(STAGES):
                counts = [row['submissions'].get(f'{kind}/{stage}', 0) for kind in (1, 2, 3)]
                calls = row['calls'].get(label, 0)
                if any(counts) or calls:
                    lines.append(f'| {files}/{chunk} | {label} | {counts[0]} | {counts[1]} | {counts[2]} | {calls} |')
        lines.append('')
    lines += ['## Limites e reprodução', '',
              'Nas variantes atuais, as checagens EOC/vínculo/diretório são executadas a cada gravação, com releitura integral e igualdade das duas FATs. Upstream recebe verificação de conteúdo, sem exigir o espelhamento que ele não mantém neste workload. Não houve injeção de falhas nesta rodada. A matriz anterior de RAM/pilha pertence à produção inalterada; os contadores e arrays novos existem somente nas cópias diagnósticas.', '',
              'Os caminhos inflight_operations e post_background não foram exercitados como emissores de metadados na base destes workloads; isso não prova que sejam dispensáveis em outros backends, latências ou números de escritores.', '',
              'JSON inclui histogramas de entradas alteradas, fases de alocação por etapa e contadores de I/O completos.', '',
              '```powershell', 'python tests/poll_metadata_audit.py --prepare',
              "$variants = (Get-Content tests/build/poll-metadata/manifest.json | ConvertFrom-Json).variants",
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/poll-metadata -Variants $variants',
              'python tests/poll_metadata_audit.py', '```']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/poll-metadata'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('tests/POLL_METADATA.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    else:
        publish(arguments.directory, arguments.output)
