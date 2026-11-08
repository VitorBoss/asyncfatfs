"""Count durable FAT entry transitions and relate each image to accepted payload."""
import argparse
from collections import Counter
from pathlib import Path
import json
import re
import shutil

from metadata_write_audit import block_end, replace_once
from metadata_write_report import load
from mirror_profile_fix_report import digest
from poll_metadata_audit import read_poll


VARIANTS = ('base', 'no_background', 'upstream')
KINDS = ('free_to_eoc', 'free_to_link', 'eoc_to_link', 'eoc_to_free', 'link_to_link', 'other')
TIMER_VARIANTS = ('base', 'no_poll_age', 'no_idle_release', 'no_age_or_release')
TRACE = r'''
static uint64_t comparisonTransitions[2][6];
static unsigned comparisonImages[2];
static uint32_t comparisonAccepted[3];
static afatfsFilePtr_t comparisonFiles[3];

/** @brief Decode the lower 28 bits of one FAT32 entry in a diagnostic image. */
static uint32_t comparisonFatValue(const uint8_t *bytes)
{
    return (bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24) & 0x0FFFFFFF;
}

/** @brief Classify actual completed media transitions, preserving repeated intermediate states. */
static void comparisonFatTransitions(const uint8_t *previous, const uint8_t *current, uint32_t sector)
{
    uint32_t fatSectors = (dataStart - 2) / 2;
    if (sector < 2 || sector >= dataStart) {
        return;
    }
    unsigned copyIndex = (sector - 2) / fatSectors;
    unsigned image = comparisonImages[copyIndex]++;
    bool detailed = image < 12;
    if (detailed) {
        fprintf(stderr, "FAT_EDGE,%u,%u,%u,%u,%u,%u", copyIndex, image, sector,
            comparisonAccepted[0], comparisonAccepted[1], comparisonAccepted[2]);
    }
    for (unsigned index = 0; index < SECTOR_BYTES / 4; index++) {
        if (!memcmp(previous + index * 4, current + index * 4, 4)) {
            continue;
        }
        uint32_t oldValue = comparisonFatValue(previous + index * 4);
        uint32_t newValue = comparisonFatValue(current + index * 4);
        unsigned kind = 5;
        if (!oldValue && newValue >= 0x0FFFFFF8) {
            kind = 0;
        } else if (!oldValue && newValue >= 2 && newValue < 0x0FFFFFF0) {
            kind = 1;
        } else if (oldValue >= 0x0FFFFFF8 && newValue >= 2 && newValue < 0x0FFFFFF0) {
            kind = 2;
        } else if (oldValue >= 0x0FFFFFF8 && !newValue) {
            kind = 3;
        } else if (oldValue >= 2 && oldValue < 0x0FFFFFF0 && newValue >= 2 && newValue < 0x0FFFFFF0) {
            kind = 4;
        }
        comparisonTransitions[copyIndex][kind]++;
        if (detailed) {
            unsigned cluster = ((sector - 2) % fatSectors) * (SECTOR_BYTES / 4) + index;
            fprintf(stderr, ",%u:%07X>%07X", cluster, oldValue, newValue);
        }
    }
    if (detailed) {
        fputc('\n', stderr);
    }
}
'''

WRAPPER = r'''
/** @brief Count only bytes actually returned as accepted to the application. */
static uint32_t comparisonTrackedWrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t length)
{
    uint32_t accepted = afatfs_fwrite(file, buffer, length);
    for (unsigned index = 0; index < 3; index++) {
        if (comparisonFiles[index] == file) {
            comparisonAccepted[index] += accepted;
            break;
        }
    }
    return accepted;
}
#define afatfs_fwrite comparisonTrackedWrite
'''


def prepare(root):
    original = Path('tests/build/poll-metadata')
    manifest = json.loads((original / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['production_sha256']
    for variant in VARIANTS:
        shutil.copytree(original / variant / 'lib', root / variant / 'lib', dirs_exist_ok=True)
    shutil.copyfile(original / 'portability.h', root / 'portability.h')
    harness = (original / 'harness.c').read_text()
    # dataStart is declared before statistics in this harness.
    harness = replace_once(harness, 'static benchmarkStats_t statistics;', 'static benchmarkStats_t statistics;\n' + TRACE)
    harness = replace_once(harness, '        if (transferWrite) {', '        if (transferWrite) {\n            comparisonFatTransitions(sectorData, transferBuffer, transferSector);')
    marker = 'static void runCase(uint32_t clusterSectors, uint32_t chunkBytes, uint32_t fileCount, uint32_t repetition)'
    harness = replace_once(harness, marker, WRAPPER + '\n' + marker)
    harness = replace_once(harness, '    memset(&statistics, 0, sizeof(statistics));',
                          '    memset(comparisonAccepted, 0, sizeof(comparisonAccepted));\n'
                          '    memset(comparisonFiles, 0, sizeof(comparisonFiles));\n'
                          '    memset(&statistics, 0, sizeof(statistics));')
    harness = replace_once(harness, '        files[index] = openFile(names[index], "w");',
                          '        files[index] = openFile(names[index], "w");\n        comparisonFiles[index] = files[index];')
    marker = '    printf("%s,%u,%u,%u,%u,%u,%u,%llu'
    harness = replace_once(harness, marker, r'''
    for (unsigned copyIndex = 0; copyIndex < 2; copyIndex++) {
        fprintf(stderr, "FAT_TRANSITIONS,%s,%u,%u,%u,%u,%u,%u,%u", phase, latencyProfile,
            clusterSectors, chunkBytes, fileCount, repetition, copyIndex, comparisonImages[copyIndex]);
        for (unsigned kind = 0; kind < 6; kind++) {
            fprintf(stderr, ",%llu", (unsigned long long)comparisonTransitions[copyIndex][kind]);
        }
        fputc('\n', stderr);
    }
    fprintf(stderr, "ACCEPTED_TOTALS,%s,%u,%u,%u,%u,%u,%u,%u,%u\n", phase, latencyProfile,
        clusterSectors, chunkBytes, fileCount, repetition, comparisonAccepted[0], comparisonAccepted[1], comparisonAccepted[2]);
    memset(comparisonImages, 0, sizeof(comparisonImages));
    memset(comparisonTransitions, 0, sizeof(comparisonTransitions));
    printf("%s,%u,%u,%u,%u,%u,%u,%llu''')
    (root / 'harness.c').write_text(harness)
    (root / 'manifest.json').write_text(json.dumps(dict(production_sha256=manifest['production_sha256'],
        upstream_commit=manifest['upstream_commit'], harness_sha256=digest(root / 'harness.c'),
        variants=VARIANTS, sources={variant: digest(root / variant / 'lib/asyncfatfs.c') for variant in VARIANTS}), indent=2))


def prepare_timer(root):
    """Separate poll counter aging from idle reservation cleanup, in frozen copies."""
    timer = root / 'timer'
    source = (root / 'base/lib/asyncfatfs.c').read_text()
    match = re.search(r'static void afatfs_releaseIdleReservation\(void\)\s*\{', source)
    assert match
    ending = block_end(source, match.end() - 1)
    body = source[match.start():ending]
    for variant in TIMER_VARIANTS:
        destination = timer / variant / 'lib'
        shutil.copytree(root / 'base/lib', destination, dirs_exist_ok=True)
        changed = body
        if variant in ('no_poll_age', 'no_age_or_release'):
            changed = replace_once(changed, '    afatfs.cacheTimer++;', '    /* Diagnostic: age only actual cache accesses. */')
        if variant in ('no_idle_release', 'no_age_or_release'):
            changed = replace_once(changed, '    afatfs_releaseReservedCluster();', '    /* Diagnostic: retain the reserve until explicit sync/close. */')
        (destination / 'asyncfatfs.c').write_text(source[:match.start()] + changed + source[ending:])
    shutil.copyfile(root / 'harness.c', timer / 'harness.c')
    shutil.copyfile(root / 'portability.h', timer / 'portability.h')
    (timer / 'manifest.json').write_text(json.dumps(dict(variants=TIMER_VARIANTS,
        sources={variant: digest(timer / variant / 'lib/asyncfatfs.c') for variant in TIMER_VARIANTS}), indent=2))


def prepare_limited(root):
    """Control application progress per call without changing upstream library code."""
    limited = root / 'upstream-limited'
    shutil.copytree(root / 'upstream/lib', limited / 'upstream/lib', dirs_exist_ok=True)
    harness = (root / 'harness.c').read_text()
    harness = replace_once(harness, '    uint32_t accepted = afatfs_fwrite(file, buffer, length);',
                          '    uint32_t accepted = afatfs_fwrite(file, buffer, length > SECTOR_BYTES ? SECTOR_BYTES : length);')
    (limited / 'harness.c').write_text(harness)
    shutil.copyfile(root / 'portability.h', limited / 'portability.h')


def prepare_snapshot(root):
    """Check whether completion-time buffer sampling changes the metadata comparison."""
    target = root / 'submission-snapshot'
    for variant in VARIANTS:
        shutil.copytree(root / variant / 'lib', target / variant / 'lib', dirs_exist_ok=True)
    harness = (root / 'harness.c').read_text()
    harness = replace_once(harness, 'static benchmarkStats_t statistics;',
                          'static benchmarkStats_t statistics;\nstatic uint8_t comparisonWriteSnapshot[SECTOR_BYTES];\nstatic uint64_t comparisonInflightMutations[4];')
    harness = replace_once(harness, '    transferBuffer = buffer;', '''    transferBuffer = buffer;
    if (writing) {
        memcpy(comparisonWriteSnapshot, buffer, SECTOR_BYTES);
    }''')
    marker = '        if (transferWrite) {'
    beginning = harness.index(marker)
    opening = harness.index('{', beginning)
    ending = block_end(harness, opening)
    body = harness[opening + 1:ending - 1].replace('transferBuffer', 'comparisonWriteSnapshot')
    prefix = '''
            if (memcmp(comparisonWriteSnapshot, transferBuffer, SECTOR_BYTES)) {
                uint32_t fatSectors = (dataStart - 2) / 2;
                unsigned kind = transferSector >= 2 && transferSector < dataStart
                    ? (transferSector - 2) / fatSectors : (transferSector < dataStart + rootSectors ? 2 : 3);
                comparisonInflightMutations[kind]++;
            }
'''
    harness = harness[:opening + 1] + prefix + body + harness[ending - 1:]
    marker = '    fprintf(stderr, "ACCEPTED_TOTALS,'
    harness = replace_once(harness, marker, r'''
    fprintf(stderr, "INFLIGHT_MUTATIONS,%s,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu\n", phase, latencyProfile,
        clusterSectors, chunkBytes, fileCount, repetition,
        (unsigned long long)comparisonInflightMutations[0], (unsigned long long)comparisonInflightMutations[1],
        (unsigned long long)comparisonInflightMutations[2], (unsigned long long)comparisonInflightMutations[3]);
    memset(comparisonInflightMutations, 0, sizeof(comparisonInflightMutations));
    fprintf(stderr, "ACCEPTED_TOTALS,''')
    (target / 'harness.c').write_text(harness)
    shutil.copyfile(root / 'portability.h', target / 'portability.h')


def read_transitions(directory, dataset):
    pending = []
    primary_phases = Counter()
    eoc_sectors = Counter()
    for line in (directory / 'events.log').read_text().splitlines():
        parts = line.split(',')
        if parts[0] == 'FAT_EDGE':
            pending.append(dict(copy=int(parts[1]), image=int(parts[2]), sector=int(parts[3]),
                                accepted=list(map(int, parts[4:7])), entries=parts[7:]))
        elif parts[0] == 'MSUB' and parts[1] == '1':
            primary_phases[f'{parts[10]}/{parts[11]}'] += 1
            if parts[10:12] == ['APPEND', '7']:
                eoc_sectors[parts[2]] += 1
        elif parts[0] == 'FAT_TRANSITIONS':
            key = '/'.join(parts[1:6])
            copy, images = map(int, parts[7:9])
            counts = dict(zip(KINDS, map(int, parts[9:])))
            assert len(parts[9:]) == len(KINDS)
            histogram = dataset[key]['histogram'][str(copy)]
            assert images == sum(histogram.values())
            assert sum(counts.values()) == sum(int(changes) * count for changes, count in histogram.items())
            dataset[key].setdefault('transitions', {})[str(copy)] = dict(images=images, counts=counts)
            if copy == 0:
                dataset[key]['primary_submission_file0_phases'] = dict(primary_phases)
                dataset[key]['file0_eoc_submission_sectors'] = dict(eoc_sectors)
                primary_phases.clear()
                eoc_sectors.clear()
            dataset[key].setdefault('first_images', []).extend(record for record in pending if record['copy'] == copy)
            pending = [record for record in pending if record['copy'] != copy]
        elif parts[0] == 'ACCEPTED_TOTALS' and parts[1] == 'write':
            key = '/'.join(parts[1:6])
            files = int(parts[5])
            accepted = list(map(int, parts[7:]))
            assert accepted == [1048576] * files + [0] * (3 - files)
            dataset[key]['accepted'] = accepted
    assert not pending
    assert not primary_phases


def publish(root, output):
    manifest = json.loads((root / 'manifest.json').read_text())
    assert digest(Path('lib/asyncfatfs.c')) == manifest['production_sha256']
    prior = json.loads(Path('tests/POLL_METADATA.json').read_text())['datasets']
    datasets = {}
    for schedule in ('serial', 'interleaved'):
        for variant in VARIANTS:
            directory = root / f'{schedule}-{variant}'
            dataset = load(directory, variant != 'upstream')
            read_transitions(directory, dataset)
            if variant != 'upstream':
                attribution = read_poll(directory)
                for key in dataset:
                    dataset[key]['poll'] = attribution[key]
            for key, row in dataset.items():
                expected = prior[f'{schedule}-{variant}'][key]['counters']
                assert row['counters'] == expected, (schedule, variant, key)
            datasets[f'{schedule}-{variant}'] = dataset
    timer_datasets = {}
    for schedule in ('serial', 'interleaved'):
        for variant in TIMER_VARIANTS:
            directory = root / f'timer/{schedule}-{variant}'
            dataset = load(directory, True)
            read_transitions(directory, dataset)
            for key, row in dataset.items():
                baseline = datasets[f'{schedule}-base'][key]
                assert row['metadata'] == baseline['metadata'], (schedule, variant, key)
                assert row['transitions'] == baseline['transitions'], (schedule, variant, key)
            timer_datasets[f'{schedule}-{variant}'] = dataset
    limited_datasets = {}
    assert digest(root / 'upstream-limited/upstream/lib/asyncfatfs.c') == digest(root / 'upstream/lib/asyncfatfs.c')
    for schedule in ('serial', 'interleaved'):
        directory = root / f'upstream-limited/{schedule}-upstream'
        dataset = load(directory, False)
        read_transitions(directory, dataset)
        limited_datasets[schedule] = dataset
    snapshot_datasets = {}
    for schedule in ('serial', 'interleaved'):
        for variant in VARIANTS:
            assert digest(root / f'submission-snapshot/{variant}/lib/asyncfatfs.c') == digest(root / f'{variant}/lib/asyncfatfs.c')
            directory = root / f'submission-snapshot/{schedule}-{variant}'
            dataset = load(directory, variant != 'upstream')
            read_transitions(directory, dataset)
            for line in (directory / 'events.log').read_text().splitlines():
                parts = line.split(',')
                if parts[0] == 'INFLIGHT_MUTATIONS':
                    key = '/'.join(parts[1:6])
                    dataset[key]['inflight_mutations'] = dict(zip(('primary', 'mirror', 'directory', 'data'), map(int, parts[7:])))
            for key, row in dataset.items():
                baseline = datasets[f'{schedule}-{variant}'][key]
                assert row['metadata'] == baseline['metadata'], (schedule, variant, key)
                if variant != 'upstream':
                    assert row['transitions'] == baseline['transitions']
                    assert not any(row['inflight_mutations'].values())
            snapshot_datasets[f'{schedule}-{variant}'] = dataset
    report = dict(manifest=manifest, datasets=datasets, timer_datasets=timer_datasets,
                  limited_upstream=limited_datasets, submission_snapshot=snapshot_datasets)
    single = datasets['serial-base']['write/0/1/4096/1']
    assert single['primary_submission_file0_phases'] == {'APPEND/7': 17, 'APPEND/4': 2047, 'CLOSE/-1': 1}
    assert len(single['file0_eoc_submission_sectors']) == 17
    assert set(single['file0_eoc_submission_sectors'].values()) == {1}
    lines = ['# Amplificação de metadados: transições FAT realmente gravadas', '',
             f"FAT32 8.3, cache oito setores, clusters 512 bytes. Upstream/master local `{manifest['upstream_commit']}`. Somente contagem de metadados; nenhum resultado de CPU nesta rodada. Produção inalterada.", '',
             'São transições entre a imagem anterior no dispositivo simulado e cada imagem concluída, não contagens de chamadas FATSetNextCluster. FREE→LINK significa que um EOC intermediário não chegou a ser persistido naquela entrada antes do vínculo. Cada registro de FAT1/FAT2 representa um setor de 512 bytes.', '']
    summary = ['## Razão da diferença', '',
               'A FAT2 explica só parte do excesso. No serial 2/4096, o excesso de 6863 setores de metadados se decompõe exatamente em **+3414 FAT1 +4160 FAT2 −711 diretório**. A contagem maior de FAT1 vem do avanço limitado por confirmações a cada cluster e da persistência separada de estados EOC intermediários, enquanto o upstream acumula mais alterações antes de enviar uma imagem.', '',
               'Exemplo serial 1/4096: a atual persiste 2049 transições FREE→EOC e 2047 EOC→LINK, além de uma liberação da reserva. O upstream persiste 362 FREE→EOC, **1686 FREE→LINK diretamente** e 361 EOC→LINK. Assim, muitas entradas do upstream chegam ao dispositivo já com seu vínculo final, sem uma imagem EOC intermediária separada. A atual registra 4097 transições em 2065 imagens; upstream 2409 em 378. As médias são 1,984 e 6,373 entradas alteradas por imagem.', '',
               'O traço inicial mostra a causa no avanço: na atual, a imagem 0 confirma EOC dos clusters 3 e 4 antes de retornar dados aceitos; após 512 bytes, a imagem 1 publica 3→4 e reserva EOC de 5; após 1024 bytes, a imagem 2 publica 4→5 e reserva EOC de 6. O ciclo se repete por cluster. O upstream aceita 3072 bytes antes da primeira conclusão FAT1 e essa imagem já contém 3→4→5→6→7→8→9→EOC. A reserva única atual já compartilha um EOC futuro com o vínculo anterior, mas não transforma esse fluxo em um lote de vários clusters por confirmação.', '',
               'A contagem atual 2065 fecha exatamente por fase: **17 imagens em NEW_CLUSTER_DURABLE +2047 em FAT_DURABLE +1 no fechamento**. As 17 imagens EOC correspondem a 17 setores FAT distintos (inicial e 16 mudanças de página FAT); o fechamento devolve a reserva não utilizada. São 2048 clusters de dados: 2048 +16 mudanças de página +1 limpeza =2065 imagens FAT1. A quantidade permanece igual com pedidos de 257 ou 4096 bytes.', '',
               'No código atual, NEW_CLUSTER_DURABLE confirma EOC nas duas FATs; UPDATE_FAT2 escreve o vínculo e uma eventual reserva seguinte; FAT_DURABLE drena novamente antes da publicação do diretório/conclusão. No upstream, UPDATE_FAT1 e UPDATE_FAT2 seguem para o diretório e COMPLETE quando as páginas estão modificadas no cache, sem essas duas fases explícitas de ACK. Essa diferença de protocolo reduz o agrupamento na atual. Remover os ACKs para imitar a contagem upstream não seria uma otimização com as mesmas garantias.', '',
               'A observação sobre pre_background está correta como local do envio: no serial 2/4096 ele submete **8257 de 8324 metadados**. O controle sem background, porém, mantém **4160 FAT1 +4160 FAT2** e transfere essas 8320 gravações FAT para first_operations. O único setor poupado é de diretório. Portanto o excesso de FAT não nasce do número de chamadas pre_background; ele já é imposto pelas páginas/barreiras que ficam pendentes entre os clusters.', '',
               'Não há atribuição de CPU nesta análise. Um custo de CPU associado ao I/O adicional continua possível; o nome do ponto de envio não identifica sozinho a causa de esse I/O existir.', '']
    lines[6:6] = summary
    for schedule in ('serial', 'interleaved'):
        lines += [f'## {schedule}, latência 0', '',
                  '| Escritores/bloco | Versão | FAT1 | FAT2 | Diretório | FREE→EOC FAT1 | FREE→LINK FAT1 | EOC→LINK FAT1 | EOC→FREE FAT1 | Entradas alteradas/FAT1 |',
                  '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|']
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/0/1/{chunk}/{files}'
            for variant in VARIANTS:
                row = datasets[f'{schedule}-{variant}'][key]
                meta = row['metadata']
                trans = row['transitions']['0']['counts']
                mean = sum(trans.values()) / meta['primary']
                lines.append(f"| {files}/{chunk} | {variant} | {meta['primary']} | {meta['mirror']} | {meta['directory']} | {trans['free_to_eoc']} | {trans['free_to_link']} | {trans['eoc_to_link']} | {trans['eoc_to_free']} | {mean:.3f} |")
        lines += ['', '### Primeiras imagens FAT1: um escritor/4096 bytes', '',
                  '| Versão | Imagem | Bytes já aceitos pela aplicação | Alterações cluster:anterior→novo |', '|---|---:|---:|---|']
        for variant in ('base', 'upstream'):
            row = datasets[f'{schedule}-{variant}']['write/0/1/4096/1']
            for record in row['first_images']:
                if record['copy'] == 0 and record['image'] < 6:
                    lines.append(f"| {variant} | {record['image']} | {record['accepted'][0]} | {'; '.join(record['entries'])} |")
        lines.append('')
    lines += ['## cacheTimer: o que os 64 significam', '',
              'releaseIdleReservation incrementa cacheTimer por poll que pode avançar, além dos incrementos nos acessos reais ao cache. Em deferDirectoryPage, cacheTimer − accessTimestamp >= 64 desativa o adiamento de background do diretório. Isso torna a página elegível; não obriga uma transferência imediata se o backend está ocupado ou outra página elegível é mais antiga. O limiar não é a política de elegibilidade das FATs, que já podem ser enviadas antes de 64. Flushes explícitos/barreiras não usam essa deferral.', '',
              'Foram separados quatro controles: base; sem incremento por poll; sem liberação em idle (mantendo o incremento); sem ambos. **Nos 16 casos de escrita, as contagens de FAT1, FAT2, diretório e transições são idênticas entre os quatro.** Portanto esse incremento não explica o excesso destes workloads contínuos. Em workloads com pausas, envelhecer o diretório ou devolver uma reserva não utilizada pode provocar I/O; isso não foi quantificado como workload de idle nesta rodada.', '',
              '## Controle upstream com retorno limitado a 512 bytes por chamada', '',
              'A biblioteca upstream é byte-idêntica; somente o wrapper da aplicação limita cada chamada efetiva a 512 bytes, preservando os pedidos externos de 257/4096 e seu pump após cada tentativa. É um controle do progresso permitido entre polls, sem adicionar FAT2 ou barreiras à biblioteca upstream.', '',
              '| Ritmo | Bloco/escritores | FAT1 upstream normal → limitado | Diretório normal → limitado | FAT1 atual |', '|---|---|---:|---:|---:|']
    for schedule in ('serial', 'interleaved'):
        for chunk, files in ((257, 1), (257, 2), (4096, 1), (4096, 2)):
            key = f'write/0/1/{chunk}/{files}'
            normal = datasets[f'{schedule}-upstream'][key]['metadata']
            limited = limited_datasets[schedule][key]['metadata']
            actual = datasets[f'{schedule}-base'][key]['metadata']
            lines.append(f"| {schedule} | {chunk}/{files} | {normal['primary']} → {limited['primary']} | {normal['directory']} → {limited['directory']} | {actual['primary']} |")
    lines += ['', 'O limite de 512 bytes no wrapper não reproduz as confirmações da atual: um escritor/4096 passa de 378 para apenas 450 gravações FAT1, ainda muito abaixo de 2065. Portanto tamanho de retorno/pedido isolado não explica a diferença; o upstream continua avançando operações e acumulando alterações sem aguardar as mesmas barreiras de ACK.', '',
              '## Controle da imagem capturada pelo backend', '',
              'O backend histórico lê o buffer na conclusão. Foi repetido o estudo congelando os 512 bytes na submissão e mantendo o ponteiro original para o callback. **As contagens de FAT1, FAT2 e diretório permaneceram iguais em todos os 48 casos desse controle.** Logo, a diferença de quantidade de metadados não depende desse momento de captura.', '',
              'Na atual não houve mudança de buffer durante o I/O nesses workloads. No upstream houve mudanças de FAT/diretório em trânsito; algumas categorias de transição diferem entre os dois modelos, e ambos estão no JSON. Os números destacados de transições para um escritor/4096, latência 0, são iguais nos dois modelos. Este controle não altera o código das bibliotecas nem mede CPU.', '',
              '## Validação', '',
              'Passaram 48 cenários de escrita e 48 releituras: três versões, dois ritmos, duas latências, um/dois arquivos de 1 MiB e 257/4096 bytes. Nas variantes atuais foram auditados EOC/vínculo/diretório por gravação e igualdade das FATs; upstream recebeu verificação de conteúdo. Todas as contagens de I/O reproduzem POLL_METADATA, incluindo as leituras de controle. Histogramas e soma das categorias de transição conferem para cada imagem/cópia/caso. Bytes retornados por fwrite conferem integralmente com os payloads.', '',
              'Os controles de cacheTimer acrescentam 64 cenários de escrita/releitura; o limite de retorno upstream acrescenta outros 16; a captura na submissão acrescenta 48. Total: **176 cenários de escrita e 176 releituras**. As contagens dos controles de timer são reconciliadas com as mesmas categorias/histogramas, sem aumento de RAM de produção.', '',
              'Instrumentação somente nas cópias tests/build/fat-transition. Sem testes de CPU nem injeção de falhas; RAM/pilha/persistência de produção permanecem as anteriores. O fluxo no_background é controle diagnóstico e não deve ser adotado como política de idle.', '',
              '```powershell', 'python tests/fat_transition_audit.py --prepare',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition -Variants base,no_background,upstream',
              'python tests/fat_transition_audit.py --prepare-timer',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/timer -Variants base,no_poll_age,no_idle_release,no_age_or_release',
              'python tests/fat_transition_audit.py --prepare-limited',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/upstream-limited -Variants upstream',
              'python tests/fat_transition_audit.py --prepare-snapshot',
              './tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/submission-snapshot -Variants base,no_background,upstream',
              'python tests/fat_transition_audit.py', '```']
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path('tests/build/fat-transition'))
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--prepare-timer', action='store_true')
    parser.add_argument('--prepare-limited', action='store_true')
    parser.add_argument('--prepare-snapshot', action='store_true')
    parser.add_argument('--output', type=Path, default=Path('tests/FAT_METADATA_TRANSITIONS.md'))
    arguments = parser.parse_args()
    if arguments.prepare:
        prepare(arguments.directory)
    elif arguments.prepare_timer:
        prepare_timer(arguments.directory)
    elif arguments.prepare_limited:
        prepare_limited(arguments.directory)
    elif arguments.prepare_snapshot:
        prepare_snapshot(arguments.directory)
    else:
        publish(arguments.directory, arguments.output)
