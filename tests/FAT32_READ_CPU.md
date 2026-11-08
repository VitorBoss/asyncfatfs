# Custo de CPU da leitura FAT32 8.3

Medição de 16/11/2026. Referência upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`. A base anterior desta otimização já contém
a correção do ponteiro do cache DMA e a política que elimina as 510 releituras entre dois
leitores. Esses ganhos anteriores não são atribuídos às alterações de CPU deste relatório.

## Alterações

- `afatfs_fprefetch()` usa a máscara de setor no cluster já existente, eliminando o módulo
  variável de 32 bits. No assembly XC16 desaparece a chamada ao auxiliar de módulo.
- O poll evita chamar a máquina de estados para operações `NONE`. Operações ativas e o
  poll da API de compatibilidade mantêm seu processamento.
- Com `AFATFS_READ_SESSION_QUERY=1`, a biblioteca consulta o backend antes de repetir a
  varredura de até 128 entradas FAT de uma sequência já aceita. A verificação barata de
  elegibilidade ocorre primeiro, evitando esse custo com dois leitores. A consulta não
  altera contadores nem aumenta a faixa aprovada. Sem sequência válida, a validação FAT,
  limites de arquivo e volume e início da sequência continuam completos.

As duas primeiras alterações são automáticas. A terceira exige também
`AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2` e a implementação de
`sdcard_isReadBlockInSequence()` no driver. Ambos os recursos de leitura multibloco
continuam desabilitados por padrão. Simulador e backends de teste implementam a consulta
usando os campos existentes. Um driver antigo continua funcionando sem essa macro.

## CPU medida

MSVC `/O2`, x64, FAT32, clusters de 512 bytes, arquivos de 1 MiB por leitor,
7 rodadas pareadas com ordem alternada, 32 passagens de leitura por caso. Cada processo
fica preso ao primeiro processador lógico permitido. A verificação dos dados permanece
habilitada. Os números incluem biblioteca, backend sintético, poll, verificação e custo
do ambiente de teste: não são ciclos isolados da biblioteca nem medições no PIC24.

Foi usado `QueryThreadCycleTime`, que contabiliza execução da thread em modo usuário e
kernel. Esses ciclos não são convertidos em tempo real; a própria documentação alerta
para diferenças entre processadores e frequência. Tempos QPC brutos também foram retidos.
Fonte: [Microsoft — QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime).

Medianas em ciclos por byte; menor é melhor:

| Bloco / leitores | Antes, padrão | Atual, padrão | Antes, multibloco | Atual, multibloco + consulta | Upstream, cache 4 | Upstream, cache 8 original |
|---|---:|---:|---:|---:|---:|---:|
| 257 / 1 | 4,1306 | 4,0782 | 4,5752 | 4,2149 | 4,5251 | 4,6563 |
| 257 / 2 | 4,1781 | 4,0934 | 4,2625 | 4,1910 | 4,5610 | 4,7277 |
| 4096 / 1 | 3,2287 | 3,2163 | 3,6719 | 3,2588 | 3,8491 | 4,0429 |
| 4096 / 2 | 3,4332 | 3,4179 | 3,4589 | 3,4004 | 3,8461 | 3,9970 |

Medianas das reduções pareadas do custo de CPU:

| Bloco / leitores | Configuração padrão | Multibloco + consulta |
|---|---:|---:|
| 257 / 1 | 1,27% | 7,80% |
| 257 / 2 | 1,97% | 1,61% |
| 4096 / 1 | 0,39% | 11,13% |
| 4096 / 2 | 0,89% | 2,04% |

O resultado atual com multibloco e consulta usa 6,86%, 8,11%, 15,34% e 11,59% menos
ciclos/byte que upstream com quatro páginas, respectivamente. São diferenças entre medianas;
não devem ser confundidas com ganho percentual de throughput. Ganhos próximos de 1% são
pequenos diante da variabilidade do host, registrada no JSON. Sem a consulta, mas com
multibloco habilitado, as reduções pareadas foram 0,04%, 1,96%, 1,15% e 1,65%: a maior
melhora de um leitor vem de evitar a varredura FAT repetida.

## I/O e throughput modelado

A comparação anterior/atual com a mesma configuração verificou igualdade de leituras,
gravações, metadados, polls e latência virtual em todos os casos do teste normal, inclusive
escrita e clusters de 4096 bytes. As quatro leituras repetidas do teste de CPU também
mantiveram esses contadores. A otimização de CPU não altera o throughput calculado pelo
modelo de latência de dispositivo.

Perfil lento do backend, clusters de 512 bytes, uma passagem:

| Bloco / leitores | Leituras atuais e upstream | Gravações na leitura | Atual padrão MiB/s | Atual multibloco MiB/s | Upstream MiB/s |
|---|---:|---:|---:|---:|---:|
| 257 / 1 | 2066 | 0 | 2,202837 | 3,932982 | 2,202595 |
| 257 / 2 | 4130 | 0 | 2,203808 | 2,203808 | 2,203565 |
| 4096 / 1 | 2066 | 0 | 2,419960 | 4,683183 | 2,405234 |
| 4096 / 2 | 4130 | 0 | 2,421132 | 2,421132 | 2,406391 |

O ganho modelado de multibloco já existia antes desta alteração. Com dois leitores o
backend usa leituras comuns. Nenhum desses valores representa uma medição de cartão SD físico.

## RAM, pilha e persistência

RAM estática da biblioteca, idêntica antes/depois e com/sem consulta, cache de quatro setores:

| ABI | Bytes |
|---|---:|
| PIC24, enum normal | 2494 |
| PIC24, enum curto | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`: o analisador soma conservadoramente os frames de
`asyncfatfs.c` e `fat_standard.c`, ajustes de pilha e endereços de retorno em cascata.
Máximo interno com enums normais: **316 bytes** no padrão e **314 bytes** com multibloco
e consulta. Com enums curtos: **302 bytes** em ambos. Todos ficam abaixo de 400 bytes.
O escopo exclui driver, callbacks da aplicação, libc/auxiliares do compilador e interrupções;
não é uma garantia de pilha da aplicação completa. O JSON lista os pontos de corte.

O frame de `fread()` permanece em 24 bytes. `fprefetch()` cai de 14 para 10;
o poll sobe de 0 para 4, sem aumentar o máximo interno em cascata. Portanto, a economia
de quatro bytes, por si só, não explica o ganho: a remoção de operações de CPU explica.

Não foram alterados journal, barreiras, geração de mídia, ordem das duas FATs, conclusão
dos callbacks ou retenção dos buffers. A consulta exige resposta falsa durante transferência,
parada, falha, reset ou remoção. Sync, close, idle e desligamento continuam aguardando
o encerramento da sequência. Não há novos buffers nem campos de estado estático.

## Verificação e reprodução

As dez configurações/casos sintéticos de leitura passaram com a consulta habilitada:
default, fat_legacy, fat_only, fat_lfn, exfat_only, exfat_freefile, lfn_exfat, focused,
backend e integration. A matriz sem consulta também passou. Incluem falhas de início,
leitura e parada, reset/remoção, callback antigo e consulta sem efeitos colaterais.
Os testes de alocação, espelhamento de FAT, metadados, reservas e cache de leitores
também foram executados. Não dependem dos VHDs perdidos na troca de branch.

Evidência bruta: `tests/build/fat32-read-cpu-20261116/`, com snapshots `before/lib`,
`after/lib`, upstream e manifesto; CSVs e logs por rodada ficam em `cpu-*`.
`run_read_cpu_benchmark.ps1` compila/executa o teste de CPU a partir desses snapshots.
`run_multiread_comparison.ps1 -ReadSessionQuery` produz as medições normais e de RAM.
`run_multiread_tests.ps1 -ReadSessionQuery` executa a matriz funcional.
`read_cpu_report.py` valida os contadores, RAM, orçamento de pilha e hashes do código
atual contra o snapshot e gera [FAT32_READ_CPU.json](FAT32_READ_CPU.json).
