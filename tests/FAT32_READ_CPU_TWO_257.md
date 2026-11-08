# CPU de leitura: dois leitores, blocos de 257 bytes

Rodada de 16/11/2026, comparada com upstream/master local
`60c5f3580152410cb08342942cbbe3a30a449c24`. O baseline contém todas as otimizações
anteriores, inclusive a consulta opcional ao backend. Não foram sobrescritas as evidências
da rodada anterior em `fat32-read-cpu-20261116`.

## Alterações implementadas

1. A busca no cache procura primeiro uma página correspondente. Só avalia candidatos
   à substituição quando essa busca falha. Isso evita calcular prioridades de descarte
   antes de encontrar uma página já presente, como a FAT compartilhada pelos leitores.
   Em caso de falta, permanecem as prioridades anteriores, incluindo a página vazia
   correspondente, páginas descartáveis e antiguidade. Cada acerto incrementa o mesmo
   timestamp uma única vez. Retenção, locks e estados dirty/reading/writing são preservados.
2. Quando `fread()` termina dentro do setor retido, avança diretamente o cursor e retorna.
   O tamanho copiado é o mínimo entre o pedido restante e o espaço no setor; portanto,
   nesse ramo o pedido está concluído. Cruzamentos de setor continuam passando pelo seek
   assíncrono e pelo prefetch existentes. A cópia e o preenchimento de dados inválidos
   do exFAT ocorrem antes desse ramo, como antes.

Ambas são automáticas e não exigem novos campos, buffers ou macros. A primeira tentativa,
apenas com o avanço direto, não mostrou ganho consistente no caso alvo; a busca em duas
etapas completou a otimização. Uma falta no cache passa por uma busca adicional; o saldo
positivo medido aplica-se aos cenários apresentados, com muitas páginas já presentes.

## Resultado de CPU

Mesmo método da [rodada anterior](FAT32_READ_CPU.md): MSVC `/O2`, x64, um processador
lógico fixo, 7 rodadas pareadas em ordem alternada, 32 passagens por arquivo de 1 MiB,
verificação integral do conteúdo. Medição por `QueryThreadCycleTime`, incluindo biblioteca,
backend sintético, verificação e poll. São ciclos do PC, não do PIC24 nem de um cartão SD.

Caso solicitado: FAT32 8.3, clusters de 512 bytes, dois leitores e blocos de 257 bytes.
Medianas em ciclos/byte, menor é melhor:

| Configuração | Antes desta rodada | Atual | Redução pareada mediana |
|---|---:|---:|---:|
| Padrão, leitura multibloco desabilitada | 4,1050 | 3,9978 | **2,05%** |
| Multibloco, consulta desabilitada | 4,1790 | 4,0828 | **2,37%** |
| Multibloco e consulta habilitados | 4,2184 | 4,1131 | **2,33%** |
| Upstream, cache de quatro setores | — | 4,5793 | — |
| Upstream, cache original de oito setores | — | 4,7611 | — |

A configuração padrão atual consome **12,70% menos ciclos/byte** que upstream com o
mesmo cache de quatro setores, e **16,03% menos** que o upstream original com oito.
Com multibloco e consulta habilitados, as diferenças são 10,18% e 13,61%.
Com dois leitores, o multibloco continua usando o fallback de leitura comum; a consulta
opcional não é necessária para obter esta nova melhora.

As reduções pareadas usam a mediana das sete razões antes/depois, por isso diferem da
razão entre as duas medianas. Há variação no host, incluindo uma amostra inicial mais
alta; as amostras individuais estão no JSON. O ganho adicional é modesto e não deve ser
interpretado como igual redução no tempo total de uma leitura limitada pelo dispositivo.

Casos de controle, configuração padrão, ciclos/byte:

| Bloco / leitores | Antes | Atual |
|---|---:|---:|
| 257 / 1 | 4,0975 | 3,9717 |
| 4096 / 1 | 3,2140 | 3,1368 |
| 4096 / 2 | 3,4203 | 3,3531 |

## I/O e persistência

O validador confirmou igualdade anterior/atual de leituras, gravações, leituras/gravações
de metadados, polls, sessões multibloco, blocos agrupados, encerramentos e tempo virtual
nos 32 casos normais com multibloco e consulta habilitados: leitura/escrita, dois perfis de latência, clusters de 512/4096 bytes,
blocos de 257/4096 bytes e um/dois arquivos. O teste de CPU também verifica igualdade
dos contadores de I/O e polls para cada configuração correspondente.

No caso alvo, uma passagem pelos dois arquivos continua realizando **4130 leituras e
zero gravações** durante a fase de leitura, tanto no código atual quanto no upstream.
O throughput modelado do perfil lento permanece em **2,203808 MiB/s** no atual e
**2,203565 MiB/s** no upstream. A otimização reduz trabalho de CPU; não altera a latência
de dispositivo usada pelo modelo.

As duas FATs, reservas, journal, barreiras de persistência, conclusão dos callbacks e
cancelamento por geração de mídia não foram alterados. Não foram removidos flushes.

## RAM e pilha

RAM estática da biblioteca idêntica antes/depois, com/sem leitura multibloco:

| ABI | Bytes |
|---|---:|
| PIC24, enum normal | 2494 |
| PIC24, enum curto | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, análise conservadora das chamadas internas de
`asyncfatfs.c` e `fat_standard.c`, incluindo endereços de retorno:

| Configuração | Enum normal | Enum curto |
|---|---:|---:|
| Antes, padrão | 316 | 302 |
| Atual, padrão | **316** | **302** |
| Atual, multibloco + consulta | **314** | **302** |

Todos os máximos ficam abaixo de **400 bytes**. O frame isolado de `fread()` passa de
24 para 28 bytes, mas o máximo em cascata permanece igual. Conforme o escopo acordado,
driver, callbacks externos, libc/auxiliares do compilador e interrupções são pontos de
corte; estes números não limitam a pilha completa da aplicação.

## Testes e reprodução

- Dez configurações/casos passaram com a consulta habilitada: default, fat_legacy,
  fat_only, fat_lfn, exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration.
- Sem consulta: default, focused, backend e integration passaram.
- A suíte de alocação confere ordem de persistência, três arquivos, solicitações
  intercaladas, volume quase cheio, metadados, espelhamento, reserva e cache dos leitores.
- A regressão do cache agora verifica leituras parciais de 257 e 254 bytes: cursor em
  511, mesma página retida e contagem de retenção, sem I/O adicional nem operação pendente.
- Os benchmarks conferem conteúdo e remontagem. Não foram necessários os VHDs perdidos.

Evidência: `tests/build/fat32-read-cpu-two-257/`, snapshots before/after/upstream,
logs `cpu-*`, `final-query-tests`, `plain-tests` e `final-allocation-tests`.
O baseline normal em `normal-before` foi reutilizado da rodada anterior após verificar
igualdade byte a byte de toda a biblioteca anterior com o snapshot daquela rodada.
Os testes atuais, as oito variantes de CPU e os probes de memória/pilha foram executados
nesta rodada. Hashes dos três snapshots e o commit upstream constam no
[JSON validado](FAT32_READ_CPU_TWO_257.json).

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-cpu-two-257
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-cpu-two-257 -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-cpu-two-257/final-query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-cpu-two-257 --output tests/FAT32_READ_CPU_TWO_257.json
```

Preservar os snapshots antes de repetir; não executar compilações ou outras suítes
simultaneamente à medição de CPU. Não houve medição em hardware físico.
