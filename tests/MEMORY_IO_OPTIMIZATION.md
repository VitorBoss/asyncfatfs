# Otimizações de memória e I/O de metadados

A reorganização posterior das variáveis por tempo de vida e suas medições adicionais estão em
[JOURNAL_LIFETIME_MEMORY.md](JOURNAL_LIFETIME_MEMORY.md).

Comparação com a revisão fc6a2939, já contendo o diário FAT32/exFAT compartilhado. A configuração padrão continua FAT/LFN, sem exFAT. Valores em bytes e setores de 512 bytes.

## Memória estática

| Configuração | ABI | Antes | Depois | Economia |
| --- | --- | ---: | ---: | ---: |
| default | 16 bits | 4140 | 4128 | 12 |
| default | 16 bits, enums curtos | 4120 | 4108 | 12 |
| default | 32 bits | 4244 | 4232 | 12 |
| default | 64 bits | 4328 | 4320 | 8 |
| exfat_freefile | 16 bits | 9424 | 8356 | 1068 |
| exfat_freefile | 16 bits, enums curtos | 9412 | 8344 | 1068 |
| exfat_freefile | 32 bits | 9792 | 8672 | 1120 |
| exfat_freefile | 64 bits | 9944 | 8808 | 1136 |
| exfat_only | 16 bits | 8356 | 7288 | 1068 |
| exfat_only | 16 bits, enums curtos | 8344 | 7276 | 1068 |
| exfat_only | 32 bits | 8668 | 7548 | 1120 |
| exfat_only | 64 bits | 8800 | 7656 | 1144 |
| fat_lfn | 16 bits | 4140 | 4128 | 12 |
| fat_lfn | 16 bits, enums curtos | 4120 | 4108 | 12 |
| fat_lfn | 32 bits | 4244 | 4232 | 12 |
| fat_lfn | 64 bits | 4328 | 4320 | 8 |
| fat_only | 16 bits | 2520 | 2520 | 0 |
| fat_only | 16 bits, enums curtos | 2500 | 2500 | 0 |
| fat_only | 32 bits | 2636 | 2636 | 0 |
| fat_only | 64 bits | 2736 | 2736 | 0 |
| lfn_exfat | 16 bits | 8536 | 7432 | 1104 |
| lfn_exfat | 16 bits, enums curtos | 8512 | 7412 | 1100 |
| lfn_exfat | 32 bits | 8848 | 7692 | 1156 |
| lfn_exfat | 64 bits | 9000 | 7816 | 1184 |

`afatfs_t` inclui cache e handles internos: não some componentes novamente. As estruturas de arquivo, diretório, request e suboperação de truncamento não cresceram. O controlador do diário diminuiu 12 bytes: candidato da reserva, setor pendente de captura e destino de replay compartilham uma união; comprimento da reserva e índice de busca compartilham outra. Essas fases nunca se sobrepõem.

O exFAT passou a usar temporariamente `openFiles[0]` durante a montagem e o devolve antes de READY, inclusive em saídas protegidas e GPT somente leitura. Isso elimina um handle permanente inteiro. O limite público de arquivos permanece igual; as proteções do nome e da alocação do diário não dependem de um endereço fixo de handle. O serial exFAT também deixou de existir em duplicata e usa o campo já presente no diário.

Foram verificadas seis configurações em x86/x64 e PIC24/XC16 com enums normais/curtos (24 casos), incluindo bitfields C99, alinhamento natural das uniões, buffers alinhados a quatro bytes e prefixos de suboperações. Os resultados de 16 bits são compilação cruzada real, sem execução em hardware. Não há heap, buffers de setor adicionais ou alteração dos registros em disco.

## I/O medido

Benchmark de 64 KiB por arquivo, um ou dois escritores alternados em blocos de 257 bytes, clusters de 512 bytes, quatro setores de cache, três handles públicos e backend com confirmação atrasada. Inclui sincronização/fechamento e desmontagem. O payload é conferido depois de remontar. São contagens de transferências, não medições de MB/s de cartão físico. O benchmark padrão e o combinado LFN+exFAT produziram os mesmos números FAT32.

| Sistema | Arquivos | Leituras antes → depois | Gravações antes → depois | I/O total antes → depois |
| --- | ---: | ---: | ---: | ---: |
| exfat | 1 | 37 → 37 | 534 → 534 | 571 → 571 |
| exfat | 2 | 1440 → 1440 | 2453 → 2453 | 3893 → 3893 |
| fat32 | 1 | 52 → 222 | 815 → 451 | 867 → 673 |
| fat32 | 2 | 1342 → 1348 | 4407 → 1099 | 5749 → 2447 |

| Primeira montagem / reserva do diário | Leituras antes → depois | Gravações antes → depois |
| --- | ---: | ---: |
| exfat | 22 → 22 | 561 → 284 |
| fat32 | 41 → 41 | 4381 → 2221 |

Em FAT32 com diário válido, a alocação de arquivos normais pode atualizar o EOC novo e o elo anterior antes de forçar uma descarga intermediária. A descarga automática é adiada apenas nessas fases; pressão do cache força uma gravação para evitar impasse com FAT fragmentada, leitores retendo buffers ou outros escritores. Publicação de diretório, checkpoint, fsync, close e desligamento mantêm a espera necessária. Diretórios e volumes sem diário conservam suas barreiras.

Na reserva inicial, a FAT e o bitmap permanecem no cache enquanto as atualizações contíguas cabem nele. A barreira final continua aguardando todas as gravações antes de publicar a extensão do diário. Os before-images e descritores ainda precisam estar confirmados antes de alterar metadados protegidos.

O agrupamento amplo na alocação normal exFAT foi experimentado e descartado: aumentava leituras e I/O total no escritor único. Seu caminho normal foi preservado; o ganho exFAT de I/O ficou na reserva inicial. No FAT32 de escritor único também há mais leituras, mas a redução das gravações diminui o total. Os ganhos dependem do cache e do workload.

## Integridade e limites

- FAT32: 4363 cortes nas operações, 2142 cortes durante o replay, 28 casos de diário inválido e 86 cortes na reserva inicial. Gravações completas e parciais, criação, renomeação e exclusão. Zero falhas nos resultados esperados; diários não confiáveis ficam protegidos.
- exFAT: 1898 cortes com fsck.exfat 1.3.2 após remontar; zero erros após recuperação, zero perda dos arquivos testemunha e de operações confirmadas. 56 transferências iniciais de propriedade permaneceram somente leitura, sem gravações, conforme o contrato anterior.
- Regressão em seis configurações e compilação dos oito testes originais. AddressSanitizer em LFN+exFAT com freefile, incluindo concorrência, falhas no backend, remoção/reinserção e bloqueio de canPowerOff com operações pendentes.

O teste de exclusividade durante close passou a aceitar que localizar uma entrada de diretório fora do cache pode aguardar I/O. Seu callback verifica que um novo escritor só é aceito depois do callback de fechamento anterior; a checagem de durabilidade continua descartando a RAM imediatamente após a confirmação do close.

A rodada externa complementar de novas quedas durante o replay exFAT não foi executada: a solicitação de acesso ao verificador foi recusada. Os testes locais de replay e a matriz exFAT acima foram executados. Não foram usados cartões físicos ou novos VHDs. A primeira reserva ainda precisa terminar antes de existir proteção pelo diário; payload sobrescrito no lugar, dados em RAM e caches que confirmem antes de persistir continuam fora da garantia.

## Reprodução

```powershell
./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/optimization/before-lib
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/optimization-after
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_fat_lfn_powercut.ps1 -Python python -Torn
./tests/run_exfat_powercut.ps1 -Python python
```

Para medir apenas o código atual, omita BaselineDirectory. A pasta de baseline usada nesta medição contém os arquivos lib da revisão indicada no início. O JSON ao lado contém tabelas completas e hashes das fontes e das evidências.
