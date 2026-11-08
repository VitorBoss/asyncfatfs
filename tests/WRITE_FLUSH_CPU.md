# CPU da escrita: filtragem antecipada no flush

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

Na seleção do flush, páginas limpas, bloqueadas ou com timestamp igual/maior que a candidata atual são ignoradas antes de consultar os arquivos. A consulta pura de adiamento de diretório só percorre os arquivos para candidatas elegíveis. A política e a ordem de seleção permanecem iguais.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.2542 | -2.06% | -69.15% |
| default | 257 | 2 | 7.0466 | -0.37% | -70.82% |
| default | 4096 | 1 | 5.3131 | -0.60% | -54.31% |
| default | 4096 | 2 | 5.5630 | -1.10% | -62.48% |
| after_no_query | 257 | 1 | 6.0190 | +2.22% | -62.79% |
| after_no_query | 257 | 2 | 6.8502 | +3.37% | -66.06% |
| after_no_query | 4096 | 1 | 5.2570 | -0.57% | -52.68% |
| after_no_query | 4096 | 2 | 5.4134 | +0.36% | -58.11% |
| after | 257 | 1 | 5.9975 | +7.10% | -62.21% |
| after | 257 | 2 | 6.8170 | +18.29% | -65.25% |
| after | 4096 | 1 | 5.2402 | +6.12% | -52.19% |
| after | 4096 | 2 | 5.3288 | +9.29% | -55.63% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 392 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-flush-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 33715 bytes; multibloco com consulta 35070 → 35105 bytes. RAM estática preservada em todas as 48 variantes PIC24.