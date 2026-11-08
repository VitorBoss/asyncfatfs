# CPU da escrita: uma passagem para detectar alocações agrupáveis

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem polling e auditoria do payload, não são uma medição em PIC24 ou cartão físico.

A detecção de dois escritores aguardando alocação faz uma única passagem, testa a geometria uma vez e termina no segundo escritor elegível. Nenhuma fase de alocação ou barreira muda. afatfs_assert é incorporada no XC16, preservando validações e estado fatal; no MSVC os caminhos comuns já eram incorporados, portanto essa parte não demonstra ganho novo de CPU no host.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: quatro páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 4 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 6.0621 | -0.95% | -66.09% |
| default | 257 | 2 | 6.8288 | +1.58% | -67.76% |
| default | 4096 | 1 | 5.1852 | +0.46% | -51.72% |
| default | 4096 | 2 | 5.3256 | +0.04% | -56.03% |
| after_no_query | 257 | 1 | 5.7286 | +6.14% | -56.95% |
| after_no_query | 257 | 2 | 6.6336 | +1.56% | -62.96% |
| after_no_query | 4096 | 1 | 5.1165 | +0.22% | -49.71% |
| after_no_query | 4096 | 2 | 5.2084 | +0.74% | -52.59% |
| after | 257 | 1 | 5.7556 | +5.05% | -57.69% |
| after | 257 | 2 | 6.4953 | +5.22% | -59.56% |
| after | 4096 | 1 | 5.0375 | +4.22% | -47.39% |
| after | 4096 | 2 | 5.1514 | +4.15% | -50.92% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 2728 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 380 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/write-group-cpu/`, incluindo snapshots before/after, trials, final-matrix/report.json, functional/ e docs.json.


Código x64: padrão 33680 → 33569 bytes; multibloco com consulta 35105 → 34994 bytes. RAM estática preservada em todas as 48 variantes PIC24.
## I/O de escrita frente ao upstream equalizado

| Bloco | Escritores | Setores gravados atual / upstream | Metadados gravados atual / upstream | Polls atual / upstream |
|---|---:|---:|---:|---:|
| 257 | 1 | 6197 / 4806 | 4149 / 2758 | 10330 / 4840 |
| 257 | 2 | 12422 / 12338 | 8326 / 8242 | 20678 / 16550 |
| 4096 | 1 | 6197 / 4128 | 4149 / 2080 | 6505 / 4166 |
| 4096 | 2 | 12420 / 8318 | 8324 / 4222 | 13096 / 8450 |

As quantidades de operações não caíram com esta otimização: o ganho procurado é o custo de processamento das mesmas operações. As duas FATs explicam parte da diferença para o upstream, mas esta medição não isola causalmente toda a diferença de CPU. A escrita continua mais custosa que a do upstream; não foi atingida paridade.

A rotina de detecção de agrupamento caiu de 220 para 109 bytes nos três objetos MSVC x64. A incorporação de assert reduziu a pilha no XC16, mas não gerou novo ganho de CPU demonstrável no host.

A configuração padrão com um escritor/257 variou 0,95% para pior; não houve ganho em todos os cenários. A comparação inclui essa limitação.
