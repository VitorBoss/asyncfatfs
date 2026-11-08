# Comparação FAT32 8.3 com upstream master

Data: 16/11/2026, America/Sao_Paulo.

A configuração atual reduz a RAM estática em relação ao padrão upstream, principalmente por usar metade do cache. Com cache igualado, ela usa um pouco mais de RAM. A quantidade de setores lidos coincide em 7 dos oito cenários de leitura do perfil de latência. Com clusters de 512 bytes, a diferença da taxa modelada de escrita em relação ao upstream igualado fica entre -33.8% e -33.1%. O upstream deixa a segunda FAT desatualizada em todos os volumes deste ensaio; a versão atual mantém as duas FATs iguais. Portanto, menos gravações no upstream também refletem comportamentos diferentes de manutenção dos metadados.

As velocidades abaixo são de execução no computador com mídia em RAM e de um modelo explícito de latência. Elas não medem um cartão SD físico nem permitem afirmar a velocidade real em PIC24.

## Revisões e configurações

- Upstream: `60c5f3580152410cb08342942cbbe3a30a449c24`, referência local `upstream/master`, sem atualização por fetch.
- Atual: checkout baseado em `399d3755e4b7f47777001fc0202dc79e9a26e8ed`, com as alterações locais registradas nos hashes de `tests/UPSTREAM_FAT32_83_COMPARISON_UPDATED.json`. A comparação não representa somente o commit HEAD.
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
- MSVC x64 `/O2 /std:c11 /W3 /WX`, mesmo computador e backend assíncrono para ambas as revisões. Sete repetições por cenário; taxas do computador calculadas pela mediana dos tempos. Execuções das revisões foram sequenciais, sem randomização; variação de frequência, escalonamento e aquecimento podem influenciar os tempos. Mínimos e máximos estão no JSON. O perfil de latência normal foi usado; o diagnóstico de latência reduzida da FAT secundária não está habilitado nesta comparação.
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
| 512 | 257 | 1 | 35/2803; 0.445 | 66/4128; 0.302 | 50/6197; 0.200 |
| 512 | 257 | 2 | 36/5570; 0.448 | 164/8353; 0.298 | 98/12422; 0.199 |
| 512 | 4096 | 1 | 21/2771; 0.450 | 37/4128; 0.302 | 50/6197; 0.201 |
| 512 | 4096 | 2 | 37/5539; 0.451 | 100/8318; 0.300 | 160/12420; 0.201 |
| 4096 | 257 | 1 | 516/2564; 0.464 | 516/2564; 0.464 | 516/2824; 0.580 |
| 4096 | 257 | 2 | 522/4877; 0.499 | 530/4881; 0.499 | 777/5653; 0.427 |
| 4096 | 4096 | 1 | 516/2564; 0.464 | 516/2564; 0.464 | 516/2824; 0.581 |
| 4096 | 4096 | 2 | 1034/5131; 0.464 | 1034/5131; 0.464 | 1034/5653; 0.580 |

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

### Diferenças com cache igualado

Variações da versão atual em relação ao upstream com quatro páginas. Reads e writes referem-se à fase de escrita; as taxas vêm das respectivas fases. Taxa negativa significa menor throughput; I/O negativo significa menos setores transferidos.

| Cluster B | Bloco B | Arquivos | Delta reads | Delta writes | Delta taxa leitura | Delta taxa escrita |
| --- | --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | -16 | +2069 | +0.01% | -33.79% |
| 512 | 257 | 2 | -66 | +4069 | +0.01% | -33.10% |
| 512 | 4096 | 1 | +13 | +2069 | +0.61% | -33.41% |
| 512 | 4096 | 2 | +60 | +4102 | -10.45% | -33.07% |
| 4096 | 257 | 1 | +0 | +260 | +0.01% | +24.89% |
| 4096 | 257 | 2 | +247 | +772 | +0.01% | -14.35% |
| 4096 | 4096 | 1 | +0 | +260 | -0.00% | +25.08% |
| 4096 | 4096 | 2 | +0 | +522 | -0.00% | +25.00% |

### Gravações de metadados e múltiplos blocos

Perfil 1. Metadados incluem MBR/boot, FATs e cluster raiz; os demais setores são dados dos arquivos.

| Cluster B | Bloco B | Arquivos | Upstream padrão meta/multi | Upstream igualado meta/multi | Atual meta/multi |
| --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 755; 0 | 2080; 0 | 4149; 0 |
| 512 | 257 | 2 | 1474; 0 | 4257; 0 | 8326; 0 |
| 512 | 4096 | 1 | 723; 0 | 2080; 0 | 4149; 0 |
| 512 | 4096 | 2 | 1443; 0 | 4222; 0 | 8324; 0 |
| 4096 | 257 | 1 | 516; 0 | 516; 0 | 776; 2048 |
| 4096 | 257 | 2 | 781; 0 | 785; 0 | 1557; 0 |
| 4096 | 4096 | 1 | 516; 0 | 516; 0 | 776; 2048 |
| 4096 | 4096 | 2 | 1035; 0 | 1035; 0 | 1557; 4096 |

## Velocidade medida no computador

Mediana em MiB/s, perfil 0, sem latências físicas. Inclui chamadas da biblioteca, backend em RAM e geração/verificação do payload. Essas taxas não estimam diretamente o throughput em um SD ou em um microcontrolador.

| Fase | Cluster B | Bloco B | Arquivos | Upstream padrão | Upstream igualado | Atual |
| --- | --- | --- | --- | --- | --- | --- |
| write | 512 | 257 | 1 | 691.1 | 772.4 | 484.2 |
| write | 512 | 257 | 2 | 684.8 | 686.1 | 419.7 |
| write | 512 | 4096 | 1 | 758.4 | 818.5 | 551.6 |
| write | 512 | 4096 | 2 | 790.6 | 824.3 | 528.4 |
| write | 4096 | 257 | 1 | 859.7 | 956.5 | 791.0 |
| write | 4096 | 257 | 2 | 764.5 | 975.2 | 797.7 |
| write | 4096 | 4096 | 1 | 913.7 | 1019.9 | 847.5 |
| write | 4096 | 4096 | 2 | 876.8 | 1001.2 | 798.7 |
| read | 512 | 257 | 1 | 582.9 | 591.5 | 552.5 |
| read | 512 | 257 | 2 | 566.2 | 583.9 | 544.3 |
| read | 512 | 4096 | 1 | 671.5 | 705.1 | 661.0 |
| read | 512 | 4096 | 2 | 671.8 | 704.3 | 623.2 |
| read | 4096 | 257 | 1 | 589.9 | 600.8 | 551.3 |
| read | 4096 | 257 | 2 | 576.5 | 590.8 | 528.6 |
| read | 4096 | 4096 | 1 | 695.2 | 714.6 | 671.9 |
| read | 4096 | 4096 | 2 | 701.4 | 716.8 | 670.0 |

## Integridade e interpretação

Foram concluídas 336 execuções de volume: 112 por configuração, com 672 fases medidas. Em cada volume, um auditor independente percorreu a FAT primária, confirmou tamanho, conteúdo byte a byte e fim exato da cadeia, além da quantidade total de clusters alocados, para detectar caudas extras. A biblioteca também releu os arquivos após remontar. As contagens de I/O e tempos virtuais foram idênticos nas sete repetições de cada cenário.

| Configuração | Volumes auditados | Volumes com FATs iguais | Setores FAT divergentes |
| --- | --- | --- | --- |
| upstream_native | 112 | 0 | 3–33 |
| upstream_equal | 112 | 0 | 3–33 |
| current_native | 112 | 112 | 0–0 |

A divergência da FAT secundária upstream foi registrada como resultado da auditoria, não ocultada para declarar interoperabilidade completa. A validação de conteúdo pelo FAT primário não elimina essa diferença. O ensaio não valida perda de energia nem equivale a CHKDSK/fsck.

A versão atual mantém espelhamento e barreiras de persistência adicionais. O ganho de RAM do padrão atual não implica ganho de escrita. O agrupamento de múltiplos blocos ajuda em alguns cenários com clusters de 4 KiB, mas não garante menos setores escritos. Esta comparação não isola quanto do custo vem do espelhamento, das barreiras ou de outras alterações; seria necessário medir variantes específicas para atribuir cada parcela.

Na leitura do perfil 1, 7 dos oito cenários têm contagens de setores iguais ao upstream igualado. As diferenças por cenário aparecem nas tabelas. Diferenças de taxa com as mesmas contagens de I/O decorrem dos polls de progresso no modelo; os tempos do computador também incluem o custo de implementação e variação do host.

## Evidências e reprodução

- `tests/run_upstream_comparison.ps1`: extrai o commit, copia o checkout, compila, executa e gera este relatório.
- `tests/upstream_comparison.c`: carga comum, backend e auditor independente.
- `tests/upstream_comparison_report.py`: valida repetições e calcula as tabelas.
- `tests/build/upstream-fat32-83-refresh-20261116/`: snapshots completos de lib, configuração igualada, shim MSVC, binários, objetos, logs, CSVs, probes de RAM e resultados brutos. Essa pasta é ignorada pelo Git.
- `tests/UPSTREAM_FAT32_83_COMPARISON_UPDATED.json`: resultados consolidados, dispersão dos tempos, revisão e hashes das fontes medidas. Os números se aplicam a esses snapshots.

```powershell
./tests/run_upstream_comparison.ps1 -OutputDirectory tests/build/upstream-fat32-83-refresh-20261116 -ReportPath tests/UPSTREAM_FAT32_83_COMPARISON_UPDATED.md
```
