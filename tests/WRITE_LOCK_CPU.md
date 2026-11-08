# CPU da escrita: incorporação da aquisição do setor

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

A rotina afatfs_fileLockCursorSectorForWrite usa AFATFS_INLINE. A obtenção e validação da página são incorporadas a fwrite, preservando leituras para escritas parciais, alocação, locks e regras de persistência.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.0701 | +3.37% | -64.06% |
| default | 257 | 2 | 7.0422 | -0.19% | -70.42% |
| default | 4096 | 1 | 5.2581 | -0.11% | -51.93% |
| default | 4096 | 2 | 5.4076 | +1.46% | -57.02% |
| after_no_query | 257 | 1 | 6.0723 | +1.34% | -64.12% |
| after_no_query | 257 | 2 | 7.0658 | +1.11% | -70.99% |
| after_no_query | 4096 | 1 | 5.2338 | +0.53% | -51.23% |
| after_no_query | 4096 | 2 | 5.4174 | +0.00% | -57.31% |
| after | 257 | 1 | 6.1203 | -0.21% | -65.42% |
| after | 257 | 2 | 7.1000 | +0.45% | -71.82% |
| after | 4096 | 1 | 5.2990 | +0.56% | -53.12% |
| after | 4096 | 2 | 5.4320 | +1.39% | -57.73% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 392 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-lock-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33268 → 33680 bytes; multibloco com consulta 34542 → 35070 bytes. RAM estática preservada em todas as 48 variantes PIC24.