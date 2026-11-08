# Correção da regressão de um escritor/257 bytes

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2, 21 repetições intercaladas, arquivos de 1 MiB. Medição final sem compilações concorrentes; CPU do host inclui polling, backend simulado e verificação dos dados. Não é medição em PIC24 ou cartão físico.

O caminho sem multibloco recupera a detecção anterior de pares de alocação. A busca em uma passagem permanece nos perfis multibloco; a escolha acontece na compilação, sem novo estado. A seleção da página, os timestamps, locks, ACKs, reserva única, duas FATs e journal não mudam.

A comparação de código gerado confirmou igualdade da detecção de grupos, flush, fwrite, fread e poll entre o padrão corrigido e a referência anterior à regressão. Nos dois perfis multibloco, essas funções são idênticas à versão otimizada imediatamente anterior. Isso preserva a otimização sem atribuir às oscilações de tempo uma mudança de código inexistente.

Referência upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. O upstream deixa diferenças entre as FATs neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas | Redução CPU upstream 8 páginas |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 6.0317 | +0.04% | -64.90% | -56.45% |
| Padrão | 257 | 2 | 6.9320 | -1.18% | -70.48% | -81.33% |
| Padrão | 4096 | 1 | 5.2153 | -0.69% | -52.81% | -50.82% |
| Padrão | 4096 | 2 | 5.3219 | -0.13% | -56.32% | -58.42% |
| Multibloco | 257 | 1 | 5.7261 | +0.05% | -56.54% | -48.52% |
| Multibloco | 257 | 2 | 6.6087 | +0.32% | -62.53% | -72.87% |
| Multibloco | 4096 | 1 | 5.1234 | -0.36% | -50.11% | -48.16% |
| Multibloco | 4096 | 2 | 5.1979 | -0.36% | -52.67% | -54.73% |
| Multibloco + consulta | 257 | 1 | 5.6946 | -0.01% | -55.68% | -47.71% |
| Multibloco + consulta | 257 | 2 | 6.5104 | -0.01% | -60.12% | -70.30% |
| Multibloco + consulta | 4096 | 1 | 5.0356 | +0.49% | -47.54% | -45.62% |
| Multibloco + consulta | 4096 | 2 | 5.1417 | +0.14% | -51.02% | -53.06% |

Referência antes da regressão, um escritor/257: 6.0212 ciclos/byte; corrigido: 6.0317. O código gerado dos caminhos verificados coincide; diferenças de tempo entre eles são ruído da medição. Os percentuais anteriores são medianas pareadas, e os de upstream são razões de medianas. Valores positivos indicam menos CPU.

RAM estática x64: 2728 bytes, igual aos dois baselines. As 48 variantes PIC24 passaram, com RAM estática preservada e pilha máxima de 380 bytes, incluindo chamadas internas/retornos e excluindo backend, callbacks da aplicação e rotinas do compilador/libc.

Todos os contadores de I/O, metadados, polling, sessões e paradas coincidem com os baselines. Auditoria das duas FATs, 11 cenários funcionais e documentação das 295 definições de função passaram.

Evidências: `tests/build/write-group-fix/`: snapshots before/after/reference, trials, final-matrix/report.json, functional/, docs.json e report.json. A referência é a versão anterior à busca em uma passagem; before contém a regressão.

## Controle da medição

A tabela usa a rodada `tests/build/write-group-fix/timing/`, com hora de compilação FAT fixa em 12:00:00 apenas nos snapshots do benchmark sob `COMPARISON_CPU_ONLY`. A biblioteca entregue e seus timestamps normais não foram alterados por esse controle. A primeira rodada, sem esse controle, divergiu até entre caminhos equivalentes e não é a rodada selecionada. Não atribuir as diferenças entre rodadas a um novo ganho da biblioteca.

Os testes funcionais e a matriz PIC24 usam as fontes normais em `tests/build/write-group-fix/`; os logs de desempenho selecionados, objetos, fontes de teste e `report.json` estão em `timing/`. O código gerado das cinco rotinas verificadas confirma a restauração do padrão e a preservação dos dois perfis multibloco.
