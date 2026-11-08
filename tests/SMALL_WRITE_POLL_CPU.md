# CPU: despacho único de append e busca limitada do par FAT

Upstream local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Fonte atual SHA-256: `9c321484f79c190b372135916133b68d2fade647dc9ef8c6621ed4d910baf9a7`.

A é a versão anterior com filtragem antecipada do flush; B é a combinação mantida abaixo. Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.

O ganho mais consistente está em dois escritores/257 bytes: padrão serial -5.95% e intercalado -3.06% de CPU pareada. Com um escritor, os resultados são pequenos ou dependem da dispersão; não há ganho geral comprovado. Razão das medianas e delta pareado podem discordar no sinal, como no padrão serial de um escritor. As regressões fora do alvo também estão registradas.

## Alterações e persistência

collectIdle conserva o campo original de seis bits. A alternativa uint8_t coube na união sem aumentar RAM, mas não apresentou ganho consistente com um escritor: a primeira confirmação marcou +1,57% no padrão intercalado. Foi descartada. Seus dados completos permanecem em tests/build/small-write-poll-cpu/final/rejected-report.md e JSON. Todos os tamanhos medidos de estado, handle, append e extensão são iguais na versão mantida.

afatfs_fileOperationsPoll classifica cada operação ativa uma vez. Um append válido usa diretamente sua continuação nos perfis nativos, sem repetir o despacho genérico. A espera da fase COLLECT é tratada no mesmo ramo: backend ocupado não avança o contador; sem backend pendente, incrementa uma vez por passe enquanto o valor é diferente de 63. No limite, usa a continuação completa original, que sela todas as coleções e aguarda ACKs. Operações de sync/close e handles inválidos continuam no despacho original. Dois passes consecutivos continuam contando dois passes.

afatfs_collectNext encerra a varredura assim que encontra as duas páginas DIRTY do par. A aquisição em estados ausente/limpo/em transferência, os três acessos lógicos do cache e as cópias de quatro bytes versus reparo integral permanecem. Não muda limites da janela, reservas, cacheTimer, submissões, ACKs ou a ordem EOC nas duas FATs → vínculos → diretório.

## Método

MSVC x64 /O2, 21 repetições alternadas, afinidade fixa, aquecimento e sem compilações concorrentes durante CPU. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, blocos 257/4096, clusters 512/4096, serial/intercalado e padrão/multiread/query. QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação; throughput host usa tempo real. Não são medidas de PIC24 ou cartão físico. Leitura CPU usa 16 passagens. Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. Amostras e quartis estão no JSON; diferenças pequenas podem ser ruído.

Upstream original usa mínimo multibloco quatro; atual dois. O controle anterior indicou que esse limiar não explica a diferença principal. Upstream não grava FAT2 neste workload e termina com espelhos diferentes; as garantias de espelhamento não são equivalentes.

## Escritas de 257 bytes — clusters de 512 bytes

| Ritmo | Perfil | Escritores | CPU A → B ciclos/B | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| serial | padrão | 1 | 4.259 → 4.267 | -1.88% | +0.18% | 4.160 | +2.38% → +2.55% | 645.37 → 610.46 |
| serial | padrão | 2 | 4.662 → 4.423 | -5.95% | -5.13% | 3.919 | +18.95% → +12.84% | 590.53 → 620.27 |
| serial | _multi | 1 | 4.880 → 4.423 | -3.42% | -9.35% | 4.160 | +17.29% → +6.32% | 560.51 → 614.33 |
| serial | _multi | 2 | 4.720 → 4.369 | -7.20% | -7.43% | 3.919 | +20.42% → +11.47% | 579.42 → 626.06 |
| serial | _query | 1 | 5.603 → 4.295 | -21.43% | -23.36% | 4.160 | +34.69% → +3.23% | 473.48 → 617.51 |
| serial | _query | 2 | 4.803 → 4.357 | -9.75% | -9.28% | 3.919 | +22.53% → +11.15% | 564.76 → 630.28 |
| interleaved | padrão | 1 | 4.523 → 4.290 | -1.25% | -5.15% | 4.163 | +8.64% → +3.05% | 601.90 → 631.71 |
| interleaved | padrão | 2 | 4.302 → 4.169 | -3.06% | -3.08% | 4.004 | +7.45% → +4.14% | 634.66 → 656.86 |
| interleaved | _multi | 1 | 4.407 → 4.225 | -3.66% | -4.12% | 4.163 | +5.84% → +1.48% | 608.94 → 613.20 |
| interleaved | _multi | 2 | 4.361 → 4.113 | -6.27% | -5.69% | 4.004 | +8.92% → +2.73% | 621.93 → 667.78 |
| interleaved | _query | 1 | 4.395 → 4.403 | -1.82% | +0.19% | 4.163 | +5.56% → +5.76% | 615.88 → 616.48 |
| interleaved | _query | 2 | 4.400 → 4.151 | -5.60% | -5.67% | 4.004 | +9.90% → +3.67% | 626.66 → 662.89 |

## Demais cenários — Δ CPU pareado B/A

Inclui variações e regressões; não generalizar um ganho a todos os caminhos.

| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |
|---|---|---:|---|---:|
| serial | read | 512 | 1/257 | -1.69% |
| serial | read | 512 | 2/257 | -0.94% |
| serial | write | 512 | 1/4096 | +2.25% |
| serial | read | 512 | 1/4096 | +0.50% |
| serial | write | 512 | 2/4096 | +0.81% |
| serial | read | 512 | 2/4096 | -0.78% |
| serial | write | 4096 | 1/257 | -0.87% |
| serial | read | 4096 | 1/257 | -1.20% |
| serial | write | 4096 | 2/257 | -0.34% |
| serial | read | 4096 | 2/257 | -1.21% |
| serial | write | 4096 | 1/4096 | +0.28% |
| serial | read | 4096 | 1/4096 | -0.31% |
| serial | write | 4096 | 2/4096 | +0.14% |
| serial | read | 4096 | 2/4096 | -0.49% |
| serial_multi | read | 512 | 1/257 | -0.61% |
| serial_multi | read | 512 | 2/257 | +1.38% |
| serial_multi | write | 512 | 1/4096 | -0.34% |
| serial_multi | read | 512 | 1/4096 | -0.30% |
| serial_multi | write | 512 | 2/4096 | -0.25% |
| serial_multi | read | 512 | 2/4096 | -1.29% |
| serial_multi | write | 4096 | 1/257 | -0.65% |
| serial_multi | read | 4096 | 1/257 | +0.27% |
| serial_multi | write | 4096 | 2/257 | -0.74% |
| serial_multi | read | 4096 | 2/257 | +0.36% |
| serial_multi | write | 4096 | 1/4096 | -0.32% |
| serial_multi | read | 4096 | 1/4096 | -0.59% |
| serial_multi | write | 4096 | 2/4096 | -0.47% |
| serial_multi | read | 4096 | 2/4096 | +0.39% |
| serial_query | read | 512 | 1/257 | -0.21% |
| serial_query | read | 512 | 2/257 | +0.99% |
| serial_query | write | 512 | 1/4096 | -2.13% |
| serial_query | read | 512 | 1/4096 | +1.29% |
| serial_query | write | 512 | 2/4096 | -2.17% |
| serial_query | read | 512 | 2/4096 | -1.66% |
| serial_query | write | 4096 | 1/257 | -2.54% |
| serial_query | read | 4096 | 1/257 | -0.96% |
| serial_query | write | 4096 | 2/257 | -1.34% |
| serial_query | read | 4096 | 2/257 | +0.24% |
| serial_query | write | 4096 | 1/4096 | -0.47% |
| serial_query | read | 4096 | 1/4096 | -0.38% |
| serial_query | write | 4096 | 2/4096 | -1.29% |
| serial_query | read | 4096 | 2/4096 | -0.89% |
| interleaved | read | 512 | 1/257 | -2.25% |
| interleaved | read | 512 | 2/257 | -1.32% |
| interleaved | write | 512 | 1/4096 | +0.02% |
| interleaved | read | 512 | 1/4096 | -2.18% |
| interleaved | write | 512 | 2/4096 | -0.30% |
| interleaved | read | 512 | 2/4096 | -1.95% |
| interleaved | write | 4096 | 1/257 | -0.46% |
| interleaved | read | 4096 | 1/257 | -1.90% |
| interleaved | write | 4096 | 2/257 | +1.21% |
| interleaved | read | 4096 | 2/257 | -1.54% |
| interleaved | write | 4096 | 1/4096 | -1.39% |
| interleaved | read | 4096 | 1/4096 | -0.24% |
| interleaved | write | 4096 | 2/4096 | -1.47% |
| interleaved | read | 4096 | 2/4096 | +0.19% |
| interleaved_multi | read | 512 | 1/257 | -0.04% |
| interleaved_multi | read | 512 | 2/257 | +1.08% |
| interleaved_multi | write | 512 | 1/4096 | -1.51% |
| interleaved_multi | read | 512 | 1/4096 | -2.27% |
| interleaved_multi | write | 512 | 2/4096 | -1.98% |
| interleaved_multi | read | 512 | 2/4096 | +0.38% |
| interleaved_multi | write | 4096 | 1/257 | -2.38% |
| interleaved_multi | read | 4096 | 1/257 | -2.64% |
| interleaved_multi | write | 4096 | 2/257 | -1.52% |
| interleaved_multi | read | 4096 | 2/257 | -0.74% |
| interleaved_multi | write | 4096 | 1/4096 | -1.78% |
| interleaved_multi | read | 4096 | 1/4096 | -0.44% |
| interleaved_multi | write | 4096 | 2/4096 | -1.11% |
| interleaved_multi | read | 4096 | 2/4096 | -0.75% |
| interleaved_query | read | 512 | 1/257 | +1.04% |
| interleaved_query | read | 512 | 2/257 | +1.79% |
| interleaved_query | write | 512 | 1/4096 | -0.53% |
| interleaved_query | read | 512 | 1/4096 | +2.04% |
| interleaved_query | write | 512 | 2/4096 | -0.46% |
| interleaved_query | read | 512 | 2/4096 | +1.22% |
| interleaved_query | write | 4096 | 1/257 | +0.25% |
| interleaved_query | read | 4096 | 1/257 | +1.46% |
| interleaved_query | write | 4096 | 2/257 | -0.49% |
| interleaved_query | read | 4096 | 2/257 | +2.43% |
| interleaved_query | write | 4096 | 1/4096 | +0.29% |
| interleaved_query | read | 4096 | 1/4096 | +0.87% |
| interleaved_query | write | 4096 | 2/4096 | -0.06% |
| interleaved_query | read | 4096 | 2/4096 | +1.18% |

## I/O e recursos

Os 192 pares auditados A/B coincidem em leituras, escritas, metadados por FAT/diretório, polls e sessões/blocos multibloco nas duas latências. Throughput virtual igual. Conteúdo integral, FATs iguais e ordem de publicação conferidos.

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

| ABI | RAM A → B bytes | Código A → B bytes |
|---|---:|---:|
| x64 | 4872 → 4872 | 46028 → 45923 |
| x86 | 4760 → 4760 | 35010 → 34800 |

| ABI | Estado global A → B | Handle A → B | Append A → B | Extensão A → B |
|---|---:|---:|---:|---:|
| 32 | 656 → 656 | 96 → 96 | 20 → 20 | 36 → 36 |
| 64 | 760 → 760 | 120 → 120 | 20 → 20 | 48 → 48 |
| 16-normal | 512 → 512 | 76 → 76 | 16 → 16 | 26 → 26 |
| 16-short | 484 → 484 | 74 → 74 | 14 → 14 | 24 → 24 |

Matriz PIC24: 48 configurações com seções estáticas iguais; pilha interna máxima **378 bytes**, incluindo PCs internos e excluindo backend/aplicação/libc. Os quatro probes host passaram nos checks de flags/alinhamento; os quatro probes XC16 compilaram os checks de alinhamento.

## Validação de persistência

Passaram 28 testes de alocação/cache. O teste dirigido percorre todos os incrementos 0..62, todos os valores 0..63 com backend ocupado, handles inválidos e operação NONE. Confere dois passes consecutivos: 62 →63 e depois selamento de todos os escritores. Duas submissões de dados rejeitadas mantêm ambos aguardando a barreira, sem avanço indevido. Os nove cenários do par FAT foram repetidos nos slots 0/1, 1/0 e 6/7, cobrindo ambos os ordenamentos e o fim do cache, nos três perfis. Demais testes: três escritores, quase cheio, reserva única, fsync imediato, callbacks, prefixos e buffers em trânsito imutáveis. Campanhas:

- requested_recovery_mirror_cache: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_multi: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_multi: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_query: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_query: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.

Evidências congeladas: `tests/build/small-write-poll-cpu/final-dispatch`. As alternativas iniciais e a rodada com contador em byte estão no diretório pai; isoladamente os ganhos foram inconsistentes.
