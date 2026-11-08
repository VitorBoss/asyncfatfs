# Agrupamento de I/O do cache

Baseline: versão das uniões por tempo de vida registrada em OPERATION_LIFETIME_MEMORY.json. A seleção padrão continua FAT/LFN; o formato no volume e as APIs foram preservados.

## Política

O poll adia o escoamento oportunista nas fases de produção de entradas LFN/exFAT, alocação FAT32 protegida pelo diário, conversão de cadeia e liberação de clusters. Barreiras explícitas e falta de cache continuam drenando setores. Quando blockAllocation aguarda checkpoint, o escoamento é permitido, evitando espera circular.

A seleção de setores mantém a ordem de writeTimestamp. Se os próximos setores sujos e desbloqueados nessa ordem forem também consecutivos fisicamente, usa AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT. Não há reordenação para formar um lote. Leituras ou endereços diferentes podem interromper a sequência conforme o contrato do backend.

Metadados não anunciam setores futuros fora do cache: FATFillWithPattern podia repassar essa indicação antes de capturar suas imagens undo. Um backend que pré-apaga a faixa poderia destruir metadados ainda não protegidos. A indicação futura do freefile continua disponível para append de dados; novos lotes são limitados aos buffers prontos.

O retorno de beginWriteBlocks agora é verificado: BUSY/FAILURE preservam o setor sujo e não submetem o bloco; FAILURE registra BACKEND_IO. Sync, close, flush, publicação e shutdown mantêm suas barreiras. Falhas deixam canPowerOff falso enquanto houver dados pendentes.

## Setores gravados por operação FAT32/LFN

As contagens incluem o diário. O mesmo workload e backend foram usados antes/depois.

| Cluster (bytes) | Operação | Antes | Depois |
| ---: | --- | ---: | ---: |
| 512 | create | 58 | 40 |
| 512 | create-data | 68 | 50 |
| 512 | rename | 54 | 45 |
| 512 | rename-empty | 54 | 45 |
| 512 | rename-same | 30 | 30 |
| 512 | rename-same-empty | 30 | 30 |
| 512 | delete | 45 | 45 |
| 512 | delete-empty | 23 | 23 |
| 4096 | create | 24 | 23 |
| 4096 | create-data | 37 | 36 |
| 4096 | rename | 35 | 34 |
| 4096 | rename-empty | 35 | 34 |
| 4096 | rename-same | 30 | 30 |
| 4096 | rename-same-empty | 30 | 30 |
| 4096 | delete | 45 | 45 |
| 4096 | delete-empty | 23 | 23 |

## Gravação de 64 KiB pré-alocados

| Sistema | Leituras antes/depois | Setores gravados antes/depois | Inícios de sequência antes → depois | Blocos agrupados depois |
| --- | ---: | ---: | ---: | ---: |
| fat32 | 37 / 37 | 137 / 137 | 137 → 43 | 127 |
| exfat | 2 / 2 | 136 / 136 | 136 → 51 | 127 |

Inícios são sequências múltiplas mais escritas isoladas no backend de teste. As chamadas writeBlock e o número de setores de payload continuam necessários. 127 dos 128 setores de dados foram agrupados; os demais totais incluem metadados/diário. O teste valida o conteúdo após remontar. Não há medição de tempo físico de cartão SD.

O benchmark anterior de streaming (um/dois escritores, FAT32/exFAT) preservou exatamente suas leituras, gravações e I/O de montagem. Adiar toda a alocação, inclusive a busca por clusters, foi descartado porque aumentava leituras.

## Limites e validação

- FATs espelhadas geralmente não são adjacentes na ordem de gravação; não são reordenadas para montar lotes.
- Bitmap, FAT, publicação de diretórios, dados/VDL e commit mantêm as dependências de persistência existentes.
- Imagem, registro e cabeçalhos do diário mantêm a ordem e o buffer único; não foram agrupados especulativamente.
- Nenhum campo, buffer ou alocação dinâmica foi adicionado à biblioteca. Os 24 layouts de 16/32/64 bits permaneceram idênticos, incluindo alinhamento PIC24 com enums normais/curtos.
- Seis configurações de regressão, oito testes legados compilados e AddressSanitizer LFN+exFAT+freefile passaram.
- Backend com pré-apagamento destrutivo: faixas consecutivas, ordem temporal não consecutiva, setores bloqueados, interrupção por leitura, begin BUSY/FAILURE e write submit/ACK inválidos; buffers pendentes e canPowerOff verificados. Também executado com múltiplos blocos desabilitados.
- FAT32: 3957 cortes de operação, 2142 cortes no replay, 28 proteções e 86 cortes de provisionamento passaram.

Os cortes usam imagens regulares e backend de rastreamento; o teste destrutivo de pré-apagamento é separado. PIC24 foi compilado sem execução física. Não foi usado novo VHD.

## Reprodução

```powershell
./tests/run_io_batching_tests.ps1 -BaselineDirectory tests/build/io-batching/before-lib
./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/io-batching/before-lib -OutputDirectory tests/build/io-batching/benchmark
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/io-batching/layout
./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/io-batching/powercut -Torn
python tests/io_batching_report.py
```

Guarde lib/ em before-lib antes da mudança. A baseline de layouts/cortes é tests/build/operation-lifetime. O JSON ao lado registra fontes e evidências por hash.
