# CPU de escrita: vínculos FAT preparados por página

Upstream local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Fonte atual SHA-256: `71fb8618315d63891b57715983460a958686990f06dcf29db6f1417d22af4348`.

A é a versão anterior; B é a otimização mantida. Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados. A janela continua contendo somente clusters já pedidos. Não há buffer, campo ou reserva adicional.

## Alteração

Depois dos ACKs dos EOCs nas duas FATs, a fase BATCH_LINKS adquire uma vez o par de páginas da janela, prepara todos os vínculos reversos na primária e espelha a imagem uma vez. Antes fazia aquisição/marcação/cópia a cada vínculo. A janela já é limitada à mesma página FAT, inclusive quando intercala posições de escritores. Bits altos de cada entrada são preservados. O vínculo anterior, que pode pertencer a outra página, continua no caminho individual. Uma aquisição incompleta não altera entradas nem avança o progresso. A barreira posterior das duas FATs continua antes da publicação do diretório.

## Método

MSVC x64 /O2, 21 repetições alternadas, afinidade fixa e aquecimento, sem compilações concorrentes durante a medição. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, 257/4096 bytes por pedido, clusters de 512/4096 bytes, ritmos serial/intercalado. Padrão, leitura multibloco e consulta. CPU usa QueryThreadCycleTime no perfil de baixa latência; inclui biblioteca, backend RAM, polling e aplicação. Throughput host usa tempo real medido; não representa SD/PIC24. As leituras CPU usam 16 passagens. Deltas B/A são medianas das razões pareadas; positivo é pior. Upstream é a versão original, com mínimo multibloco quatro; a atual usa dois. Upstream não grava FAT2 no workload e tem espelhos diferentes ao final.

## Escritas de 257 bytes — clusters de 512 bytes

| Ritmo | Perfil | Escritores | CPU A → B (ciclos/B) | Δ pareado | Δ medianas | CPU upstream | Host A → B (MiB/s) |
|---|---|---:|---:|---:|---:|---:|---:|
| serial | padrão | 1 | 5.454 → 5.097 | -6.10% | -6.56% | 3.941 | 501.91 → 535.91 |
| serial | padrão | 2 | 5.841 → 5.614 | -3.69% | -3.88% | 3.861 | 469.19 → 486.27 |
| serial | _multi | 1 | 4.770 → 4.583 | -3.36% | -3.94% | 3.941 | 567.92 → 596.52 |
| serial | _multi | 2 | 5.191 → 5.157 | -0.67% | -0.66% | 3.861 | 523.52 → 528.64 |
| serial | _query | 1 | 4.782 → 4.594 | -4.03% | -3.95% | 3.941 | 571.43 → 596.55 |
| serial | _query | 2 | 5.315 → 5.064 | -4.54% | -4.73% | 3.861 | 513.00 → 538.53 |
| interleaved | padrão | 1 | 5.351 → 5.252 | -2.34% | -1.85% | 4.084 | 507.95 → 518.59 |
| interleaved | padrão | 2 | 5.306 → 5.085 | -4.26% | -4.18% | 3.950 | 511.20 → 533.55 |
| interleaved | _multi | 1 | 4.883 → 5.201 | -4.53% | +6.50% | 4.084 | 546.78 → 529.27 |
| interleaved | _multi | 2 | 4.871 → 4.634 | -4.30% | -4.87% | 3.950 | 557.03 → 583.79 |
| interleaved | _query | 1 | 4.952 → 4.733 | -3.87% | -4.41% | 4.084 | 536.57 → 580.62 |
| interleaved | _query | 2 | 4.851 → 4.752 | -1.92% | -2.03% | 3.950 | 565.28 → 576.32 |

## Confirmação isolada do perfil multibloco intercalado

A rodada ampla teve alta dispersão em um escritor/257: delta pareado e razão das medianas discordaram no sinal. Sem recompilar, os mesmos executáveis foram repetidos em 31 pares AB/BA, com três aquecimentos. As duas rodadas são mantidas no JSON.

| Escritores/bloco | CPU A → B (ciclos/B) | Δ pareado | Δ medianas |
|---|---:|---:|---:|
| 1/257 | 4.944 → 4.694 | -4.75% | -5.06% |
| 2/257 | 4.877 → 4.652 | -4.62% | -4.61% |
| 1/4096 | 4.059 → 3.802 | -6.02% | -6.34% |
| 2/4096 | 4.038 → 3.837 | -5.44% | -4.96% |

## Demais cenários de CPU — Δ B/A

Inclui regressões e variação do host; os ciclos e quartis completos estão no JSON.

| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |
|---|---|---:|---|---:|
| serial | read | 512 | 1/257 | -1.02% |
| serial | read | 512 | 2/257 | -1.97% |
| serial | write | 512 | 1/4096 | -7.59% |
| serial | read | 512 | 1/4096 | -0.37% |
| serial | write | 512 | 2/4096 | -7.67% |
| serial | read | 512 | 2/4096 | -0.19% |
| serial | write | 4096 | 1/257 | -3.57% |
| serial | read | 4096 | 1/257 | +0.17% |
| serial | write | 4096 | 2/257 | -2.01% |
| serial | read | 4096 | 2/257 | -0.50% |
| serial | write | 4096 | 1/4096 | -0.12% |
| serial | read | 4096 | 1/4096 | +0.33% |
| serial | write | 4096 | 2/4096 | +0.72% |
| serial | read | 4096 | 2/4096 | +0.37% |
| serial_multi | read | 512 | 1/257 | -2.23% |
| serial_multi | read | 512 | 2/257 | -2.03% |
| serial_multi | write | 512 | 1/4096 | -2.78% |
| serial_multi | read | 512 | 1/4096 | -0.13% |
| serial_multi | write | 512 | 2/4096 | -3.33% |
| serial_multi | read | 512 | 2/4096 | +0.18% |
| serial_multi | write | 4096 | 1/257 | -0.36% |
| serial_multi | read | 4096 | 1/257 | -2.74% |
| serial_multi | write | 4096 | 2/257 | -0.64% |
| serial_multi | read | 4096 | 2/257 | -1.30% |
| serial_multi | write | 4096 | 1/4096 | +0.58% |
| serial_multi | read | 4096 | 1/4096 | -0.28% |
| serial_multi | write | 4096 | 2/4096 | -0.48% |
| serial_multi | read | 4096 | 2/4096 | +0.42% |
| serial_query | read | 512 | 1/257 | -1.14% |
| serial_query | read | 512 | 2/257 | -0.68% |
| serial_query | write | 512 | 1/4096 | -4.75% |
| serial_query | read | 512 | 1/4096 | -1.23% |
| serial_query | write | 512 | 2/4096 | -3.88% |
| serial_query | read | 512 | 2/4096 | -0.51% |
| serial_query | write | 4096 | 1/257 | -1.73% |
| serial_query | read | 4096 | 1/257 | -0.76% |
| serial_query | write | 4096 | 2/257 | -0.34% |
| serial_query | read | 4096 | 2/257 | -0.88% |
| serial_query | write | 4096 | 1/4096 | -0.72% |
| serial_query | read | 4096 | 1/4096 | -0.57% |
| serial_query | write | 4096 | 2/4096 | -0.63% |
| serial_query | read | 4096 | 2/4096 | -0.61% |
| interleaved | read | 512 | 1/257 | -0.72% |
| interleaved | read | 512 | 2/257 | +0.09% |
| interleaved | write | 512 | 1/4096 | -2.12% |
| interleaved | read | 512 | 1/4096 | -0.20% |
| interleaved | write | 512 | 2/4096 | -1.96% |
| interleaved | read | 512 | 2/4096 | +0.92% |
| interleaved | write | 4096 | 1/257 | +3.09% |
| interleaved | read | 4096 | 1/257 | -0.73% |
| interleaved | write | 4096 | 2/257 | -0.03% |
| interleaved | read | 4096 | 2/257 | -1.00% |
| interleaved | write | 4096 | 1/4096 | +0.18% |
| interleaved | read | 4096 | 1/4096 | -0.06% |
| interleaved | write | 4096 | 2/4096 | -0.28% |
| interleaved | read | 4096 | 2/4096 | -0.21% |
| interleaved_multi | read | 512 | 1/257 | +1.39% |
| interleaved_multi | read | 512 | 2/257 | +1.07% |
| interleaved_multi | write | 512 | 1/4096 | -4.52% |
| interleaved_multi | read | 512 | 1/4096 | +2.20% |
| interleaved_multi | write | 512 | 2/4096 | -4.71% |
| interleaved_multi | read | 512 | 2/4096 | +0.54% |
| interleaved_multi | write | 4096 | 1/257 | -2.07% |
| interleaved_multi | read | 4096 | 1/257 | -0.02% |
| interleaved_multi | write | 4096 | 2/257 | +1.30% |
| interleaved_multi | read | 4096 | 2/257 | +0.35% |
| interleaved_multi | write | 4096 | 1/4096 | +2.10% |
| interleaved_multi | read | 4096 | 1/4096 | +0.28% |
| interleaved_multi | write | 4096 | 2/4096 | +0.67% |
| interleaved_multi | read | 4096 | 2/4096 | -0.16% |
| interleaved_query | read | 512 | 1/257 | -0.62% |
| interleaved_query | read | 512 | 2/257 | -0.17% |
| interleaved_query | write | 512 | 1/4096 | -4.46% |
| interleaved_query | read | 512 | 1/4096 | -0.54% |
| interleaved_query | write | 512 | 2/4096 | -3.60% |
| interleaved_query | read | 512 | 2/4096 | -0.50% |
| interleaved_query | write | 4096 | 1/257 | +0.75% |
| interleaved_query | read | 4096 | 1/257 | +0.86% |
| interleaved_query | write | 4096 | 2/257 | +0.64% |
| interleaved_query | read | 4096 | 2/257 | -0.30% |
| interleaved_query | write | 4096 | 1/4096 | +1.40% |
| interleaved_query | read | 4096 | 1/4096 | -1.06% |
| interleaved_query | write | 4096 | 2/4096 | +1.30% |
| interleaved_query | read | 4096 | 2/4096 | -1.04% |

## I/O preservado

Todos os 192 pares A/B de fases auditadas preservaram exatamente leituras, escritas, metadados por FAT/diretório, polls e sessões/blocos multibloco, em ambas as latências. Conteúdo integral, igualdade das FATs e ordem EOC/vínculo/diretório conferidos. A tabela abaixo usa padrão/intercalado; A e B têm os mesmos valores.

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

## RAM, pilha e persistência

Recursos x64/x86: `{"x64": {"before": {"static_ram_bytes": 4872, "code_bytes": 44912}, "after": {"static_ram_bytes": 4872, "code_bytes": 45076}}, "x86": {"before": {"static_ram_bytes": 4760, "code_bytes": 34602}, "after": {"static_ram_bytes": 4760, "code_bytes": 34682}}}`. Matriz PIC24 de 48 configurações: RAM estática igual em cada par; pilha interna máxima **378 B**, abaixo de 400 B (inclui PCs internos; exclui backend, aplicação e libc).

Passaram os 28 testes de alocação/cache, incluindo três escritores, volume quase cheio, reserva, páginas de espelho reconstruídas, fsync imediato, buffers em trânsito imutáveis e seis campanhas de corte/recuperação/retry nos três perfis. Detalhes das campanhas:

- requested_recovery_mirror_cache: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_multi: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_multi: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_query: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_query: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.

Evidências e fontes congeladas: `tests/build/small-write-cpu/final`. As alternativas de simplificação do fwrite e polling não foram mantidas; as primeiras rodadas estão em tests/build/small-write-cpu.
