# Correção das regressões com consulta multibloco

Rodada de 16/11/2026. Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alteração

A busca de páginas legíveis já presentes no cache foi extraída para
`afatfs_findReadableCacheSector()`. Com `AFATFS_READ_SESSION_QUERY=1`, ela é uma função
curta `noinline`, separando seus registradores e instruções do caminho de sessão de
leitura. Sem consulta, a busca permanece obrigatoriamente inline, preservando a
otimização anterior. Não há nova macro, campo de estado ou buffer.

Foi confirmado no assembly PIC24 que o helper existe como função com consulta e que
não existe como chamada separada no padrão. A escolha é de geração de código; não
altera quais páginas podem ser lidas, limites do volume, timestamps, retenção, locks,
callbacks ou processamento das sessões.

## Comparação de CPU

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB por leitor.
**41 rodadas pareadas**, ordem alternada, **32 passagens de leitura**, thread presa ao
mesmo processador lógico e verificação integral dos dados. Nove variantes foram medidas
sem compilações ou outras suítes simultâneas. Uma primeira rodada com 21 repetições foi
preservada em `first-21-trials`; a tabela abaixo usa a rodada ampliada.

Além da versão imediatamente anterior, foi compilada novamente a referência anterior
ao retorno direto de cache, que não apresentava as regressões. Ambas usam exatamente
as mesmas macros de multibloco e consulta do código atual. Isso evita comparar apenas
contra uma versão que já estava mais lenta.

Consulta habilitada, ciclos por byte e reduções pareadas:

| Bloco / leitores | Referência sem regressão | Versão com regressão | Atual | Redução vs. versão anterior | Redução vs. referência |
|---|---:|---:|---:|---:|---:|
| 257 / 1 | 4,1109 | 4,1331 | 3,9830 | 3,55% | 2,88% |
| **257 / 2** | **4,1191** | **4,0992** | **3,9711** | **3,24%** | **3,44%** |
| 4096 / 1 | 3,2172 | 3,2238 | 3,2056 | 0,90% | 0,41% |
| 4096 / 2 | 3,2945 | 3,3435 | 3,3165 | 0,78% | 0,47% |

As colunas de ciclos/byte são medianas. As reduções são medianas das razões por rodada,
usando medições pareadas; não são calculadas dividindo as medianas das colunas.
Isso importa especialmente no controle de 4096 bytes/dois leitores, sujeito a variação
durante a rodada. Não reapareceram as regressões pareadas de cerca de 1–1,5% da rodada
anterior. Os controles de 4096 bytes ficaram próximos da referência: ganhos abaixo de
1% não são evidência de uma aceleração relevante. Todas as amostras estão no JSON.

Dois leitores, blocos de 257 bytes, comparação com upstream:

| Configuração | Ciclos/byte |
|---|---:|
| Atual, padrão | **3,9192** |
| Atual, multibloco e consulta | **3,9711** |
| Upstream, quatro setores de cache | 4,5995 |
| Upstream, oito setores originais | 4,7623 |

O padrão usa **14,79% menos ciclos/byte** que upstream com quatro setores, e **17,70%
menos** que o upstream original. Com consulta, são **13,66%** e **16,61%**, respectivamente.
Esses percentuais comparam medianas de custo, não são ganhos percentuais de throughput.

Na configuração padrão, a redução pareada anterior/atual no caso alvo foi 0,07%,
compatível com desempenho preservado. Sem consulta, os controles ficaram entre -0,25%
e +0,17% no padrão. O ganho anterior de dois leitores/257 bytes foi mantido; esta mudança
não introduz uma nova aceleração relevante nessa configuração.

Os ciclos medidos por `QueryThreadCycleTime` incluem biblioteca, backend sintético,
poll e verificação dos dados no PC. Não são ciclos do PIC24 nem medições de cartão SD.
O [relatório inicial](FAT32_READ_CPU.md) descreve o método e suas limitações.

## RAM, pilha, I/O e persistência

RAM estática da biblioteca antes/depois, idêntica com/sem consulta:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, limite conservador de pilha interna em cascata:

| Configuração | Antes | Atual |
|---|---:|---:|
| Padrão, enums normais | 328 | **328** |
| Consulta, enums normais | 314 | **314** |
| Padrão, enums curtos | 302 | **302** |
| Consulta, enums curtos | 310 | **302** |

Todos ficam **abaixo de 400 bytes**. A análise inclui as chamadas internas de
`asyncfatfs.c`, `fat_standard.c` e endereços de retorno. Driver, callbacks externos,
libc/auxiliares do compilador e interrupções são pontos de corte, conforme o escopo
acordado; não é uma garantia da pilha completa da aplicação.

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, paradas e tempo virtual nos 32 casos normais com
consulta: leitura/escrita, dois perfis de latência, clusters de 512/4096 bytes, blocos de
257/4096 bytes e um/dois arquivos. Os testes de CPU também confirmam igualdade de I/O
e polls nas configurações correspondentes, inclusive contra a referência sem regressão.

No caso alvo, uma passagem mantém **4130 leituras e zero gravações** durante a leitura,
iguais ao upstream. O throughput modelado do perfil lento permanece em **2,203808 MiB/s**,
contra **2,203565 MiB/s** no upstream. Não há mudança na latência modelada de dispositivo.

As duas FATs, reservas, journal, barreiras e conclusões de operações não foram alterados.
O helper apenas identifica páginas que já podiam ser lidas. O caller mantém a validação
de limites e atualiza a antiguidade uma única vez; páginas READING e solicitações com
flags adicionais seguem pelo caminho completo. A consulta continua pura e opcional.

## Validação e reprodução

Passaram as dez configurações/casos com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. A suíte de alocação passou, incluindo ordem de
persistência, três arquivos, operações intercaladas, volume quase cheio, metadados,
espelhamento, reservas e proteção de páginas no cache. Foram verificados conteúdo e
remontagem. Não foram necessários os VHDs perdidos.

Evidência: `tests/build/fat32-read-query-fix/`. Snapshots:

- `before/lib`: versão imediatamente anterior, com a regressão documentada.
- `reference/lib`: versão anterior ao retorno direto de cache, sem a regressão.
- `after/lib`: código atual.
- `upstream/lib`: commit local upstream indicado acima.

O baseline normal foi reutilizado da rodada anterior após conferir igualdade byte a byte
de toda a biblioteca. CPU, testes, benchmarks atuais e probes de RAM/pilha são desta rodada.
O [JSON validado](FAT32_READ_QUERY_FIX.json) registra hashes dos quatro snapshots,
manifesto upstream, amostras, contadores e análise de pilha.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-query-fix -Repetitions 41 -Variants before_default,default,before,before_query,reference_query,after,after_no_query,upstream_equal,upstream_native
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-query-fix -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-query-fix/query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-query-fix --output tests/FAT32_READ_QUERY_FIX.json --repetitions 41
```

Preservar os snapshots e executar a medição de CPU isoladamente.
