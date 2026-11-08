# CPU: filtrar candidatas antes das políticas de flush

Upstream local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Fonte atual SHA-256: `bec48f9daaf4c157c653311781a6c325eb57a1643d2eeb6bd805df6d4dd44538`.

A é a versão anterior, com acesso direto ao par FAT sujo; B acrescenta a filtragem abaixo. Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.

No padrão, os quatro casos de 257 bytes/cluster 512 reduziram CPU pareada entre 9.23% e 13.87%. O I/O permaneceu idêntico. Nos perfis opcionais, vários deltas são pequenos e alguns são positivos; não há ganho geral comprovado nesses perfis. Todos os resultados estão registrados abaixo.

## Alteração

afatfs_flushCacheFiltered ignora páginas limpas, travadas e com timestamp que não pode superar a candidata atual antes de consultar as políticas de retenção. O perfil multibloco já usava essa ordem; o padrão passa a compartilhar o mesmo laço. As políticas são consultas puras: não avançam estados, timers ou I/O. A candidata final e o desempate pelo primeiro índice são preservados, inclusive timestamps iguais.

afatfs_collectFatPage rejeita setores fora das duas regiões FAT antes de percorrer os escritores. A diferença unsigned relativa ao início da FAT e duas subtrações evitam multiplicação com overflow. Se o setor é da primária ou do espelho de uma página válida, passa pelo filtro mesmo quando os endereços físicos dão a volta. O laço de proprietários permanece como antes.

Nenhuma alteração nos EOCs, vínculos, reservas, limites da coleção, timestamps do cache, locks, submissões ou ACKs. As barreiras das duas FATs antes do diretório permanecem.

## Método

MSVC x64 /O2, 21 repetições alternadas, afinidade fixa, aquecimento e sem compilações concorrentes durante CPU. FAT32 8.3, arquivos de 1 MiB, um/dois arquivos, 257/4096 bytes, clusters 512/4096 bytes, serial/intercalado e padrão/multiread/query. QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação; throughput host usa tempo real medido. Não são medições de PIC24 ou cartão físico. Leitura CPU tem 16 passagens. Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. O JSON registra quartis e todas as amostras; diferenças pequenas podem ser ruído.

Upstream original: mínimo multibloco quatro; atual: dois. O controle anterior mostrou que esse limiar não explica a diferença principal. Upstream não grava FAT2 neste workload e termina com espelhos distintos; as garantias de espelhamento não são iguais.

## Escritas de 257 bytes — clusters de 512 bytes

| Ritmo | Perfil | Escritores | CPU A → B ciclos/B | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| serial | padrão | 1 | 4.778 → 4.365 | -9.23% | -8.64% | 4.068 | +17.45% → +7.30% | 552.00 → 570.42 |
| serial | padrão | 2 | 5.269 → 4.677 | -11.26% | -11.24% | 3.909 | +34.79% → +19.64% | 504.49 → 567.99 |
| serial | _multi | 1 | 4.414 → 4.350 | -0.43% | -1.47% | 4.068 | +8.52% → +6.92% | 594.57 → 596.69 |
| serial | _multi | 2 | 4.740 → 4.772 | +0.10% | +0.66% | 3.909 | +21.27% → +22.07% | 557.48 → 562.90 |
| serial | _query | 1 | 4.888 → 4.404 | -2.59% | -9.90% | 4.068 | +20.16% → +8.27% | 526.79 → 583.43 |
| serial | _query | 2 | 4.745 → 4.785 | +0.22% | +0.86% | 3.909 | +21.38% → +22.42% | 563.11 → 570.76 |
| interleaved | padrão | 1 | 5.044 → 4.349 | -13.87% | -13.78% | 4.060 | +24.23% → +7.12% | 515.44 → 601.03 |
| interleaved | padrão | 2 | 4.800 → 4.340 | -9.52% | -9.58% | 3.958 | +21.28% → +9.65% | 559.47 → 619.79 |
| interleaved | _multi | 1 | 4.491 → 4.417 | -0.25% | -1.65% | 4.060 | +10.62% → +8.80% | 524.99 → 582.14 |
| interleaved | _multi | 2 | 4.406 → 4.422 | +0.26% | +0.36% | 3.958 | +11.32% → +11.72% | 602.74 → 600.35 |
| interleaved | _query | 1 | 4.701 → 4.616 | -1.32% | -1.81% | 4.060 | +15.79% → +13.69% | 560.22 → 531.66 |
| interleaved | _query | 2 | 4.394 → 4.398 | +0.30% | +0.10% | 3.958 | +11.01% → +11.12% | 614.04 → 609.72 |

## Demais cenários — Δ CPU pareado B/A

Inclui variações e regressões; não generalizar um ganho a todos os caminhos.

| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |
|---|---|---:|---|---:|
| serial | read | 512 | 1/257 | +0.93% |
| serial | read | 512 | 2/257 | +0.24% |
| serial | write | 512 | 1/4096 | -7.31% |
| serial | read | 512 | 1/4096 | +0.18% |
| serial | write | 512 | 2/4096 | -8.16% |
| serial | read | 512 | 2/4096 | -1.00% |
| serial | write | 4096 | 1/257 | -6.14% |
| serial | read | 4096 | 1/257 | +0.79% |
| serial | write | 4096 | 2/257 | -8.80% |
| serial | read | 4096 | 2/257 | +1.03% |
| serial | write | 4096 | 1/4096 | -6.04% |
| serial | read | 4096 | 1/4096 | +0.53% |
| serial | write | 4096 | 2/4096 | -5.43% |
| serial | read | 4096 | 2/4096 | +0.83% |
| serial_multi | read | 512 | 1/257 | +0.23% |
| serial_multi | read | 512 | 2/257 | +0.08% |
| serial_multi | write | 512 | 1/4096 | -0.80% |
| serial_multi | read | 512 | 1/4096 | -0.48% |
| serial_multi | write | 512 | 2/4096 | -0.23% |
| serial_multi | read | 512 | 2/4096 | -1.76% |
| serial_multi | write | 4096 | 1/257 | -1.61% |
| serial_multi | read | 4096 | 1/257 | +0.36% |
| serial_multi | write | 4096 | 2/257 | +0.38% |
| serial_multi | read | 4096 | 2/257 | -0.40% |
| serial_multi | write | 4096 | 1/4096 | -0.50% |
| serial_multi | read | 4096 | 1/4096 | -0.47% |
| serial_multi | write | 4096 | 2/4096 | -1.02% |
| serial_multi | read | 4096 | 2/4096 | -0.20% |
| serial_query | read | 512 | 1/257 | -0.21% |
| serial_query | read | 512 | 2/257 | -1.47% |
| serial_query | write | 512 | 1/4096 | -0.96% |
| serial_query | read | 512 | 1/4096 | +0.37% |
| serial_query | write | 512 | 2/4096 | -1.15% |
| serial_query | read | 512 | 2/4096 | -1.06% |
| serial_query | write | 4096 | 1/257 | -2.74% |
| serial_query | read | 4096 | 1/257 | +0.28% |
| serial_query | write | 4096 | 2/257 | -0.69% |
| serial_query | read | 4096 | 2/257 | -0.82% |
| serial_query | write | 4096 | 1/4096 | -2.50% |
| serial_query | read | 4096 | 1/4096 | +0.49% |
| serial_query | write | 4096 | 2/4096 | -2.00% |
| serial_query | read | 4096 | 2/4096 | +1.16% |
| interleaved | read | 512 | 1/257 | -0.40% |
| interleaved | read | 512 | 2/257 | -0.58% |
| interleaved | write | 512 | 1/4096 | -8.29% |
| interleaved | read | 512 | 1/4096 | +2.08% |
| interleaved | write | 512 | 2/4096 | -4.96% |
| interleaved | read | 512 | 2/4096 | +2.02% |
| interleaved | write | 4096 | 1/257 | -5.55% |
| interleaved | read | 4096 | 1/257 | -0.88% |
| interleaved | write | 4096 | 2/257 | -6.11% |
| interleaved | read | 4096 | 2/257 | +0.21% |
| interleaved | write | 4096 | 1/4096 | -4.21% |
| interleaved | read | 4096 | 1/4096 | +0.41% |
| interleaved | write | 4096 | 2/4096 | -4.40% |
| interleaved | read | 4096 | 2/4096 | -0.56% |
| interleaved_multi | read | 512 | 1/257 | -0.62% |
| interleaved_multi | read | 512 | 2/257 | +0.33% |
| interleaved_multi | write | 512 | 1/4096 | +0.64% |
| interleaved_multi | read | 512 | 1/4096 | -0.59% |
| interleaved_multi | write | 512 | 2/4096 | +1.30% |
| interleaved_multi | read | 512 | 2/4096 | -1.62% |
| interleaved_multi | write | 4096 | 1/257 | +0.37% |
| interleaved_multi | read | 4096 | 1/257 | +1.44% |
| interleaved_multi | write | 4096 | 2/257 | +0.07% |
| interleaved_multi | read | 4096 | 2/257 | +1.49% |
| interleaved_multi | write | 4096 | 1/4096 | -0.18% |
| interleaved_multi | read | 4096 | 1/4096 | +2.97% |
| interleaved_multi | write | 4096 | 2/4096 | -1.78% |
| interleaved_multi | read | 4096 | 2/4096 | +1.32% |
| interleaved_query | read | 512 | 1/257 | +0.50% |
| interleaved_query | read | 512 | 2/257 | +1.29% |
| interleaved_query | write | 512 | 1/4096 | -0.17% |
| interleaved_query | read | 512 | 1/4096 | -0.52% |
| interleaved_query | write | 512 | 2/4096 | -0.27% |
| interleaved_query | read | 512 | 2/4096 | +1.91% |
| interleaved_query | write | 4096 | 1/257 | -1.56% |
| interleaved_query | read | 4096 | 1/257 | +0.28% |
| interleaved_query | write | 4096 | 2/257 | -0.77% |
| interleaved_query | read | 4096 | 2/257 | -0.33% |
| interleaved_query | write | 4096 | 1/4096 | -0.25% |
| interleaved_query | read | 4096 | 1/4096 | +0.27% |
| interleaved_query | write | 4096 | 2/4096 | -2.86% |
| interleaved_query | read | 4096 | 2/4096 | +0.09% |

## I/O preservado

Os 192 pares auditados A/B coincidem em leituras, escritas, metadados por FAT/diretório, polls, sessões e blocos multibloco nas duas latências. O throughput virtual também é igual. Conteúdo integral, FATs iguais e ordem EOC/vínculo/diretório foram conferidos.

| Latência | Escritores/bloco | R | W | Metadados |
|---:|---|---:|---:|---:|
| 0 | 1/257 | 19 | 2730 | 682 |
| 0 | 2/257 | 70 | 4947 | 722 |
| 0 | 1/4096 | 19 | 3202 | 1154 |
| 0 | 2/4096 | 35 | 5656 | 1560 |
| 1 | 1/257 | 27 | 2730 | 682 |
| 1 | 2/257 | 39 | 4826 | 722 |
| 1 | 1/4096 | 19 | 3189 | 1141 |
| 1 | 2/4096 | 35 | 5453 | 1357 |

## Recursos e validação

| ABI | RAM A → B bytes | Código A → B bytes |
|---|---:|---:|
| x64 | 4872 → 4872 | 45327 → 46028 |
| x86 | 4760 → 4760 | 34944 → 35010 |

Matriz PIC24 de 48 configurações com seções estáticas iguais em cada par. Pilha interna máxima **378 bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.

Passaram 28 testes de alocação/cache. O teste do espelho inclui agora dois escritores e quatro geometrias de endereçamento: normal, LBA com volta, comprimento acima de 2³¹ e UINT32_MAX. Confere ambas as FATs e setores sem proprietário. Os nove cenários anteriores de par sujo, cacheTimer, cópia parcial e propriedade do backend continuam passando nos três perfis. Também passaram quase cheio, três escritores, reserva única e fsync imediato. Campanhas:

- requested_recovery_mirror_cache: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_multi: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_multi: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_query: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_query: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.

Evidências congeladas: `tests/build/small-write-flush-cpu/final`. As três alternativas iniciais estão em tests/build/small-write-flush-cpu; a versão final usa o teste de intervalo sem overflow.
