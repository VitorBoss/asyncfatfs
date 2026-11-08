# CPU: conversão do índice de cache e endereço do cursor

Rodada de 17/11/2026. Baseline: versão aprovada na
[correção de 4096 bytes](FAT32_READ_4096_FIX.md). Upstream/master local:
`60c5f3580152410cb08342942cbbe3a30a449c24`.

## Alterações

Na configuração padrão, com pipeline de alocação e sem leitura multibloco:

- O índice de um buffer é calculado a partir de um offset sem sinal, após verificar
  que ele está dentro da área do cache. Evita a correção de sinal da divisão anterior.
  Buffers válidos, inclusive ponteiros para o interior de uma página, mantêm o mesmo índice.
- Para clusters de um setor, o setor físico do cursor é `clusterStart + cursorCluster - 2`.
  O índice de setor dentro do cluster é sempre zero, dispensando máscara e multiplicação.
  A raiz FAT16 continua sendo tratada primeiro; clusters maiores mantêm o cálculo geral.

O assembly XC16 confirmou que o caminho de um setor retorna sem as duas chamadas
anteriores a `afatfs_sectorIndexInCluster()` e `afatfs_fileClusterToPhysical()`.
Não houve medição de ciclos no PIC24.

Não há campos, buffers, macros públicas ou estado novo. A aplicação global dos atalhos
foi testada e descartada porque piorou os perfis multibloco. Neles, e nas configurações
com API de compatibilidade, LFN, exFAT, journal ou freefile, o código anterior é preservado.
A incorporação da função de obtenção do setor retido e a correção de 4096 bytes permanecem.

## CPU medida

MSVC `/O2`, x64, FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB por leitor.
**41 rodadas pareadas**, ordem alternada, **32 passagens** por arquivo, afinidade fixa ao
mesmo processador lógico e verificação integral dos dados. Não houve compilações ou
outras suítes simultâneas à medição.

Configuração padrão; ciclos por byte, menor é melhor:

| Bloco / leitores | Antes | Atual | Redução pareada mediana |
|---|---:|---:|---:|
| 257 / 1 | 3,5625 | 3,5443 | 0,46% |
| **257 / 2** | **3,5953** | **3,5766** | **0,34%** |
| 4096 / 1 | 2,8825 | 2,8462 | 1,32% |
| 4096 / 2 | 2,9873 | 2,9331 | 1,12% |

O ganho adicional no caso alvo é **pequeno**; não deve ser apresentado como uma aceleração
substancial ou garantia de tempo real. Os ganhos abaixo de 1% são sensíveis à variação
do host. As amostras individuais estão no JSON. A correção anterior de um leitor/4096
bytes foi preservada e esse caso melhorou novamente na medição pareada.

Comparação com upstream, padrão atual:

| Caso | Atual ciclos/byte | Upstream, cache 4 | Upstream, cache 8 original | Menos custo vs. cache 4 | Menos custo vs. cache 8 |
|---|---:|---:|---:|---:|---:|
| Dois leitores / 257 bytes | **3,5766** | 4,5665 | 4,7075 | **21,68%** | **24,02%** |
| Um leitor / 4096 bytes | **2,8462** | 3,8594 | 4,0194 | **26,25%** | **29,19%** |

As reduções pareadas usam a mediana das razões por rodada; não são a razão entre as
medianas das colunas. Os percentuais contra upstream comparam medianas de ciclos/byte,
não são aumentos percentuais de throughput. `QueryThreadCycleTime` inclui biblioteca,
backend sintético, poll e verificação dos dados no PC. Não são ciclos isolados da
biblioteca, execução no PIC24 ou medição de cartão SD físico. Método e limitações no
[relatório inicial](FAT32_READ_CPU.md).

Os ganhos multibloco foram preservados. O validador confirmou que o corpo de `fread()`
nos objetos MSVC x64 é idêntico antes/depois, com e sem consulta: 906 bytes, com os
mesmos hashes registrados no JSON. As variações pareadas desses perfis ficaram entre
-0,18% e +0,55%; não são apresentadas como ganhos novos.

## RAM e pilha

RAM estática da biblioteca, idêntica antes/depois e com/sem multibloco:

| ABI | Bytes |
|---|---:|
| PIC24, enums normais | 2494 |
| PIC24, enums curtos | 2474 |
| x86 | 2616 |
| x64 | 2728 |

XC16 2.10, PIC24FJ256GA702, `-Os`, limites internos conservadores em cascata, inalterados:

| Configuração | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | **328 bytes** | **302 bytes** |
| Multibloco e consulta | **314 bytes** | **302 bytes** |

Todos ficam **abaixo de 400 bytes**. O frame de `fread()` permanece em 28 bytes.
A análise inclui chamadas internas de `asyncfatfs.c`, `fat_standard.c` e endereços de
retorno. Conforme o escopo acordado, driver, callbacks externos, libc/auxiliares do
compilador e interrupções são pontos de corte; não é a pilha completa da aplicação.

## I/O, persistência e testes

O validador confirmou igualdade anterior/atual de leituras, gravações, metadados, polls,
sessões multibloco, blocos agrupados, paradas e tempo virtual nos 32 casos normais do
padrão e nos 32 com consulta: leitura/escrita, dois perfis de latência, clusters de
512/4096 bytes, blocos de 257/4096 bytes e um/dois arquivos. Os casos de CPU também
conferem igualdade de I/O e polls em cada configuração correspondente.

Dois leitores/257 bytes mantêm **4130 leituras e zero gravações** por passagem durante
a leitura, iguais ao upstream. O throughput modelado do perfil lento permanece em
**2,203808 MiB/s**, contra **2,203565 MiB/s** no upstream. A redução de CPU não altera
a latência de dispositivo usada pelo modelo.

As duas FATs, reserva única, journal, barreiras, ACKs, retenções e propriedade dos
buffers não foram alterados. Nenhum flush ou validação de arquivo foi removido.

Passaram os dez casos/configurações com consulta: default, fat_legacy, fat_only, fat_lfn,
exfat_only, exfat_freefile, lfn_exfat, focused, backend e integration. Sem consulta passaram
default, focused, backend e integration. Passou a suíte de alocação, incluindo ordem de
persistência, três arquivos, solicitações intercaladas, volume quase cheio, metadados,
espelhamento, reservas e cache dos leitores. Conteúdo e remontagem foram conferidos,
incluindo falhas de ACK das duas FATs. Os testes não dependem dos VHDs perdidos.

As regressões específicas verificam início e último byte de todas as páginas de cache,
rejeição do endereço imediatamente após o cache, cálculo físico em clusters de um e oito
setores e prioridade da raiz FAT16. A geometria sintética é restaurada antes do I/O real.

## Evidência e reprodução

Diretório: `tests/build/fat32-read-cursor-cpu/`, snapshots before/after/upstream,
logs `cpu-*`, `final-query-tests`, `final-plain-tests` e `final-allocation-tests`.
Os baselines normais, probes anteriores e resultados normais upstream foram reutilizados
após conferir igualdade byte a byte das bibliotecas e igualdade do commit upstream.
CPU, benchmarks atuais, testes finais e probes atuais são desta versão.

O [JSON validado](FAT32_READ_CURSOR_CPU.json) contém hashes, amostras, contadores e análise
de pilha. `global-address-trials` preserva a tentativa global descartada; a tentativa
isolada da divisão está em `tests/build/fat32-read-index-cpu/`.

```powershell
./tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/fat32-read-cursor-cpu -Repetitions 41
./tests/run_multiread_comparison.ps1 -OutputDirectory tests/build/fat32-read-cursor-cpu -CurrentOnly -ReadSessionQuery
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-read-cursor-cpu/final-query-tests -ReadSessionQuery
python tests/read_cpu_report.py --directory tests/build/fat32-read-cursor-cpu --output tests/FAT32_READ_CURSOR_CPU.json --repetitions 41 --verify-multiread-code
```

Preservar os snapshots e executar a medição de CPU isoladamente.
