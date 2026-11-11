# Memória compartilhada nas operações e na montagem

Comparação com a versão imediatamente anterior, já contendo as uniões do diário descritas em [JOURNAL_LIFETIME_MEMORY.md](JOURNAL_LIFETIME_MEMORY.md). Os números abaixo são economias adicionais, em bytes.

## Tempos de vida

| Área | Campos compartilhados | Transição segura |
| --- | --- | --- |
| Criação | Alias/reserva FAT e contadores exFAT | O tipo do volume fixa qual membro é utilizado. |
| Criação LFN | Parser e contadores de reserva/publicação | Inicializa os contadores após terminar a busca do nome/alias. |
| Truncamento | Contador de clusters, próximo cluster e início do trecho contíguo | A busca termina antes da liberação; liberação normal e devolução ao freefile usam caminhos distintos. |
| Expansão | Conversão da cadeia e setor de zeragem | Inicializa o setor zero após concluir a conversão, antes de publicar o novo cluster. |
| Montagem exFAT | Boot, busca no diretório e decodificação up-case | Reinicializa o workspace a cada transição; metadados da tabela permanecem independentes. |
| Inicialização | Montagem exFAT e busca/preparo do freefile FAT | Inicialização de freefile é pulada no exFAT; montagem exFAT não ocorre em FAT. |

Os valores necessários para retomar I/O pendente continuam válidos entre chamadas. O índice de órfão LFN permanece separado dos contadores de publicação. Nome longo e alias curto coexistem; entradas e posições exFAT precisam sobreviver às operações; cursores e callbacks também não foram sobrepostos. O formato no volume, APIs e seleção padrão FAT/LFN foram preservados.

## Memória global e por arquivo

O global inclui cache, diário e handles internos. Não some economias de componentes ao total global. Os tamanhos de Request, Directory e diário não mudaram.

| Configuração | ABI | Global antes → depois | Economia | Arquivo antes → depois |
| --- | --- | ---: | ---: | ---: |
| default | 16 bits | 4120 → 4112 | 8 | 340 → 338 |
| default | 16 bits, enums curtos | 4100 → 4092 | 8 | 338 → 336 |
| default | 32 bits | 4220 → 4204 | 16 | 356 → 352 |
| default | 64 bits | 4304 → 4272 | 32 | 376 → 368 |
| exfat_freefile | 16 bits | 8348 → 8316 | 32 | 1050 → 1050 |
| exfat_freefile | 16 bits, enums curtos | 8336 → 8304 | 32 | 1050 → 1050 |
| exfat_freefile | 32 bits | 8660 → 8624 | 36 | 1104 → 1104 |
| exfat_freefile | 64 bits | 8792 → 8752 | 40 | 1128 → 1128 |
| exfat_only | 16 bits | 7280 → 7268 | 12 | 1050 → 1050 |
| exfat_only | 16 bits, enums curtos | 7268 → 7256 | 12 | 1050 → 1050 |
| exfat_only | 32 bits | 7536 → 7520 | 16 | 1104 → 1104 |
| exfat_only | 64 bits | 7640 → 7624 | 16 | 1128 → 1128 |
| fat_lfn | 16 bits | 4120 → 4112 | 8 | 340 → 338 |
| fat_lfn | 16 bits, enums curtos | 4100 → 4092 | 8 | 338 → 336 |
| fat_lfn | 32 bits | 4220 → 4204 | 16 | 356 → 352 |
| fat_lfn | 64 bits | 4304 → 4272 | 32 | 376 → 368 |
| fat_only | 16 bits | 2520 → 2488 | 32 | 84 → 76 |
| fat_only | 16 bits, enums curtos | 2500 → 2468 | 32 | 82 → 74 |
| fat_only | 32 bits | 2636 → 2604 | 32 | 104 → 96 |
| fat_only | 64 bits | 2736 → 2704 | 32 | 128 → 120 |
| lfn_exfat | 16 bits | 7424 → 7396 | 28 | 1086 → 1082 |
| lfn_exfat | 16 bits, enums curtos | 7404 → 7376 | 28 | 1084 → 1080 |
| lfn_exfat | 32 bits | 7680 → 7648 | 32 | 1140 → 1136 |
| lfn_exfat | 64 bits | 7800 → 7752 | 48 | 1168 → 1160 |

## Workspaces da configuração padrão

| ABI | Criação | Append | Extend | Truncamento |
| --- | ---: | ---: | ---: | ---: |
| 16 bits | 286 → 284 | 20 → 16 | 30 → 26 | 40 → 32 |
| 16 bits, enums curtos | 286 → 284 | 18 → 14 | 28 → 24 | 38 → 30 |
| 32 bits | 292 → 288 | 24 → 20 | 40 → 36 | 48 → 40 |
| 64 bits | 304 → 296 | 24 → 20 | 48 → 48 | 56 → 48 |

A montagem exFAT usa 32 bytes de scratch, compartilhados com a inicialização do freefile quando habilitado. A tabela ASCII/GPT já compartilhada permanece independente: a tabela precisa coexistir com o decodificador durante a montagem. O ganho global depende do maior membro de cada union e do padding da ABI.

## Validação

- 24 layouts: x86/x64 executados e PIC24/XC16 C99 com enums normais/curtos compilados. Alinhamento natural, buffers alinhados, flags e prefixos de suboperações preservados.
- Regressão em seis configurações, mais compilação dos oito testes legados. APIs de criação/rename/truncamento/expansão, diretórios, GPT/up-case, falhas de I/O, remoção/reinserção, diário e desligamento pendente passaram.
- AddressSanitizer em LFN+exFAT+freefile sem erros de memória.
- FAT32: 4363 cortes nas operações, 2142 cortes no replay, 28 casos de proteção e 86 cortes no provisionamento.
- Benchmark padrão/combinado: contagens de leituras/gravações idênticas às anteriores; conteúdo validado após remontar. Esta mudança reduz RAM, sem ganho de I/O medido.

Os testes usam backend simulado e imagens regulares; PIC24 foi compilado sem execução no hardware. Nenhum novo VHD ou cartão físico foi usado nesta etapa.

## Reprodução

```powershell
# Guarde lib/ em tests/build/operation-lifetime/before-lib antes de alterar.
# Rode o probe no checkout anterior para gerar layout-before/report.json.
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/operation-lifetime/layout-after
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/operation-lifetime/powercut -Torn
./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/operation-lifetime/before-lib -OutputDirectory tests/build/operation-lifetime/benchmark
python tests/operation_lifetime_report.py
```

O JSON ao lado guarda os 24 casos, componentes e hashes das fontes/baseline/evidências. Os tamanhos medidos dependem da configuração e ABI do compilador.
