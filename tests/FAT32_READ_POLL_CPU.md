# CPU da leitura: evitar trabalho vazio de alocação no poll

Rodada de 16/11/2026, comparada à versão da
[correção da consulta multibloco](FAT32_READ_QUERY_FIX.md). Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alteração

O caminho de `afatfs_poll()` da configuração com pipeline de alocação agora verifica
os contadores existentes antes de entrar nos helpers:

- Sem flush em andamento, não chama `afatfs_allocationWriteInProgress()`, que retornaria falso.
- Sem páginas sujas, não chama a busca de grupos de alocação nem o flush de background,
  que não produziriam trabalho nesses estados.
- Depois de avançar as operações dos arquivos, consulta novamente o contador de páginas
  sujas. Não reutiliza um resultado antigo: uma operação pode produzir metadados durante
  o próprio poll e continuar exigindo agrupamento ou flush.

`sdcard_poll()`, o avanço das máquinas de estados e `afatfs_releaseIdleReservation()`
continuam sendo executados como antes. O incremento do relógio do cache e a liberação
de uma reserva ociosa permanecem intactos. Não foram alterados os flushes explícitos de
sync, close e barreiras de persistência.

São quatro guardas locais, sem novos campos, buffers, macros ou variáveis de estado.
Aplicam-se ao pipeline já utilizado pelo padrão FAT32 8.3; os caminhos com API de
compatibilidade, LFN, exFAT, journal e freefile continuam usando suas configurações anteriores.

## Medição de CPU

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB por leitor.
**21 rodadas pareadas**, ordem alternada, **32 passagens de leitura** por arquivo,
afinidade fixa ao mesmo processador lógico e verificação integral do conteúdo.
Nenhuma compilação ou outra suíte foi executada simultaneamente à medição.

Caso solicitado: dois leitores e blocos de 257 bytes. Ciclos por byte, menor é melhor:

| Configuração | Antes desta rodada | Atual | Redução pareada mediana |
|---|---:|---:|---:|
| Padrão, multibloco desabilitado | 3,9059 | **3,6856** | **5,65%** |
| Multibloco sem consulta | 3,9677 | **3,7974** | **4,41%** |
| Multibloco com consulta | 3,9802 | **3,7575** | **5,33%** |
| Upstream, quatro setores de cache | — | 4,5606 | — |
| Upstream, oito setores originais | — | 4,7159 | — |

O padrão atual usa **19,18% menos ciclos/byte** que upstream com o mesmo cache de quatro
setores e **21,85% menos** que o upstream original com oito. Com consulta multibloco,
as diferenças são **17,61%** e **20,32%**, respectivamente.

Controles: redução pareada do custo de CPU anterior/atual, mantendo as mesmas macros:

| Bloco / leitores | Padrão | Multibloco sem consulta | Multibloco com consulta |
|---|---:|---:|---:|
| 257 / 1 | 6,20% | 4,11% | 6,00% |
| 4096 / 1 | 3,77% | 1,78% | 3,23% |
| 4096 / 2 | 2,43% | 2,14% | 1,93% |

Não apareceram regressões pareadas nesses controles. O custo adicional de CPU dos
polls pesa mais com blocos de 257 bytes, onde a aplicação faz mais chamadas.

As reduções pareadas são medianas das razões por rodada; não são a razão das medianas
das colunas. Os percentuais contra upstream comparam medianas de ciclos/byte. Não são
percentuais de aumento de throughput. `QueryThreadCycleTime` inclui biblioteca, backend
sintético, verificação dos dados e poll no PC. Não são ciclos isolados da biblioteca nem
medições no PIC24 ou em cartão SD físico. Método e limitações adicionais estão no
[relatório inicial](FAT32_READ_CPU.md); as amostras individuais constam no JSON.

## RAM e pilha

RAM estática da biblioteca, idêntica antes/depois, com/sem multibloco e consulta:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, máximo interno conservador em cascata, também inalterado:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | **328 bytes** | **302 bytes** |
| Multibloco e consulta | **314 bytes** | **302 bytes** |

Todos ficam **abaixo de 400 bytes**. A análise inclui as chamadas internas de
`asyncfatfs.c`, `fat_standard.c` e endereços de retorno. Conforme o escopo acordado,
driver, callbacks externos, libc/auxiliares do compilador e interrupções são pontos de
corte; os números não limitam a pilha completa da aplicação.

## I/O e persistência

O validador confirmou igualdade de leituras, gravações, metadados, polls, sessões
multibloco, blocos agrupados, paradas e tempo virtual nos 32 casos normais com consulta:
leitura/escrita, dois perfis de latência, clusters de 512/4096 bytes, blocos de 257/4096
bytes e um/dois arquivos. Os testes de CPU também verificam igualdade de I/O e polls
em cada configuração correspondente.

No caso alvo, uma passagem mantém **4130 leituras e zero gravações** durante a fase de
leitura, iguais ao upstream. O throughput modelado do perfil lento permanece em
**2,203808 MiB/s**, contra **2,203565 MiB/s** no upstream. A redução de CPU não altera a
latência de dispositivo usada pelo modelo.

As duas FATs, reserva única, journal, barreiras, ACKs e ordem de publicação não foram
alterados. Os testes de agrupamento mantiveram ambos os escritores bloqueados sob
falhas de ACK da FAT primária e do espelho e verificaram o conteúdo após remontar.

## Validação e reprodução

Passaram os dez casos/configurações com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. A suíte de alocação passou, incluindo ordem de
persistência, três arquivos, solicitações intercaladas, volume quase cheio, metadados,
espelhamento, reservas e cache dos leitores. Os testes são sintéticos e não dependem dos
VHDs perdidos.

Evidência: `tests/build/fat32-read-poll-cpu/`, snapshots before/after/upstream, CSV/logs
`cpu-*`, `query-tests`, `plain-tests` e `allocation-tests`. O baseline normal foi reutilizado
da rodada anterior após verificar igualdade byte a byte de toda a biblioteca. CPU,
benchmarks atuais, testes e probes de RAM/pilha são desta rodada. O
[JSON validado](FAT32_READ_POLL_CPU.json) registra hashes, manifesto upstream, contadores,
amostras e análise de pilha.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-poll-cpu -Repetitions 21
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-poll-cpu -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-poll-cpu/query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-poll-cpu --output tests/FAT32_READ_POLL_CPU.json --repetitions 21
```

Preservar os snapshots e executar a medição de CPU isoladamente.
