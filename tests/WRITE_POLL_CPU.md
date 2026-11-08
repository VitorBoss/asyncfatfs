# CPU da escrita: incorporação das verificações do poll

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

Somente no multibloco sem consulta, afatfs_releaseIdleReservation e afatfs_allocationWriteInProgress usam AFATFS_INLINE. Os builds padrão e com consulta preservam as decisões de incorporação anteriores. As verificações passam para o chamador sem remover testes de durabilidade. Relógio do cache, reserva única, varredura de atividade e decisões de avanço/flush mantêm a ordem e os efeitos anteriores. A busca de grupos multibloco permanece em uma passagem.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.0613 | -0.11% | -64.96% |
| default | 257 | 2 | 6.9399 | +0.48% | -70.24% |
| default | 4096 | 1 | 5.2476 | -0.02% | -52.57% |
| default | 4096 | 2 | 5.3375 | -0.21% | -57.23% |
| after_no_query | 257 | 1 | 5.6822 | +1.23% | -54.64% |
| after_no_query | 257 | 2 | 6.4907 | +1.91% | -59.22% |
| after_no_query | 4096 | 1 | 5.0163 | +2.75% | -45.84% |
| after_no_query | 4096 | 2 | 5.1383 | +1.53% | -51.36% |
| after | 257 | 1 | 5.7363 | -0.41% | -56.11% |
| after | 257 | 2 | 6.5439 | -0.73% | -60.53% |
| after | 4096 | 1 | 5.1065 | +0.56% | -48.47% |
| after | 4096 | 2 | 5.1582 | +0.20% | -51.95% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 380 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-poll-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 33680 bytes; multibloco com consulta 34994 → 34994 bytes. RAM estática preservada em todas as 48 variantes PIC24.
## Comparação adicional com upstream nativo

Perfil beneficiado: multibloco sem consulta. Upstream nativo usa oito páginas; o equalizado usa quatro. A biblioteca atual usa quatro. Percentual positivo abaixo indica CPU adicional frente ao upstream nativo.

| Bloco | Escritores | CPU adicional ao upstream nativo | W atual / upstream equalizado | Polls atual / upstream equalizado |
|---|---:|---:|---:|---:|
| 257 | 1 | +46.49% | 6197 / 4806 | 10330 / 4840 |
| 257 | 2 | +69.29% | 12422 / 12338 | 20678 / 16550 |
| 4096 | 1 | +43.64% | 6197 / 4128 | 6505 / 4166 |
| 4096 | 2 | +52.54% | 12420 / 8318 | 13096 / 8450 |

## Seleção final e limites

No padrão e no perfil com consulta, poll, fwrite, fread e as duas rotinas de controle conferidas geram os mesmos bytes do baseline. As variações de tempo desses perfis não demonstram efeito desta alteração. O perfil sem consulta mantém o código anterior da busca de grupos em uma passagem.

Código x64 do perfil sem consulta: 35084 → 35093 bytes. A redução de CPU vem do processamento; nenhum setor ou ACK foi suprimido. A diferença restante para o upstream não foi eliminada nem atribuída integralmente à segunda FAT.

Evidências selecionadas: `selected/lib/`, `selected-matrix/report.json`, `selected-functional/`, `selected-docs.json` e `selected-code-equivalence.json`, sob `tests/build/write-poll-cpu/`. Objetos, trials e relatório JSON final de CPU estão em `timing/`. `after/lib/`, `final-matrix/` e `exploratory-report.json` representam o experimento anterior à seleção por perfil.

A hora FAT foi fixada em 12:00:00 somente nos snapshots sob COMPARISON_CPU_ONLY. A biblioteca entregue, a configuração e os timestamps normais não foram modificados por esse controle. A matriz PIC24 e os testes funcionais usam as fontes normais.
