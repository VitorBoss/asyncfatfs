# CPU da escrita: incorporação das verificações de invariantes

Experimento intermediário. A conferência posterior dos objetos mostrou que o MSVC já incorporava as verificações nos caminhos comuns. Os percentuais de tempo desta rodada não demonstram ganho novo de CPU no host. O ganho confirmado dessa alteração é de pilha no PIC24. A investigação continuou em `WRITE_GROUP_CPU.md`.

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

afatfs_assert usa AFATFS_INLINE: solicita a incorporação das verificações, preservando a transição para estado fatal, o erro e o comportamento de depuração. Não remove nenhuma verificação nem altera ACKs e barreiras.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.0439 | -0.09% | -65.13% |
| default | 257 | 2 | 6.9456 | -0.02% | -70.61% |
| default | 4096 | 1 | 5.2322 | +0.06% | -53.11% |
| default | 4096 | 2 | 5.3561 | -0.69% | -57.71% |
| after_no_query | 257 | 1 | 5.9058 | +4.33% | -61.36% |
| after_no_query | 257 | 2 | 6.7991 | +2.73% | -67.02% |
| after_no_query | 4096 | 1 | 5.1413 | +1.13% | -50.46% |
| after_no_query | 4096 | 2 | 5.2533 | +2.01% | -54.68% |
| after | 257 | 1 | 5.9806 | +9.82% | -63.40% |
| after | 257 | 2 | 6.7651 | +9.50% | -66.18% |
| after | 4096 | 1 | 5.1695 | +10.56% | -51.28% |
| after | 4096 | 2 | 5.2961 | +10.16% | -55.94% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 380 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/assert-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 33680 bytes; multibloco com consulta 35105 → 35105 bytes. RAM estática preservada em todas as 48 variantes PIC24.
