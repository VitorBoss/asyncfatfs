# CPU: retorno direto de leituras já presentes no cache

Rodada de 16/11/2026. Baseline: resultado da
[otimização anterior](FAT32_READ_CPU_TWO_257.md), incluindo busca de cache em duas etapas
e avanço direto de leituras parciais. Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alteração

Uma solicitação com exatamente `AFATFS_CACHE_READ` agora pode retornar diretamente uma
página correspondente já em estado IN_SYNC, DIRTY ou WRITING. Esses são os mesmos estados
que a rotina anterior permitia ler. Isso evita a busca de um espelho FAT e a passagem pela
alocação em acessos a uma página existente, especialmente a FAT compartilhada pelos leitores.

O caminho mantém a verificação de sessão de mídia, os limites do volume e o incremento
único do timestamp de acesso. Não modifica locks, retenção, descarte, contadores de páginas
sujas nem estado de gravação. Solicitações com flags adicionais, páginas ausentes, vazias
ou ainda em leitura continuam pelo caminho existente. Nenhum campo, buffer ou macro foi
adicionado. A otimização é automática na configuração padrão.

## Medição de CPU

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes. Cada leitor percorre um arquivo de
1 MiB em blocos de 257 bytes. Foram feitas **21 rodadas pareadas**, alternando a ordem
das variantes, com **32 passagens** por arquivo e afinidade fixa ao mesmo processador
lógico. Não houve compilações ou outras suítes simultâneas à medição.

`QueryThreadCycleTime` mede o custo da thread, incluindo biblioteca, backend sintético,
verificação de conteúdo e poll. Não são ciclos isolados da biblioteca nem medições no
PIC24. O [relatório inicial](FAT32_READ_CPU.md) documenta o método e suas limitações.

Dois leitores, blocos de 257 bytes; ciclos/byte, menor é melhor:

| Configuração | Antes desta rodada | Atual | Redução pareada mediana |
|---|---:|---:|---:|
| Padrão, multibloco desabilitado | 3,9991 | **3,9131** | **2,05%** |
| Multibloco sem consulta | 4,1371 | **3,9512** | **4,40%** |
| Multibloco com consulta | 4,1068 | 4,0899 | 0,40% |
| Upstream, quatro setores de cache | — | 4,5825 | — |
| Upstream, oito setores originais | — | 4,7486 | — |

A configuração padrão atual usa **14,61% menos ciclos/byte** que upstream com o mesmo
cache de quatro setores, e **17,60% menos** que upstream com oito setores. Com multibloco
sem consulta, as diferenças são 13,78% e 16,79%. Multibloco e consulta continuam opcionais
e desabilitados por padrão; dois leitores continuam usando o fallback de leitura comum.

As reduções pareadas são medianas das razões por rodada; não são a razão das medianas
das colunas. A primeira medição, com sete rodadas, foi ampliada para 21 porque os ganhos
são pequenos. As amostras finais individuais estão no JSON.

Controles, redução pareada do custo de CPU:

| Bloco / leitores | Padrão | Multibloco sem consulta | Multibloco com consulta |
|---|---:|---:|---:|
| 257 / 1 | 2,56% | 3,87% | -0,31% |
| 4096 / 1 | 2,48% | 3,25% | -1,16% |
| 4096 / 2 | 0,23% | 3,82% | -1,54% |

Valores negativos representam aumento de custo. Com a consulta habilitada, o ganho no
caso alvo é próximo da variabilidade da medição e existem pequenas regressões nos controles.
Não há evidência de ganho geral nessa configuração. O resultado favorável é mais claro
no padrão e no multibloco sem consulta. O código gerado e o custo no hardware alvo podem
diferir; não foi medido um cartão SD físico.

## I/O, RAM, pilha e persistência

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, encerramentos e tempo virtual nos 32 casos normais
com multibloco e consulta: leitura/escrita, dois perfis de latência, clusters de 512/4096
bytes, blocos de 257/4096 bytes e um/dois arquivos. Os casos de CPU verificam também
igualdade dos contadores de I/O e polls em cada configuração correspondente.

No caso alvo, uma passagem pelos dois arquivos mantém **4130 leituras e zero gravações**
na fase de leitura, iguais ao upstream. O throughput modelado do perfil lento permanece
**2,203808 MiB/s**, contra **2,203565 MiB/s** no upstream. Essa otimização de CPU não
altera o modelo de latência do dispositivo.

RAM estática da biblioteca, idêntica antes/depois e com/sem multibloco:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, máximo interno conservador em cascata:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Antes, padrão | 316 | 302 |
| Atual, padrão | **328** | **302** |
| Atual, multibloco e consulta | **314** | **310** |

Todos permanecem **abaixo de 400 bytes**. No padrão com enums normais, o frame de
`cacheSector()` aumenta de 24 para 36 bytes; `fread()` permanece em 28. O analisador
inclui `asyncfatfs.c`, `fat_standard.c` e endereços de retorno. Driver, callbacks externos,
libc/auxiliares do compilador e interrupções são pontos de corte, conforme o escopo
interno acordado; não é uma garantia da pilha completa da aplicação.

Não foram alterados os caminhos de gravação, as duas FATs, reservas, journal, barreiras,
callbacks de conclusão ou geração de mídia. Páginas em WRITING já podiam ser lidas;
o caminho direto não libera nem modifica seu buffer e não considera a gravação concluída.

## Verificação e reprodução

- Dez configurações/casos passaram com consulta: default, fat_legacy, fat_only, fat_lfn,
  exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration.
- Sem consulta: default, focused, backend e integration passaram.
- Passaram os testes de alocação e ordem de persistência, três arquivos, operações
  intercaladas, volume quase cheio, metadados, espelhamento, reserva e cache dos leitores.
- A regressão específica cobre leitura de páginas IN_SYNC/DIRTY/WRITING sem alterar
  locks, retenção, descarte, contadores ou I/O, e rejeita a entrega de página READING.
- Benchmarks verificaram conteúdo e remontagem. Não dependem dos VHDs perdidos.

Evidência bruta: `tests/build/fat32-read-cpu-hits/`, com snapshots before/after/upstream,
CSV/logs `cpu-*`, `query-tests`, `plain-tests` e `allocation-tests`.
O baseline normal em `normal-before` foi reutilizado da rodada anterior após verificar
igualdade byte a byte de toda a biblioteca daquela rodada com o snapshot before atual.
Medições de CPU, benchmarks atuais, testes e probes de RAM/pilha são desta rodada.
O [JSON validado](FAT32_READ_CPU_HITS.json) registra hashes, manifesto upstream, contadores,
frames e cadeias de pilha.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-cpu-hits -Repetitions 21
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-cpu-hits -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-cpu-hits/query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-cpu-hits --output tests/FAT32_READ_CPU_HITS.json --repetitions 21
```

Preservar os snapshots anteriores e executar a medição de CPU isoladamente.
