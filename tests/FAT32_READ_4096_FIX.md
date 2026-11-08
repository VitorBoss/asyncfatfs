# Correção da regressão de CPU: um leitor, blocos de 4096 bytes

Rodada de 17/11/2026. Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

A regressão de 1,26% documentada na [rodada anterior](FAT32_READ_PREFETCH_CPU.md) foi
eliminada na configuração padrão. A incorporação de `afatfs_fileRetainCursorSectorForRead()`
e o fluxo de prefetch aprovados foram mantidos.

## Alteração

No pipeline de alocação sem leitura multibloco, `fread()` agora:

- Expõe ao compilador uma cópia de exatamente 512 bytes quando o trecho lido ocupa um
  setor inteiro. Nesse caso o offset necessariamente é zero: o tamanho copiado é limitado
  por `512 - offset`, com offset entre 0 e 511. A cópia variável permanece para trechos
  parciais. Não há novos acessos fora do setor nem suposição adicional de alinhamento.
- Detecta o término parcial comparando o pedido restante com o espaço restante do setor.
  Isso reutiliza a expressão do limite da cópia e evita somar novamente offset e tamanho.
  A condição é equivalente à anterior porque o tamanho copiado é o mínimo desses limites.

A especialização usa apenas macros existentes. Foi aplicada somente a
`AFATFS_PIPELINE_ALLOCATION` sem `AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT`, configuração
em que o ganho foi confirmado. Uma tentativa de aplicá-la globalmente prejudicou os
perfis multibloco e foi descartada. Com multibloco, API de compatibilidade, LFN, exFAT,
journal ou freefile, o caminho anterior permanece como antes.

## CPU: regressão corrigida e ganhos preservados

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB por leitor.
Foram feitas **41 rodadas pareadas**, alternando a ordem das nove variantes, com
**32 passagens** por arquivo, afinidade fixa ao mesmo processador lógico e verificação
integral dos dados. Não houve compilações ou outras suítes simultâneas à medição.

Foram recompiladas tanto a versão imediatamente anterior, com a regressão, quanto a
referência anterior à incorporação/prefetch, que não a apresentava. Assim a correção não
é avaliada apenas contra uma versão já mais lenta.

Configuração padrão, ciclos por byte; menor é melhor:

| Caso | Referência sem regressão | Versão anterior | Atual | Redução pareada vs. anterior | Redução pareada vs. referência |
|---|---:|---:|---:|---:|---:|
| **Um leitor / 4096 bytes** | **2,9368** | **2,9706** | **2,8875** | **2,98%** | **1,93%** |
| **Dois leitores / 257 bytes** | **3,6708** | **3,6538** | **3,5941** | **1,37%** | **1,88%** |
| Um leitor / 257 bytes | 3,6420 | 3,6303 | 3,5675 | 1,74% | 2,14% |
| Dois leitores / 4096 bytes | 3,0492 | 3,0828 | 2,9975 | 2,82% | 2,02% |

Todos os quatro casos do padrão melhoraram na comparação pareada. As reduções pareadas
são medianas das razões por rodada, não a razão entre as medianas das colunas.

Os ganhos multibloco foram preservados sem alterar esse caminho. O validador compara
os corpos de `afatfs_fread` nos objetos MSVC x64, antes das relocações: são idênticos
antes/depois, tanto sem consulta quanto com consulta, com 906 bytes e SHA-256
`ce7dfbd85b45f9dd751dcec9a64c1ea4726e594a8293c7eaec8564431fe4a062`.
Nas medições desses perfis, as variações pareadas ficaram entre -0,22% e +0,30%, próximas
do ruído do host; não são apresentadas como ganhos novos.

Comparação com upstream, configuração padrão atual:

| Caso | Atual ciclos/byte | Upstream, cache 4 | Upstream, cache 8 original | Menos custo vs. cache 4 | Menos custo vs. cache 8 |
|---|---:|---:|---:|---:|---:|
| Um leitor / 4096 bytes | **2,8875** | 3,8463 | 4,0476 | **24,93%** | **28,66%** |
| Dois leitores / 257 bytes | **3,5941** | 4,5679 | 4,7369 | **21,32%** | **24,13%** |

Os percentuais contra upstream comparam medianas de ciclos/byte, não são aumentos
percentuais de throughput. A medição com `QueryThreadCycleTime` inclui biblioteca,
backend sintético, poll e verificação dos dados no PC. Não são ciclos isolados da
biblioteca, execução no PIC24 ou medição de cartão SD físico. O
[relatório inicial](FAT32_READ_CPU.md) descreve o método e as limitações.

## RAM e pilha

RAM estática da biblioteca, idêntica antes/depois e com/sem multibloco:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, máximo interno conservador em cascata, inalterado:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | **328 bytes** | **302 bytes** |
| Multibloco e consulta | **314 bytes** | **302 bytes** |

Todos ficam **abaixo de 400 bytes**. O frame de `fread()` permanece em 28 bytes.
A análise inclui as chamadas internas de `asyncfatfs.c`, `fat_standard.c` e endereços
de retorno. Conforme o escopo acordado, driver, callbacks externos, libc/auxiliares do
compilador e interrupções são pontos de corte; não é uma garantia da pilha da aplicação.

## I/O, persistência e testes

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, paradas e tempo virtual nos **32 casos normais do
padrão e nos 32 com consulta**: leitura/escrita, dois perfis de latência, clusters de
512/4096 bytes, blocos de 257/4096 bytes e um/dois arquivos. Os casos de CPU conferem
também igualdade de I/O e polls em cada configuração correspondente.

No perfil lento, uma passagem mantém:

| Caso | Leituras atuais e upstream | Gravações na leitura | Atual MiB/s modelados | Upstream MiB/s modelados |
|---|---:|---:|---:|---:|
| Um leitor / 4096 bytes | 2066 | 0 | 2,419960 | 2,405234 |
| Dois leitores / 257 bytes | 4130 | 0 | 2,203808 | 2,203565 |

O throughput modelado permanece igual ao anterior, pois esta alteração reduz apenas
trabalho de CPU. As duas FATs, reserva única, journal, barreiras, ACKs, validações de
arquivo e propriedade dos buffers não foram alterados. Nenhum flush foi removido.

Passaram os dez casos/configurações com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. Passou a suíte de alocação, incluindo ordem de
persistência, três arquivos, solicitações intercaladas, volume quase cheio, metadados,
espelhamento, reservas e cache dos leitores. Os testes conferiram conteúdo após remontar
e bloqueio dos escritores sob falhas de ACK de ambas as FATs. Não dependem dos VHDs perdidos.

## Evidência e reprodução

Diretório: `tests/build/fat32-read-4096-fix/`.

- `before/lib`: versão imediatamente anterior, com a regressão documentada.
- `reference/lib`: referência anterior à incorporação/prefetch, sem a regressão.
- `after/lib`: versão final, mantendo a incorporação e especializando apenas o padrão.
- `upstream/lib`: commit local indicado acima.
- `cpu-*`: rodada final de 41 repetições.
- `length-condition-trials` e `global-copy-trials`: medições intermediárias descartadas.
- `final-query-tests`, `final-plain-tests`, `final-allocation-tests`: validação final.

Os baselines normais, probes anteriores e resultados normais upstream foram reutilizados
após conferir igualdade byte a byte das bibliotecas e igualdade do commit upstream.
As medições de CPU, testes finais, benchmarks atuais e probes atuais são desta versão.
O [JSON validado](FAT32_READ_4096_FIX.json) registra hashes dos quatro snapshots,
comparações de código de máquina, amostras, contadores e análise de pilha.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-4096-fix -Repetitions 41 -Variants before_default,reference_default,default,before,before_query,after,after_no_query,upstream_equal,upstream_native
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-4096-fix -CurrentOnly -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-4096-fix/final-query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-4096-fix --output tests/FAT32_READ_4096_FIX.json --repetitions 41
```

Preservar os snapshots e executar a medição de CPU isoladamente.
