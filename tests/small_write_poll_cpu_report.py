"""Publish bounded collection-poll CPU trials, RAM/layout, I/O and recovery evidence."""
import json
from pathlib import Path
import re

from small_write_cpu_validation import validate_final


def constants(path, symbol):
    """Read a uint16_t constant array from an XC16 assembly listing."""
    source = path.read_text(encoding='utf-8-sig')
    count = int(re.search(r'\.size\s+_' + re.escape(symbol) + r',\s*(\d+)', source)[1]) // 2
    body = source.split(f'_{symbol}:', 1)[1]
    values = []
    for line in body.splitlines():
        if match := re.match(r'\s*\.word\s+(\d+)', line):
            values.append(int(match[1]))
        elif match := re.match(r'\s*\.skip\s+(\d+)', line):
            values.extend([0] * (int(match[1]) // 2))
        elif line.strip() and values:
            break
        if len(values) == count:
            break
    assert len(values) == count, (path, symbol)
    return values


root = Path('tests/build/small-write-poll-cpu/final-dispatch')
report = validate_final(root)
results = report['results']
paired = report['paired_cpu_delta_percent']
layouts = {}
for abi in ('32', '64', '16-normal', '16-short'):
    layouts[abi] = {}
    for version in ('before', 'normal'):
        directory = root / f'layout-{version}-{abi}'
        assert f'/{version}/lib/asyncfatfs.c' in (directory / 'probe.c').read_text()
        if abi in ('32', '64'):
            log = (directory / 'results.log').read_text()
            assert 'PASS C99 Boolean' in log
            size = list(map(int, next(line for line in log.splitlines() if re.fullmatch(r'\d+(,\d+)+', line)).split(',')))
            operations = list(map(int, next(line for line in log.splitlines() if line.startswith('OPERATIONS,')).split(',')[1:]))
        else:
            size = constants(directory / 'layout.s', 'afatfs_layout_sizes')
            operations = constants(directory / 'layout.s', 'afatfs_operation_layout_sizes')
        layouts[abi][version] = dict(global_state=size[2], handle=size[3], append=operations[1], extend=operations[2])
    assert layouts[abi]['before']['global_state'] == layouts[abi]['normal']['global_state']
    assert layouts[abi]['before']['handle'] == layouts[abi]['normal']['handle']
    assert layouts[abi]['before'] == layouts[abi]['normal']
test_source = (root / 'validation/tests/fat32_cache_mirror.c').read_text()
assert all(call in test_source for call in ('checkCollectionIdlePoll();', 'checkCollectedPair(0, 1);',
    'checkCollectedPair(1, 0);', 'checkCollectedPair(6, 7);'))
report['layouts'] = layouts
rejected = json.loads((root.parent / 'final/rejected-report.json').read_text())
report['rejected_byte_counter'] = dict(source=rejected['manifest']['production']['asyncfatfs.c'],
    paired_cpu_delta_percent=rejected['paired_cpu_delta_percent']['interleaved'])
lines = ['# CPU: despacho único de append e busca limitada do par FAT', '',
    f"Upstream local: `{report['manifest']['upstream']}`. Fonte atual SHA-256: `{report['manifest']['production']['asyncfatfs.c']}`.", '',
    'A é a versão anterior com filtragem antecipada do flush; B é a combinação mantida abaixo. '
    'Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.', '',
    'O ganho mais consistente está em dois escritores/257 bytes: padrão serial '
    f"{paired['serial']['write/0/1/257/2']:+.2f}% e intercalado "
    f"{paired['interleaved']['write/0/1/257/2']:+.2f}% de CPU pareada. "
    'Com um escritor, os resultados são pequenos ou '
    'dependem da dispersão; não há ganho geral comprovado. Razão das medianas e delta '
    'pareado podem discordar no sinal, como no padrão serial de um escritor. '
    'As regressões fora do alvo também estão registradas.', '',
    '## Alterações e persistência', '',
    'collectIdle conserva o campo original de seis bits. A alternativa uint8_t coube na união '
    'sem aumentar RAM, mas não apresentou ganho consistente com um escritor: a primeira '
    'confirmação marcou +1,57% no padrão intercalado. Foi descartada. Seus dados completos '
    'permanecem em tests/build/small-write-poll-cpu/final/rejected-report.md e JSON. '
    'Todos os tamanhos medidos de estado, handle, append e extensão são iguais na versão mantida.', '',
    'afatfs_fileOperationsPoll classifica cada operação ativa uma vez. Um append válido '
    'usa diretamente sua continuação nos perfis nativos, sem repetir o despacho genérico. '
    'A espera da fase COLLECT é tratada no mesmo ramo: backend ocupado não avança o contador; '
    'sem backend pendente, incrementa uma vez '
    'por passe enquanto o valor é diferente de 63. No limite, usa a continuação completa original, '
    'que sela todas as coleções e aguarda ACKs. Operações de sync/close e handles inválidos '
    'continuam no despacho original. Dois passes consecutivos continuam contando dois passes.', '',
    'afatfs_collectNext encerra a varredura assim que encontra as duas páginas DIRTY do par. '
    'A aquisição em estados ausente/limpo/em transferência, os três acessos lógicos do cache '
    'e as cópias de quatro bytes versus reparo integral permanecem. Não muda limites da janela, '
    'reservas, cacheTimer, submissões, ACKs ou a ordem EOC nas duas FATs → vínculos → diretório.', '',
    '## Método', '',
    'MSVC x64 /O2, 21 repetições alternadas, afinidade fixa, aquecimento e sem compilações '
    'concorrentes durante CPU. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, blocos 257/4096, '
    'clusters 512/4096, serial/intercalado e padrão/multiread/query. QueryThreadCycleTime inclui '
    'biblioteca, backend RAM, polling e aplicação; throughput host usa tempo real. '
    'Não são medidas de PIC24 ou cartão físico. Leitura CPU usa 16 passagens. '
    'Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. '
    'Amostras e quartis estão no JSON; diferenças pequenas podem ser ruído.', '',
    'Upstream original usa mínimo multibloco quatro; atual dois. O controle anterior indicou '
    'que esse limiar não explica a diferença principal. Upstream não grava FAT2 neste workload '
    'e termina com espelhos diferentes; as garantias de espelhamento não são equivalentes.', '',
    '## Escritas de 257 bytes — clusters de 512 bytes', '',
    '| Ritmo | Perfil | Escritores | CPU A → B ciclos/B | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |',
    '|---|---|---:|---:|---:|---:|---:|---:|---:|']
for schedule in ('serial', 'interleaved'):
    for profile in ('', '_multi', '_query'):
        for files in (1, 2):
            key = f'write/0/1/257/{files}'
            before = results[f'{schedule}-before{profile}']['cpu'][key]
            after = results[f'{schedule}-after{profile}']['cpu'][key]
            upstream = results[f'{schedule}-upstream']['cpu'][key]['median']
            ratio = (after['median'] / before['median'] - 1) * 100
            excess = f"{(before['median']/upstream-1)*100:+.2f}% → {(after['median']/upstream-1)*100:+.2f}%"
            lines.append(f"| {schedule} | {profile or 'padrão'} | {files} | {before['median']:.3f} → {after['median']:.3f} | {paired[schedule+profile][key]:+.2f}% | {ratio:+.2f}% | {upstream:.3f} | {excess} | {before['host_mib_s']:.2f} → {after['host_mib_s']:.2f} |")
lines += ['', '## Demais cenários — Δ CPU pareado B/A', '',
    'Inclui variações e regressões; não generalizar um ganho a todos os caminhos.', '',
    '| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |', '|---|---|---:|---|---:|']
for name, records in paired.items():
    for key, change in records.items():
        phase, _, cluster, chunk, files = key.split('/')
        if phase == 'write' and cluster == '1' and chunk == '257':
            continue
        lines.append(f'| {name} | {phase} | {int(cluster)*512} | {files}/{chunk} | {change:+.2f}% |')
lines += ['', '## I/O e recursos', '',
    'Os 192 pares auditados A/B coincidem em leituras, escritas, metadados por FAT/diretório, '
    'polls e sessões/blocos multibloco nas duas latências. Throughput virtual igual. '
    'Conteúdo integral, FATs iguais e ordem de publicação conferidos.', '',
    '| Latência | Escritores/bloco | R | W | Metadados |', '|---:|---|---:|---:|---:|']
for latency in (0, 1):
    for files, chunk in ((1, 257), (2, 257), (1, 4096), (2, 4096)):
        record = results['interleaved-after']['io'][f'write/{latency}/1/{chunk}/{files}']
        lines.append(f"| {latency} | {files}/{chunk} | {record['reads']} | {record['writes']} | {record['metadata_writes']} |")
lines += ['', '| ABI | RAM A → B bytes | Código A → B bytes |', '|---|---:|---:|']
for name, record in report['resources'].items():
    lines.append(f"| {name} | {record['before']['static_ram_bytes']} → {record['after']['static_ram_bytes']} | {record['before']['code_bytes']} → {record['after']['code_bytes']} |")
lines += ['', '| ABI | Estado global A → B | Handle A → B | Append A → B | Extensão A → B |', '|---|---:|---:|---:|---:|']
for abi, records in layouts.items():
    cells = ' | '.join(f"{records['before'][field]} → {records['normal'][field]}" for field in ('global_state', 'handle', 'append', 'extend'))
    lines.append(f'| {abi} | {cells} |')
lines += ['', f"Matriz PIC24: 48 configurações com seções estáticas iguais; pilha interna máxima **{report['peak_stack_bytes']} bytes**, "
    'incluindo PCs internos e excluindo backend/aplicação/libc. Os quatro probes host passaram '
    'nos checks de flags/alinhamento; os quatro probes XC16 compilaram os checks de alinhamento.', '',
    '## Validação de persistência', '',
    'Passaram 28 testes de alocação/cache. O teste dirigido percorre todos os incrementos 0..62, '
    'todos os valores 0..63 com backend ocupado, handles inválidos e operação NONE. '
    'Confere dois passes consecutivos: 62 →63 e depois selamento de todos os escritores. '
    'Duas submissões de dados rejeitadas mantêm ambos aguardando a barreira, sem avanço indevido. '
    'Os nove cenários do par FAT foram repetidos nos slots 0/1, 1/0 e 6/7, cobrindo ambos os '
    'ordenamentos e o fim do cache, nos três perfis. Demais testes: três escritores, quase cheio, '
    'reserva única, fsync imediato, callbacks, prefixos e buffers em trânsito imutáveis. Campanhas:', '']
lines += [f'- {name}: {message}' for name, message in report['recovery'].items()]
lines += ['', f'Evidências congeladas: `{root.as_posix()}`. As alternativas iniciais e a rodada '
    'com contador em byte estão no diretório pai; isoladamente os ganhos foram inconsistentes.']
output = Path('tests/SMALL_WRITE_POLL_CPU.md')
output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
output.with_suffix('.json').write_text(json.dumps(report, indent=2))
print(output)
