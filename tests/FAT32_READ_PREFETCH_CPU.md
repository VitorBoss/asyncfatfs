# CPU da leitura: incorporação do setor retido e prefetch

Rodada de 17/11/2026. Baseline: código aprovado na
[otimização do poll](FAT32_READ_POLL_CPU.md). Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alterações mantidas

- `afatfs_fileRetainCursorSectorForRead()` passa a solicitar inlining obrigatório no
  MSVC e no GCC/XC16. Essa alteração foi mantida conforme solicitado. A função continua
  fazendo as mesmas verificações e operações de cache; nenhuma validação foi removida.
- Em `fread()`, o caso que termina dentro do setor já retorna antes do seek. Portanto,
  o caminho restante necessariamente terminou o setor e não precisa testar novamente
  essa condição para chamar o prefetch.
- Se o seek fica pendente, o loop encerra antes de chamar `fprefetch()`. Antes, essa
  chamada retornava sem trabalho porque o arquivo estava ocupado. Quando o seek termina
  imediatamente, o prefetch continua sendo chamado. O status do seek deixa de precisar
  sobreviver à chamada de prefetch, reduzindo sua vida útil no código gerado.

Não foram adicionados campos, buffers ou macros de configuração. Os atalhos experimentais
para aquisição de páginas retidas/pendentes foram descartados: não trouxeram ganho
consistente no padrão e afetaram a configuração com consulta. O código final mantém os
caminhos de aquisição de cache anteriores.

## CPU medida e comparação com upstream

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB por leitor.
**21 rodadas pareadas**, ordem alternada, **32 passagens** por arquivo, afinidade fixa
ao mesmo processador lógico e verificação integral do conteúdo. A medição ocorreu sem
compilações ou outras suítes em paralelo.

Dois leitores, blocos de 257 bytes. Ciclos por byte, menor é melhor:

| Configuração | Antes desta rodada | Atual | Redução pareada mediana |
|---|---:|---:|---:|
| Padrão | 3,6719 | **3,6405** | **1,01%** |
| Multibloco sem consulta | 3,7910 | **3,5882** | **5,55%** |
| Multibloco com consulta | 3,7351 | **3,6571** | **2,09%** |
| Upstream, quatro setores de cache | — | 4,5675 | — |
| Upstream, oito setores originais | — | 4,7361 | — |

O padrão atual usa **20,30% menos ciclos/byte** que upstream com o mesmo cache de quatro
setores e **23,13% menos** que upstream com oito. Sem consulta, os valores são 21,44%
e 24,24%; com consulta, 19,93% e 22,78%.

Reduções pareadas em todos os casos de leitura medidos:

| Bloco / leitores | Padrão | Multibloco sem consulta | Multibloco com consulta |
|---|---:|---:|---:|
| 257 / 1 | 0,68% | 4,75% | 2,26% |
| 257 / 2 | 1,01% | 5,55% | 2,09% |
| 4096 / 1 | **-1,26%** | 3,69% | 2,06% |
| 4096 / 2 | 0,26% | 4,55% | 1,12% |

Houve melhora na mediana pareada de **11 dos 12 casos**. A exceção é um leitor com
blocos de 4096 bytes no padrão, que ficou 1,26% mais caro em CPU. Essa regressão não foi
ocultada nem considerada resolvida. A incorporação solicitada foi preservada e a combinação
melhora os casos com consulta, os casos sem consulta e o caso alvo de dois leitores/257
bytes. Ganhos inferiores a 1% são pequenos frente à variabilidade do host; as amostras
individuais estão no JSON.

As reduções pareadas são medianas das razões por rodada, não a razão entre as medianas
das colunas. Os percentuais contra upstream comparam medianas de ciclos/byte; não são
percentuais de aumento de throughput. `QueryThreadCycleTime` inclui biblioteca, backend
sintético, poll e verificação dos dados no PC. Não são ciclos isolados da biblioteca nem
medições de CPU no PIC24. O [relatório inicial](FAT32_READ_CPU.md) descreve o método.

## RAM e pilha

RAM estática da biblioteca, inalterada antes/depois e com/sem multibloco e consulta:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`: o assembly confirmou que a função de obtenção do setor
retido existia como chamada separada antes e deixou de existir como função separada no
código atual, tanto com enums normais quanto curtos, no padrão e com consulta.
O frame de `fread()` permanece em 28 bytes na combinação final. A incorporação isolada
havia aumentado esse frame para 30 bytes; o ajuste do fluxo de prefetch compensou isso.

Limites conservadores de pilha interna em cascata, inalterados:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | **328 bytes** | **302 bytes** |
| Multibloco e consulta | **314 bytes** | **302 bytes** |

Todos ficam **abaixo de 400 bytes**. A análise inclui as chamadas internas de
`asyncfatfs.c`, `fat_standard.c` e endereços de retorno. Conforme o escopo acordado,
driver, callbacks externos, libc/auxiliares do compilador e interrupções são pontos de
corte; não é uma garantia da pilha completa da aplicação. Não houve execução em PIC24
nem medição com cartão SD físico.

## I/O e persistência

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, paradas e tempo virtual nos 32 casos normais com
consulta: leitura/escrita, dois perfis de latência, clusters de 512/4096 bytes, blocos de
257/4096 bytes e um/dois arquivos. Os casos de CPU também conferem igualdade de I/O e
polls nas configurações correspondentes.

No caso alvo, uma passagem mantém **4130 leituras e zero gravações** na fase de leitura,
iguais ao upstream. O throughput modelado do perfil lento permanece em **2,203808 MiB/s**,
contra **2,203565 MiB/s** no upstream. A otimização de CPU não altera a latência modelada
do dispositivo.

As duas FATs, reserva única, journal, barreiras, retenções e conclusões por ACK permanecem
intactos. Nenhum flush foi removido. As máquinas de estados e o backend continuam
responsáveis por concluir operações pendentes antes de entregar os respectivos buffers.

## Testes e evidência

Passaram os dez casos/configurações com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. Passou a suíte de alocação: ordem de persistência,
três arquivos, solicitações intercaladas, volume quase cheio, metadados, espelhamento,
reservas e cache dos leitores.

A regressão de cache ganhou verificações de aquisição com retenção nos estados IN_SYNC,
DIRTY e WRITING, incremento único do contador e do timestamp, preservação de locks e
ausência de aquisição ou entrega de buffer no estado READING. Foi executada também com
multibloco e consulta habilitados. Não foram necessários os VHDs perdidos.

Evidência final: `tests/build/fat32-read-prefetch-cpu/`, snapshots before/after/upstream,
logs `cpu-*`, `query-tests`, `plain-tests`, `allocation-tests` e `reader-query`.
Os baselines normais, os probes de RAM/pilha anteriores e os resultados normais upstream
foram reutilizados após verificar igualdade byte a byte das bibliotecas e igualdade do
commit upstream. CPU, benchmarks atuais, probes atuais e testes são desta combinação.
O [JSON validado](FAT32_READ_PREFETCH_CPU.json) registra hashes, amostras, contadores e pilha.

As tentativas descartadas e a medição isolada de inlining foram preservadas em
`fat32-read-retain-cpu`, `fat32-read-pending-cpu` e `fat32-read-inline-cpu`, sob `tests/build/`.
Elas não representam o código final.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-prefetch-cpu -Repetitions 21
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-prefetch-cpu -CurrentOnly -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-prefetch-cpu/query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-prefetch-cpu --output tests/FAT32_READ_PREFETCH_CPU.json --repetitions 21
```

Preservar os snapshots e executar a medição de CPU isoladamente.
