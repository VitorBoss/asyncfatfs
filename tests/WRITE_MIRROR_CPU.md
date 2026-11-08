# CPU da escrita: recuperar a imagem da segunda FAT somente em faltas de cache

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem biblioteca, backend simulado, polling e preparação/verificação dos dados no harness. A auditoria completa do volume fica fora da janela de escrita. Não são medições em PIC24 ou cartão físico.

A recuperação de uma imagem limpa da segunda FAT ocorre somente quando falta a página primária. A busca é feita após selecionar a vítima, mas antes de reinicializar seu descritor, preservando a recuperação quando a própria página do espelho é reutilizada. Acertos de cache não precisam dessa varredura. O alocador é incorporado ao chamador para evitar outra chamada e permitir reaproveitar cálculos de acesso. Nenhum buffer, estado estático, reserva ou regra de confirmação foi acrescentado ou removido.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 5.8964 | +1.50% | -59.97% |
| default | 257 | 2 | 6.6888 | +2.26% | -64.22% |
| default | 4096 | 1 | 5.0571 | +2.17% | -47.60% |
| default | 4096 | 2 | 5.1491 | +1.92% | -51.71% |
| after_no_query | 257 | 1 | 5.5024 | +2.36% | -49.28% |
| after_no_query | 257 | 2 | 6.2609 | +2.38% | -53.72% |
| after_no_query | 4096 | 1 | 4.9147 | +1.51% | -43.44% |
| after_no_query | 4096 | 2 | 4.9811 | +1.19% | -46.76% |
| after | 257 | 1 | 5.5008 | +2.02% | -49.23% |
| after | 257 | 2 | 6.2714 | +3.26% | -53.97% |
| after | 4096 | 1 | 4.8790 | +2.67% | -42.40% |
| after | 4096 | 2 | 4.9915 | +2.05% | -47.07% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 382 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-mirror-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 34151 bytes; multibloco com consulta 34994 → 35461 bytes. RAM estática preservada em todas as 48 variantes PIC24.
## Upstream nativo e I/O de escrita

Atual: multibloco com consulta, quatro páginas de cache. Upstream equalizado: quatro; upstream nativo: oito. A tabela mostra CPU adicional ao upstream nativo, não uma redução.

| Bloco | Escritores | CPU adicional ao upstream nativo | Setores gravados atual / upstream equalizado | Polls atual / upstream equalizado |
|---|---:|---:|---:|---:|
| 257 | 1 | +41.40% | 6197 / 4806 | 10330 / 4840 |
| 257 | 2 | +64.27% | 12422 / 12338 | 20678 / 16550 |
| 4096 | 1 | +40.61% | 6197 / 4128 | 6505 / 4166 |
| 4096 | 2 | +47.58% | 12420 / 8318 | 13096 / 8450 |

A diferença de CPU não foi eliminada nem atribuída integralmente à segunda FAT. Os ganhos desta mudança vêm do processamento; a quantidade de operações permanece igual ao baseline.

## Controles de leitura

O I/O e as sessões multibloco de leitura foram preservados. Os tempos de CPU de leitura tiveram ganhos e regressões; não houve melhoria em todas as operações da biblioteca. Valores positivos são redução de CPU frente ao baseline.

| Perfil | Bloco | Leitores | Redução de CPU |
|---|---:|---:|---:|
| Padrão | 257 | 1 | -0.92% |
| Padrão | 257 | 2 | -0.93% |
| Padrão | 4096 | 1 | -1.72% |
| Padrão | 4096 | 2 | -2.61% |
| Multibloco | 257 | 1 | +0.93% |
| Multibloco | 257 | 2 | -1.49% |
| Multibloco | 4096 | 1 | +2.92% |
| Multibloco | 4096 | 2 | -0.59% |
| Multibloco + consulta | 257 | 1 | -0.42% |
| Multibloco + consulta | 257 | 2 | -0.43% |
| Multibloco + consulta | 4096 | 1 | -1.73% |
| Multibloco + consulta | 4096 | 2 | -1.27% |

A maior regressão de CPU observada nesses controles foi de 2,61%, no padrão com dois leitores/4096. A alteração foi mantida pelo ganho em todos os cenários de escrita solicitados; esse resultado não deve ser descrito como ganho universal de CPU.

## Testes e evidências selecionadas

Além dos 11 cenários gerais, passaram as 10 verificações de alocação: ordenação, três arquivos, pedidos sobrepostos, volume quase cheio, hints de metadados, cache de espelho, confirmação da reserva, alocações pendentes e cache dos leitores. O teste de espelho cobre a recuperação no mesmo slot e em outro slot, primária mais nova com retenção, e rejeição de espelho sujo, bloqueado, em voo ou sem ACK confirmado. Os chamadores internos no teste de cache foram atualizados para o novo parâmetro opcional; a API pública não mudou.

Fontes normais selecionadas: `selected/lib`; matriz: `selected-matrix/report.json`; regressões: `selected-functional/` e `selected-allocation-checks/`; Doxygen: `selected-docs.json`, sob `tests/build/write-mirror-cpu/`. Os objetos, trials e o JSON final de CPU estão em `timing/`. O experimento inicial, sem incorporação do alocador, permanece em `after/lib/`, `final-matrix/` e `initial-report.json`.

A hora FAT foi fixada em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY do benchmark. A biblioteca entregue e seus timestamps normais não foram alterados por esse controle. A matriz e as regressões usam fontes normais.

Os tamanhos de código x64 são a soma das seções de código dos objetos da biblioteca, não o tamanho final do firmware PIC24. Nenhuma RAM estática ou buffer foi acrescentado.
