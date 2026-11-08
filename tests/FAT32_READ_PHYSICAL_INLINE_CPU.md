# CPU: incorporação do cálculo de setor no XC16

Rodada de 17/11/2026. Baseline: versão aprovada na
[otimização do cursor](FAT32_READ_CURSOR_CPU.md). Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alteração final

`afatfs_fileGetCursorPhysicalSector()` solicita inlining obrigatório **somente no XC16**.
Sua lógica, validações e resultados não mudaram. Os demais compiladores mantêm suas
decisões anteriores de inlining.

A atribuição direta de `discardable = false` foi mantida. A tentativa de testar o bit
antes de limpá-lo ficou mais lenta e foi descartada, conforme apontado pelo usuário.
Não existe essa condição adicional no código final.

Forçar a incorporação também no MSVC foi testado e rejeitado: apesar de `fread()` já ter
o mesmo corpo de máquina, outros trechos, incluindo o prefetch, mudaram e os resultados
pioraram. A versão final não força essa função no MSVC.

## Evidência no PIC24

XC16 2.10, PIC24FJ256GA702, `-Os`:

| Configuração | Função separada antes | Função separada atual | Pontos de chamada em `fread()` antes | Atuais |
|---|---|---|---:|---:|
| Padrão, enums normais | Sim | Não | 2 | **0** |
| Padrão, enums curtos | Sim | Não | 2 | **0** |
| Multibloco e consulta, enums normais | — | Não | — | **0** |
| Multibloco e consulta, enums curtos | — | Não | — | **0** |

São pontos estáticos no assembly, não duas chamadas obrigatórias por execução da API.
A mudança elimina chamadas separadas nesse caminho do código PIC24. Não foi executado
um benchmark no hardware; portanto, **não há percentual medido de aceleração no PIC24**.

## Comparação de CPU no PC

No MSVC x64, a versão final de `fread()` **e** de `fprefetch()` tem código de máquina
idêntico ao anterior, confirmado nos objetos antes das relocações, nas três configurações
medidas: padrão, multibloco sem consulta e multibloco com consulta. Não se atribui ganho
novo no PC a esta alteração.

A medição foi repetida: MSVC `/O2`, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB,
21 rodadas pareadas, ordem alternada, 32 passagens por arquivo, afinidade fixa e verificação
integral dos dados. Não houve compilações ou outras suítes simultâneas à medição.

Dois leitores, blocos de 257 bytes; medianas em ciclos por byte:

| Configuração | Antes desta alteração | Atual |
|---|---:|---:|
| Padrão | 3,5801 | 3,5887 |
| Multibloco sem consulta | 3,5975 | 3,5851 |
| Multibloco com consulta | 3,6609 | 3,6642 |
| Upstream, cache de quatro setores | — | 4,5973 |
| Upstream, cache original de oito setores | — | 4,7299 |

O custo atual do padrão é **21,94% menor** que upstream com o mesmo cache de quatro
setores e **24,13% menor** que upstream com oito. Essa diferença é acumulada das
otimizações anteriores, não um ganho novo desta rodada.

Nos quatro casos padrão, as variações pareadas ficaram entre -0,48% e -0,17%, apesar
dos corpos de máquina idênticos. Não são apresentadas como mudança de desempenho causada
por esta edição. As amostras e hashes estão no JSON. `QueryThreadCycleTime` inclui
biblioteca, backend sintético, poll e verificação dos dados; não são ciclos isolados da
biblioteca nem medição em cartão SD físico. Método no [relatório inicial](FAT32_READ_CPU.md).

## RAM e pilha

RAM estática da biblioteca, idêntica antes/depois:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

Limites conservadores de pilha interna em cascata no XC16, inalterados:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | **328 bytes** | **302 bytes** |
| Multibloco e consulta | **314 bytes** | **302 bytes** |

Todos permanecem abaixo de **400 bytes**. O frame de `fread()` permanece em 28 bytes.
O frame isolado de `fprefetch()` passa de 10 para 14 bytes, mas a chamada ao helper
deixa de existir e o máximo interno em cascata permanece igual.

A análise inclui chamadas internas de `asyncfatfs.c`, `fat_standard.c` e endereços de
retorno. Conforme o escopo acordado, driver, callbacks externos, libc/auxiliares do
compilador e interrupções são pontos de corte; não é a pilha completa da aplicação.

## I/O e persistência

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, paradas e tempo virtual nos 32 casos normais do
padrão e nos 32 com consulta: leitura/escrita, dois perfis de latência, clusters de
512/4096 bytes, blocos de 257/4096 bytes e um/dois arquivos. Os casos de CPU também
conferem igualdade de I/O e polls nas configurações correspondentes.

Dois leitores/257 bytes mantêm 4130 leituras e zero gravações durante a fase de leitura,
iguais ao upstream. O throughput modelado do perfil lento permanece em 2,203808 MiB/s,
contra 2,203565 MiB/s no upstream. Não há alteração da latência modelada do dispositivo.

As duas FATs, reserva única, journal, barreiras, ACKs, validações e propriedade dos
buffers não foram alterados. A edição final é apenas uma diretiva de compilação para
uma função interna pura; nenhum flush ou máquina de estados foi removido.

## Testes e reprodução

Passaram os dez casos/configurações com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. Passou a suíte de alocação, incluindo ordem de
persistência, três arquivos, solicitações intercaladas, volume quase cheio, metadados,
espelhamento, reservas e cache dos leitores. Conteúdo e remontagem foram conferidos,
inclusive sob falhas de ACK das duas FATs. Não foram necessários os VHDs perdidos.

Evidência final: `tests/build/fat32-read-physical-inline-cpu/`, com snapshots before/after/
upstream, logs `cpu-*`, `final-query-tests`, `final-plain-tests` e `final-allocation-tests`.
Os baselines normais, probes anteriores e resultados normais upstream foram reutilizados
após verificar igualdade byte a byte das bibliotecas e igualdade do commit upstream.
CPU, benchmarks atuais, probes atuais e testes finais são desta versão.

O [JSON validado](FAT32_READ_PHYSICAL_INLINE_CPU.json) contém hashes, amostras, contadores,
análise da pilha, contagem de chamadas PIC24 e identidade dos corpos de máquina MSVC.
As tentativas rejeitadas foram preservadas em `global-inline-trials` e em
`tests/build/fat32-read-hint-cpu/`; elas não representam o código final.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-physical-inline-cpu -Repetitions 21
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-physical-inline-cpu -CurrentOnly -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-physical-inline-cpu/final-query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-physical-inline-cpu --output tests/FAT32_READ_PHYSICAL_INLINE_CPU.json --repetitions 21 --verify-physical-inline
```

Preservar os snapshots e executar a medição de CPU isoladamente.
