# Correção da regressão de escrita sem multibloco

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2, 21 repetições intercaladas, arquivos de 1 MiB. Medição final sem compilações concorrentes; CPU do host inclui polling, backend simulado e verificação dos dados. Não é medição em PIC24 ou cartão físico.

O caminho sem multibloco recupera a ordem anterior de seleção no flush. A filtragem antecipada permanece no multibloco. A seleção da página, os timestamps, locks, ACKs, reserva única, duas FATs e journal não mudam.

A comparação de código gerado confirmou igualdade de flush, fwrite, fread e poll entre o padrão corrigido e a referência anterior à regressão. Nos dois perfis multibloco, essas funções são idênticas à versão otimizada imediatamente anterior. Isso preserva a otimização sem atribuir às oscilações de tempo uma mudança de código inexistente.

Referência upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. O upstream deixa diferenças entre as FATs neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas | Redução CPU upstream 8 páginas |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 6.1427 | -5.23% | -64.24% | -53.36% |
| Padrão | 257 | 2 | 7.0606 | -4.63% | -66.57% | -77.71% |
| Padrão | 4096 | 1 | 5.2413 | -2.90% | -52.05% | -48.74% |
| Padrão | 4096 | 2 | 5.4485 | -3.37% | -58.80% | -59.46% |
| Multibloco | 257 | 1 | 6.0477 | -0.91% | -61.70% | -50.99% |
| Multibloco | 257 | 2 | 6.8400 | -0.28% | -61.37% | -72.16% |
| Multibloco | 4096 | 1 | 5.1833 | +0.00% | -50.37% | -47.10% |
| Multibloco | 4096 | 2 | 5.3146 | +0.85% | -54.90% | -55.54% |
| Multibloco + consulta | 257 | 1 | 5.9969 | +2.20% | -60.34% | -49.72% |
| Multibloco + consulta | 257 | 2 | 6.9115 | -1.15% | -63.05% | -73.96% |
| Multibloco + consulta | 4096 | 1 | 5.2490 | -0.57% | -52.28% | -48.96% |
| Multibloco + consulta | 4096 | 2 | 5.3460 | -0.38% | -55.82% | -56.46% |

Referência antes da regressão, um escritor/257: 6.1093 ciclos/byte; corrigido: 6.1427. O código gerado dos caminhos verificados coincide; diferenças de tempo entre eles são ruído da medição. Os percentuais anteriores são medianas pareadas, e os de upstream são razões de medianas. Valores positivos indicam menos CPU.

RAM estática x64: 2728 bytes, igual aos dois baselines. As 48 variantes PIC24 passaram, com RAM estática preservada e pilha máxima de 392 bytes, incluindo chamadas internas/retornos e excluindo backend, callbacks da aplicação e rotinas do compilador/libc.

Todos os contadores de I/O, metadados, polling, sessões e paradas coincidem com os baselines. Auditoria das duas FATs, 11 cenários funcionais e documentação das 295 definições de função passaram.

Evidências: `tests/build/write-flush-fix/`: snapshots before/after/reference, trials, final-matrix/report.json, functional/, docs.json e report.json. A referência é a versão anterior à filtragem antecipada; before contém a regressão.

A fonte completa após o pré-processamento também coincide com a referência anterior à regressão no perfil padrão e com a versão otimizada nos dois perfis multibloco, normalizando apenas a hora de compilação. Evidência: `preprocessed-equivalence.json`. A diferença medida de 0,55% frente à referência de código igual não reproduz os 2,06% anteriores; não constitui um novo ganho de CPU.
