# CPU da escrita: avanço dentro do setor

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

A escrita que termina dentro do setor bloqueado avança diretamente o cursor. Não passa por fseekInternal; fronteiras de setor e cluster mantêm o caminho anterior.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.2788 | +0.49% | -67.27% |
| default | 257 | 2 | 7.0436 | +2.09% | -70.56% |
| default | 4096 | 1 | 5.2932 | +0.13% | -50.91% |
| default | 4096 | 2 | 5.4980 | -1.14% | -59.13% |
| after_no_query | 257 | 1 | 6.1595 | +0.31% | -64.09% |
| after_no_query | 257 | 2 | 7.1163 | +1.37% | -72.32% |
| after_no_query | 4096 | 1 | 5.2758 | +0.13% | -50.42% |
| after_no_query | 4096 | 2 | 5.4399 | -0.45% | -57.45% |
| after | 257 | 1 | 6.1642 | +0.46% | -64.22% |
| after | 257 | 2 | 7.1512 | +1.12% | -73.16% |
| after | 4096 | 1 | 5.3586 | -0.66% | -52.78% |
| after | 4096 | 2 | 5.4620 | +0.47% | -58.08% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 398 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-cursor-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.
