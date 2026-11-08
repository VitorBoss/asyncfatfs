# CPU: coleção com o par FAT já sujo em cache

Upstream local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Fonte atual SHA-256: `318dc1687ca5f508f70536eff9685a6c0ef3a5cbfb49dc6d3edce28058d7e2e8`.

A é a versão anterior, com vínculos preparados por página; B acrescenta o caminho para o par FAT já DIRTY. Cache de oito setores, duas FATs, reserva especulativa única e RAM estática preservados.

Nos 12 casos de 257 bytes/cluster 512, a redução pareada de CPU ficou entre 5.80% e 9.43%, com I/O idêntico. A diferença para upstream diminuiu, mas permanece. Os resultados dos demais cenários, inclusive variações/regressões, estão registrados abaixo.

## Alteração e persistência

afatfs_collectNext procura as duas páginas na mesma passagem pelo cache. Se ambas estão DIRTY, suas imagens já foram pareadas e nenhuma está em uso pelo backend ou pode ser expulsa. Verifica a entrada livre e as reservas, grava o EOC na primária e copia somente seus quatro bytes para o espelho. Preserva os bits altos e as entradas vizinhas. Mantém os três acessos lógicos do caminho anterior: leitura, aquisição da primária e aquisição do espelho, com os mesmos timestamps e atualização de fsInfoDirty. Não altera writeTimestamp, dirtyEntries ou retainCount dessas páginas já sujas.

Página ausente, limpa ou em transferência usa o caminho anterior integral. O par só pode ser modificado quando as duas páginas estão disponíveis. As barreiras EOC nas duas FATs → vínculos nas duas FATs → diretório permanecem. A janela continua limitada aos pedidos já recebidos, sem nova reserva ou buffer.

O código gerado padrão MSVC de fwrite passou de 1658 para 1289 bytes; o total da biblioteca cresce conforme a tabela de recursos. O compilador também muda a organização do caminho de escrita. fread e poll mantêm os mesmos bytes no objeto padrão; diferenças de leitura pequenas podem refletir disposição do executável e ruído. Esses efeitos não comprovam o mesmo ganho no PIC24.

## Método

MSVC x64 /O2, 21 repetições alternadas AB/BA, afinidade fixa e aquecimento, sem compilações concorrentes durante CPU. FAT32 8.3, 1 MiB por arquivo, um/dois arquivos, 257/4096 bytes por pedido, clusters 512/4096 bytes, serial/intercalado, padrão/multiread/query. CPU QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação. Throughput host usa tempo real medido, não cartão físico. Leituras CPU usam 16 passagens. Δ pareado é a mediana das razões por rodada; Δ medianas é a razão das medianas. Diferenças pequenas perante a dispersão não comprovam ganho; quartis e amostras estão no JSON.

Upstream original usa mínimo multibloco quatro, atual dois. O controle anterior mostrou que esse limiar não explica a maior parte da diferença. Upstream não grava FAT2 neste workload e termina com espelhos diferentes; as garantias de persistência não são equivalentes.

## Escritas de 257 bytes — clusters de 512 bytes

| Ritmo | Perfil | Escritores | CPU A → B (ciclos/B) | Δ pareado | Δ medianas | CPU upstream | Excesso A → B sobre upstream | Host A → B MiB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| serial | padrão | 1 | 5.146 → 4.761 | -7.72% | -7.47% | 4.045 | +27.21% → +17.71% | 520.97 → 574.42 |
| serial | padrão | 2 | 5.690 → 5.308 | -6.39% | -6.72% | 3.929 | +44.83% → +35.09% | 480.28 → 508.98 |
| serial | _multi | 1 | 4.653 → 4.367 | -6.75% | -6.15% | 4.045 | +15.04% → +7.97% | 573.36 → 628.61 |
| serial | _multi | 2 | 5.237 → 4.744 | -9.43% | -9.43% | 3.929 | +33.31% → +20.74% | 517.72 → 569.44 |
| serial | _query | 1 | 4.642 → 4.419 | -6.08% | -4.81% | 4.045 | +14.76% → +9.24% | 590.49 → 606.80 |
| serial | _query | 2 | 5.147 → 4.774 | -7.04% | -7.24% | 3.929 | +31.00% → +21.52% | 528.76 → 565.85 |
| interleaved | padrão | 1 | 5.348 → 5.015 | -7.40% | -6.24% | 4.167 | +28.34% → +20.33% | 489.28 → 536.74 |
| interleaved | padrão | 2 | 5.098 → 4.748 | -6.75% | -6.87% | 4.044 | +26.07% → +17.42% | 530.59 → 569.01 |
| interleaved | _multi | 1 | 4.688 → 4.450 | -5.80% | -5.08% | 4.167 | +12.50% → +6.78% | 582.00 → 594.85 |
| interleaved | _multi | 2 | 4.663 → 4.410 | -5.84% | -5.42% | 4.044 | +15.30% → +9.06% | 579.63 → 617.68 |
| interleaved | _query | 1 | 4.860 → 4.585 | -6.22% | -5.66% | 4.167 | +16.63% → +10.03% | 553.56 → 567.57 |
| interleaved | _query | 2 | 4.776 → 4.400 | -8.07% | -7.87% | 4.044 | +18.11% → +8.81% | 567.04 → 623.60 |

## Demais cenários — Δ CPU pareado B/A

| Ritmo/perfil | Operação | Cluster B | Arquivos/bloco | Δ CPU |
|---|---|---:|---|---:|
| serial | read | 512 | 1/257 | +2.25% |
| serial | read | 512 | 2/257 | +3.63% |
| serial | write | 512 | 1/4096 | -1.33% |
| serial | read | 512 | 1/4096 | +1.95% |
| serial | write | 512 | 2/4096 | -1.01% |
| serial | read | 512 | 2/4096 | +1.39% |
| serial | write | 4096 | 1/257 | -0.77% |
| serial | read | 4096 | 1/257 | +0.87% |
| serial | write | 4096 | 2/257 | +0.15% |
| serial | read | 4096 | 2/257 | +2.04% |
| serial | write | 4096 | 1/4096 | -0.68% |
| serial | read | 4096 | 1/4096 | +1.01% |
| serial | write | 4096 | 2/4096 | +1.95% |
| serial | read | 4096 | 2/4096 | +0.57% |
| serial_multi | read | 512 | 1/257 | +0.14% |
| serial_multi | read | 512 | 2/257 | -0.64% |
| serial_multi | write | 512 | 1/4096 | +0.34% |
| serial_multi | read | 512 | 1/4096 | -0.14% |
| serial_multi | write | 512 | 2/4096 | +0.45% |
| serial_multi | read | 512 | 2/4096 | +0.21% |
| serial_multi | write | 4096 | 1/257 | +1.19% |
| serial_multi | read | 4096 | 1/257 | -0.54% |
| serial_multi | write | 4096 | 2/257 | +1.17% |
| serial_multi | read | 4096 | 2/257 | -1.82% |
| serial_multi | write | 4096 | 1/4096 | -1.86% |
| serial_multi | read | 4096 | 1/4096 | -0.04% |
| serial_multi | write | 4096 | 2/4096 | +0.19% |
| serial_multi | read | 4096 | 2/4096 | -0.84% |
| serial_query | read | 512 | 1/257 | -2.04% |
| serial_query | read | 512 | 2/257 | -1.73% |
| serial_query | write | 512 | 1/4096 | -0.46% |
| serial_query | read | 512 | 1/4096 | +0.07% |
| serial_query | write | 512 | 2/4096 | -0.55% |
| serial_query | read | 512 | 2/4096 | +1.23% |
| serial_query | write | 4096 | 1/257 | +1.72% |
| serial_query | read | 4096 | 1/257 | -1.80% |
| serial_query | write | 4096 | 2/257 | +0.63% |
| serial_query | read | 4096 | 2/257 | -1.83% |
| serial_query | write | 4096 | 1/4096 | +0.53% |
| serial_query | read | 4096 | 1/4096 | -0.23% |
| serial_query | write | 4096 | 2/4096 | -0.36% |
| serial_query | read | 4096 | 2/4096 | -0.74% |
| interleaved | read | 512 | 1/257 | +3.40% |
| interleaved | read | 512 | 2/257 | +4.01% |
| interleaved | write | 512 | 1/4096 | -3.49% |
| interleaved | read | 512 | 1/4096 | +1.30% |
| interleaved | write | 512 | 2/4096 | -4.16% |
| interleaved | read | 512 | 2/4096 | +2.28% |
| interleaved | write | 4096 | 1/257 | -4.11% |
| interleaved | read | 4096 | 1/257 | +3.18% |
| interleaved | write | 4096 | 2/257 | -2.32% |
| interleaved | read | 4096 | 2/257 | +2.74% |
| interleaved | write | 4096 | 1/4096 | -2.45% |
| interleaved | read | 4096 | 1/4096 | +1.86% |
| interleaved | write | 4096 | 2/4096 | -3.10% |
| interleaved | read | 4096 | 2/4096 | +2.01% |
| interleaved_multi | read | 512 | 1/257 | -2.36% |
| interleaved_multi | read | 512 | 2/257 | -3.76% |
| interleaved_multi | write | 512 | 1/4096 | +0.89% |
| interleaved_multi | read | 512 | 1/4096 | +1.97% |
| interleaved_multi | write | 512 | 2/4096 | -0.86% |
| interleaved_multi | read | 512 | 2/4096 | +0.92% |
| interleaved_multi | write | 4096 | 1/257 | +0.27% |
| interleaved_multi | read | 4096 | 1/257 | -0.97% |
| interleaved_multi | write | 4096 | 2/257 | -1.14% |
| interleaved_multi | read | 4096 | 2/257 | -2.95% |
| interleaved_multi | write | 4096 | 1/4096 | +0.45% |
| interleaved_multi | read | 4096 | 1/4096 | -0.49% |
| interleaved_multi | write | 4096 | 2/4096 | -0.33% |
| interleaved_multi | read | 4096 | 2/4096 | -0.92% |
| interleaved_query | read | 512 | 1/257 | -2.04% |
| interleaved_query | read | 512 | 2/257 | -5.05% |
| interleaved_query | write | 512 | 1/4096 | -0.53% |
| interleaved_query | read | 512 | 1/4096 | -0.32% |
| interleaved_query | write | 512 | 2/4096 | -0.55% |
| interleaved_query | read | 512 | 2/4096 | -1.77% |
| interleaved_query | write | 4096 | 1/257 | +0.25% |
| interleaved_query | read | 4096 | 1/257 | -3.43% |
| interleaved_query | write | 4096 | 2/257 | -1.12% |
| interleaved_query | read | 4096 | 2/257 | -4.07% |
| interleaved_query | write | 4096 | 1/4096 | -0.52% |
| interleaved_query | read | 4096 | 1/4096 | -0.05% |
| interleaved_query | write | 4096 | 2/4096 | -0.14% |
| interleaved_query | read | 4096 | 2/4096 | -0.01% |

## I/O

Os 192 pares de fases A/B preservaram exatamente leituras, escritas, metadados por FAT/diretório, polls, sessões e blocos multibloco nas duas latências. Portanto o throughput virtual também é igual. Conteúdo integral, FATs iguais e ordem EOC/vínculo/diretório foram auditados.

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

| ABI | RAM A → B (bytes) | Código A → B (bytes) |
|---|---:|---:|
| x64 | 4872 → 4872 | 45076 → 45327 |
| x86 | 4760 → 4760 | 34682 → 34944 |

Matriz PIC24 de 48 configurações: RAM estática igual em todos os pares; pilha interna máxima **378 bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.

Passaram 28 testes de alocação/cache, incluindo nove novos cenários dirigidos do par coletado em cada perfil: timestamps e estouro de cacheTimer, bits altos/vizinhos, retainCount, FSInfo, reparo de espelho limpo, primária limpa, cada FAT em transferência, entrada ocupada, reserva de outro escritor e dados em transferência independentes. Os testes existentes cobrem volumes quase cheios, três escritores, fsync imediato, imutabilidade do buffer e cortes de energia/recuperação/retry:

- requested_recovery_mirror_cache: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_multi: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_multi: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- requested_recovery_mirror_cache_query: Requested batch recovery: 82 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.
- small_collection_mirror_cache_query: Requested batch recovery: 76 write cuts, 58 recovery cuts, 8 retry scenarios; immutable I/O, FAT mirrors, root/nested data and orphan cleanup passed.

Evidências congeladas: `tests/build/small-write-cpu2/final`. As alternativas aritmética e combinada ficaram somente nos snapshots experimentais.
