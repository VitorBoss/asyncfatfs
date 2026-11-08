# Comparação FAT32 8.3 com upstream master

Data: 16/11/2026, America/Sao_Paulo.

A configuração atual reduz a RAM estática em relação ao padrão upstream, principalmente por usar metade do cache. Com cache igualado, ela usa um pouco mais de RAM. A leitura faz a mesma quantidade de I/O em sete dos oito cenários do perfil de latência; no restante, faz mais leituras. A escrita atual faz mais I/O, sobretudo com clusters de 512 bytes. O upstream deixa a segunda FAT desatualizada em todos os volumes deste ensaio; a versão atual mantém as duas FATs iguais. Portanto, menos gravações no upstream também refletem comportamentos diferentes de manutenção dos metadados.

As velocidades abaixo são de execução no computador com mídia em RAM e de um modelo explícito de latência. Elas não medem um cartão SD físico nem permitem afirmar a velocidade real em PIC24.

## Revisões e configurações

- Upstream: `60c5f3580152410cb08342942cbbe3a30a449c24`, referência local `upstream/master`, sem atualização por fetch.
- Atual: checkout baseado em `399d3755e4b7f47777001fc0202dc79e9a26e8ed`, com as alterações locais registradas nos hashes de `upstream_comparison.json`. A comparação não representa somente o commit HEAD.
- Upstream padrão: 8 setores de cache, 3 handles, limiar de múltiplos blocos 4.
- Upstream igualado: 4 setores de cache, 3 handles, limiar de múltiplos blocos 2.
- Atual padrão: 4 setores de cache, 3 handles, limiar de múltiplos blocos 2; API de compatibilidade, LFN, exFAT, freefile e diário desabilitados.

## RAM estática

Bytes de todas as seções graváveis de `asyncfatfs.c` e `fat_standard.c`, incluindo variáveis estáticas locais. Cache e handles já estão incluídos. Não inclui backend, heap do ensaio, pilha nem buffers da aplicação; a biblioteca não usa heap.

| ABI | Upstream padrão | Upstream igualado | Atual | Atual vs padrão | Atual vs igualado |
| --- | --- | --- | --- | --- | --- |
| PIC24 | 4544 | 2424 | 2494 | -2050 (-45.1%) | +70 (+2.9%) |
| PIC24 enums curtos | 4520 | 2408 | 2474 | -2046 (-45.3%) | +66 (+2.7%) |
| x86 | 4668 | 2524 | 2612 | -2056 (-44.0%) | +88 (+3.5%) |
| x64 | 4784 | 2640 | 2720 | -2064 (-43.1%) | +80 (+3.0%) |

### Estruturas com cache igualado

| ABI | afatfs_t upstream | afatfs_t atual | Handle upstream | Handle atual |
| --- | --- | --- | --- | --- |
| 16-normal | 2424 | 2488 | 60 | 76 |
| 16-short | 2408 | 2468 | 60 | 74 |
| 32 | 2524 | 2604 | 76 | 96 |
| 64 | 2640 | 2704 | 104 | 120 |

`sizeof(afatfs_t)` não é toda a RAM estática. Os totais anteriores incluem também os demais símbolos graváveis. Os valores dos handles são informativos e não devem ser somados novamente.

## Carga e método

- FAT32 sintético com MBR, 65.530 clusters, dois FATs, setores de 512 bytes; clusters de 512 ou 4.096 bytes. FSInfo e boot de backup ausentes, indicados como 0xFFFF. Cada caso começa com volume vazio idêntico.
- Um ou dois arquivos de 1 MiB, nomes 8.3, blocos da aplicação de 257 ou 4.096 bytes. Dois arquivos são processados alternadamente na mesma thread.
- Escrita inclui criação, transferência, fechamento e drenagem explícita. Depois da drenagem, o desmonte não pode fazer gravações adicionais: essa condição é verificada por assert.
- Leitura usa remontagem para esvaziar o cache da biblioteca e inclui abertura, consulta de tamanho, leitura com verificação byte a byte, fechamento e drenagem. Montagem, formatação e auditoria independente ficam fora das fases cronometradas.
- MSVC x64 `/O2 /std:c11 /W3 /WX`, mesmo computador e backend assíncrono para ambas as revisões. Sete repetições por cenário; taxas do computador calculadas pela mediana dos tempos. Execuções das revisões foram sequenciais, sem randomização; variação de frequência, escalonamento e aquecimento podem influenciar os tempos. Mínimos e máximos estão no JSON.
- RAM medida também em MSVC x86 e XC16 2.10 para `24FJ256GA702`, enums normais/curtos. PIC24 usa compilação cruzada, sem execução em hardware. A licença XC16 informa restrições de otimização; isso não altera os valores de sizeof e das seções de RAM usados aqui.
- O header upstream usa atributos GCC para registros de disco. No MSVC, um header forçado aplica `#pragma pack(push, 1)` apenas a esse header e neutraliza `__attribute__`. Os fontes upstream extraídos do commit permanecem intactos.

### Perfis do backend

Perfil 0: cada transferência completa no próximo poll, com mídia em RAM; usado para medir o custo de execução no computador. Não simula uma velocidade física.

Perfil 1: cada poll avança 10 µs virtuais; leitura de setor custa 200 µs, escrita isolada 800 µs e escrita em sequência múltipla 400 µs, com 100 µs para iniciar/encerrar uma sequência ativa. São premissas do ensaio, não especificações de um cartão. O tempo virtual inclui polls de progresso da máquina de estados e pode alterar a oportunidade de agrupar metadados.

## I O e velocidade no modelo de latência

R/W são setores de 512 bytes lidos/escritos. A taxa é o payload agregado dividido pelo tempo virtual, em MiB/s. As três configurações usam o perfil 1. A tabela de escrita conta dados e metadados.

### Escrita

| Cluster B | Bloco B | Arquivos | Upstream padrão R/W; MiB/s | Upstream igualado R/W; MiB/s | Atual R/W; MiB/s |
| --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 35/2803; 0.445 | 66/4128; 0.302 | 50/12288; 0.101 |
| 512 | 257 | 2 | 36/5570; 0.448 | 164/8353; 0.298 | 4218/24575; 0.097 |
| 512 | 4096 | 1 | 21/2771; 0.450 | 37/4128; 0.302 | 66/12288; 0.102 |
| 512 | 4096 | 2 | 37/5539; 0.451 | 100/8318; 0.300 | 223/24575; 0.101 |
| 4096 | 257 | 1 | 516/2564; 0.464 | 516/2564; 0.464 | 516/3328; 0.470 |
| 4096 | 257 | 2 | 522/4877; 0.499 | 530/4881; 0.499 | 781/6655; 0.365 |
| 4096 | 4096 | 1 | 516/2564; 0.464 | 516/2564; 0.464 | 516/3328; 0.470 |
| 4096 | 4096 | 2 | 1034/5131; 0.464 | 1034/5131; 0.464 | 1034/6655; 0.470 |

### Leitura

| Cluster B | Bloco B | Arquivos | Upstream padrão R/W; MiB/s | Upstream igualado R/W; MiB/s | Atual R/W; MiB/s |
| --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 2066/0; 2.203 | 2066/0; 2.203 | 2066/0; 2.203 |
| 512 | 257 | 2 | 4130/0; 2.204 | 4130/0; 2.204 | 4130/0; 2.204 |
| 512 | 4096 | 1 | 2066/0; 2.405 | 2066/0; 2.405 | 2066/0; 2.420 |
| 512 | 4096 | 2 | 4130/0; 2.406 | 4130/0; 2.406 | 4640/0; 2.155 |
| 4096 | 257 | 1 | 2305/0; 1.993 | 2305/0; 1.993 | 2305/0; 1.993 |
| 4096 | 257 | 2 | 4357/0; 2.099 | 4357/0; 2.099 | 4357/0; 2.099 |
| 4096 | 4096 | 1 | 2305/0; 2.169 | 2305/0; 2.169 | 2305/0; 2.169 |
| 4096 | 4096 | 2 | 4609/0; 2.170 | 4609/0; 2.170 | 4609/0; 2.170 |

### Gravações de metadados e múltiplos blocos

Perfil 1. Metadados incluem MBR/boot, FATs e cluster raiz; os demais setores são dados dos arquivos.

| Cluster B | Bloco B | Arquivos | Upstream padrão meta/multi | Upstream igualado meta/multi | Atual meta/multi |
| --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 755; 0 | 2080; 0 | 10240; 0 |
| 512 | 257 | 2 | 1474; 0 | 4257; 0 | 20479; 16 |
| 512 | 4096 | 1 | 723; 0 | 2080; 0 | 10240; 2 |
| 512 | 4096 | 2 | 1443; 0 | 4222; 0 | 20479; 2 |
| 4096 | 257 | 1 | 516; 0 | 516; 0 | 1280; 2048 |
| 4096 | 257 | 2 | 781; 0 | 785; 0 | 2559; 0 |
| 4096 | 4096 | 1 | 516; 0 | 516; 0 | 1280; 2048 |
| 4096 | 4096 | 2 | 1035; 0 | 1035; 0 | 2559; 4096 |

## Velocidade medida no computador

Mediana em MiB/s, perfil 0, sem latências físicas. Inclui chamadas da biblioteca, backend em RAM e geração/verificação do payload. Essas taxas não estimam diretamente o throughput em um SD ou em um microcontrolador.

| Fase | Cluster B | Bloco B | Arquivos | Upstream padrão | Upstream igualado | Atual |
| --- | --- | --- | --- | --- | --- | --- |
| write | 512 | 257 | 1 | 643.3 | 714.2 | 429.9 |
| write | 512 | 257 | 2 | 654.4 | 631.3 | 347.8 |
| write | 512 | 4096 | 1 | 714.9 | 767.9 | 451.2 |
| write | 512 | 4096 | 2 | 731.6 | 761.6 | 456.6 |
| write | 4096 | 257 | 1 | 795.8 | 880.5 | 775.6 |
| write | 4096 | 257 | 2 | 736.7 | 891.5 | 761.3 |
| write | 4096 | 4096 | 1 | 867.7 | 947.6 | 790.4 |
| write | 4096 | 4096 | 2 | 826.2 | 908.2 | 778.3 |
| read | 512 | 257 | 1 | 713.2 | 754.4 | 676.9 |
| read | 512 | 257 | 2 | 690.4 | 741.9 | 684.3 |
| read | 512 | 4096 | 1 | 895.9 | 948.7 | 901.5 |
| read | 512 | 4096 | 2 | 898.4 | 944.3 | 847.5 |
| read | 4096 | 257 | 1 | 741.9 | 764.4 | 706.2 |
| read | 4096 | 257 | 2 | 700.8 | 746.6 | 695.9 |
| read | 4096 | 4096 | 1 | 943.2 | 972.2 | 910.2 |
| read | 4096 | 4096 | 2 | 941.3 | 972.6 | 918.2 |

## Integridade e interpretação

Foram concluídas 336 execuções de volume: 112 por configuração, com 672 fases medidas. Em cada volume, um auditor independente percorreu a FAT primária, confirmou tamanho, conteúdo byte a byte e fim exato da cadeia. A biblioteca também releu os arquivos após remontar. As contagens de I/O e tempos virtuais foram idênticos nas sete repetições de cada cenário.

| Configuração | Volumes auditados | Volumes com FATs iguais | Setores FAT divergentes |
| --- | --- | --- | --- |
| upstream_native | 112 | 0 | 3–33 |
| upstream_equal | 112 | 0 | 3–33 |
| current_native | 112 | 112 | 0–0 |

A divergência da FAT secundária upstream foi registrada como resultado da auditoria, não ocultada para declarar interoperabilidade completa. A validação de conteúdo pelo FAT primário não elimina essa diferença. O ensaio não valida perda de energia nem equivale a CHKDSK/fsck.

A versão atual mantém espelhamento e barreiras de persistência adicionais. Com clusters pequenos, o custo observado de metadados é grande. O ganho de RAM do padrão atual não implica ganho de escrita. O agrupamento de múltiplos blocos ajuda em alguns cenários com clusters de 4 KiB, mas não garante menos setores escritos. Esta comparação não isola quanto do custo vem do espelhamento, das barreiras ou de outras alterações; seria necessário medir variantes específicas para atribuir cada parcela.

Na leitura do perfil 1, sete dos oito cenários têm contagens de setores iguais. Com clusters de 512 bytes, blocos de 4 KiB e dois leitores alternados, a versão atual lê 4.640 setores contra 4.130 do upstream, aumento de 12,35%, e a taxa modelada cai de 2,406 para 2,155 MiB/s. As demais pequenas diferenças de tempo virtual decorrem de polls de progresso; os tempos do computador também incluem o custo de implementação e variação do host.

## Evidências e reprodução

- `tests/run_upstream_comparison.ps1`: extrai o commit, copia o checkout, compila, executa e gera este relatório.
- `tests/upstream_comparison.c`: carga comum, backend e auditor independente.
- `tests/upstream_comparison_report.py`: valida repetições e calcula as tabelas.
- `tests/build/upstream-fat32-83-20261116/`: snapshots completos de lib, configuração igualada, shim MSVC, binários, objetos, logs, CSVs, probes de RAM e resultados brutos. Essa pasta é ignorada pelo Git.
- `tests/upstream_comparison.json`: resultados consolidados, dispersão dos tempos, revisão e hashes das fontes medidas. Os números se aplicam a esses snapshots.

```powershell
./tests/run_upstream_comparison.ps1
```
