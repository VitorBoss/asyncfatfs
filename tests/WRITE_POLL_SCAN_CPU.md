# CPU da escrita: evitar repetir uma consulta de agrupamento negativa

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

Depois de avançar as operações dos arquivos, um resultado falso na consulta de agrupamento termina o poll. Não existe operação entre essa consulta e a consulta final equivalente. Quando o resultado é verdadeiro, a passagem extra e a reavaliação antes do flush permanecem. Inicialização, relógio do cache, reserva única, consulta multibloco e barreiras de persistência mantêm os caminhos anteriores.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 5.9365 | +11.01% | -62.66% |
| default | 257 | 2 | 6.8199 | +13.39% | -67.21% |
| default | 4096 | 1 | 5.1297 | +11.38% | -50.43% |
| default | 4096 | 2 | 5.2606 | +10.93% | -54.28% |
| after_no_query | 257 | 1 | 5.5602 | +1.85% | -52.35% |
| after_no_query | 257 | 2 | 6.4008 | +1.50% | -56.93% |
| after_no_query | 4096 | 1 | 4.9091 | +1.26% | -43.96% |
| after_no_query | 4096 | 2 | 5.0383 | +0.86% | -47.77% |
| after | 257 | 1 | 5.5922 | +2.01% | -53.23% |
| after | 257 | 2 | 6.3931 | +1.39% | -56.75% |
| after | 4096 | 1 | 4.9840 | +1.07% | -46.16% |
| after | 4096 | 2 | 5.0833 | +1.36% | -49.09% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 380 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-poll-scan/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 33680 bytes; multibloco com consulta 34994 → 34994 bytes. RAM estática preservada em todas as 48 variantes PIC24.
## Referências de CPU e upstream nativo

A tabela abaixo mostra as medianas de ciclos/byte com um e dois escritores/257. Upstream nativo usa oito páginas; equalizado usa quatro. A biblioteca atual usa quatro.

| Perfil | Escritores | Antes | Depois | Upstream 4 páginas | Upstream 8 páginas |
|---|---:|---:|---:|---:|---:|
| Padrão | 1 | 6.6692 | 5.9365 | 3.6496 | 3.8617 |
| Padrão | 2 | 7.8741 | 6.8199 | 4.0787 | 3.8121 |
| Multibloco | 1 | 5.6648 | 5.5602 | 3.6496 | 3.8617 |
| Multibloco | 2 | 6.4808 | 6.4008 | 4.0787 | 3.8121 |
| Multibloco + consulta | 1 | 5.7010 | 5.5922 | 3.6496 | 3.8617 |
| Multibloco + consulta | 2 | 6.4895 | 6.3931 | 4.0787 | 3.8121 |

O ganho pareado da tabela principal é a mediana das razões em cada rodada; não é a razão das medianas acima. A diferença de CPU para o upstream continua grande e não foi atribuída integralmente à segunda FAT. Esta mudança evita processamento repetido, não elimina setores gravados ou ACKs.

## Controle e evidências

A hora FAT foi fixada em 12:00:00 apenas nos snapshots COMPARISON_CPU_ONLY do benchmark. As fontes normais e os timestamps da biblioteca entregue não foram alterados por esse controle. A matriz de pilha e as regressões funcionais usam as fontes normais.

Fontes normais: `before/lib` e `after/lib`; pilha/RAM: `final-matrix/report.json`; regressões: `functional/`; documentação: `docs.json`, sob `tests/build/write-poll-scan/`. Os objetos, trials, snapshots com hora controlada e o relatório JSON final de CPU estão em `timing/`. Os hashes e as 48 cadeias de pilha estão no JSON final.
