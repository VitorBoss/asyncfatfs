# CPU de escrita com oito setores: detecção de grupos em uma passagem

FAT32 8.3, clusters de 512 bytes, MSVC x64 /O2. 21 repetições intercaladas, sem compilação concorrente na medição final. Arquivos de 1 MiB; backend simulado. Os ciclos incluem biblioteca, backend simulado, polling e preparação/verificação dos dados no harness. A auditoria completa do volume ocorre fora da janela de escrita. Não são medições em PIC24 ou cartão físico.

Com oito ou mais setores, o padrão usa a busca em uma passagem por dois escritores aguardando alocação. Testa a geometria uma vez e termina no segundo escritor elegível. Os perfis multibloco já usavam essa busca e preservam os bytes de cinco rotinas conferidas. Nenhuma reserva, flag persistente ou regra de ACK/flush foi alterada.

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Cache equalizado: oito páginas; nativo: oito. A versão atual mantém as duas FATs; upstream deixa a segunda cópia desatualizada neste workload.

| Perfil | Bloco | Escritores | Ciclos/byte | Redução CPU anterior | Redução CPU upstream 8 páginas |
|---|---:|---:|---:|---:|---:|
| default | 257 | 1 | 7.0720 | +1.54% | -75.35% |
| default | 257 | 2 | 7.4257 | +1.37% | -91.68% |
| default | 4096 | 1 | 6.0753 | +1.48% | -71.70% |
| default | 4096 | 2 | 6.3553 | +1.81% | -85.45% |
| after_no_query | 257 | 1 | 6.2283 | +0.24% | -54.43% |
| after_no_query | 257 | 2 | 6.5659 | -0.48% | -69.48% |
| after_no_query | 4096 | 1 | 5.5563 | -0.85% | -57.03% |
| after_no_query | 4096 | 2 | 5.7276 | -0.66% | -67.13% |
| after | 257 | 1 | 6.4842 | -0.19% | -60.77% |
| after | 257 | 2 | 6.7635 | +0.15% | -74.58% |
| after | 4096 | 1 | 5.5729 | +1.04% | -57.50% |
| after | 4096 | 2 | 5.7799 | +0.14% | -68.66% |

Percentuais positivos indicam menos CPU. Diferenças pequenas podem refletir ruído do host. Comparação anterior: mediana de razões pareadas. Upstream: razão das medianas.

RAM estática x64: 4872 bytes antes/depois. As 48 variantes PIC24 passaram, com seções estáticas iguais e pico interno de 378 bytes (incluindo retornos; excluindo backend, callbacks da aplicação e compilador/libc).

I/O, metadados, sessões, paradas e polling iguais ao baseline em todos os casos. As duas FATs passaram na auditoria. Os 11 cenários funcionais e a verificação Doxygen passaram. Nenhuma mudança nas regras de ACK, reserva, journal ou publicação.

Evidências: `tests/build/cache8-group-cpu/`, incluindo snapshots before/after, trials, matrix/report.json, functional/ e final-docs.json.


Código x64: padrão 36301 → 36190 bytes; multibloco com consulta 37370 → 37370 bytes. RAM estática preservada em todas as 48 variantes PIC24.
## Limites e seleção final

As cinco rotinas conferidas nos perfis multibloco são idênticas ao baseline; as variações de tempo desses perfis não demonstram uma alteração de código. A melhoria selecionada aplica a busca em uma passagem ao padrão com oito setores; não altera o caminho padrão de quatro setores.

A tentativa adicional de incorporar o conversor de ponteiro para índice no padrão foi descartada por não mostrar ganho consistente. selected/lib, selected-matrix, selected-functional, selected-allocation-checks e selected-docs.json são desse experimento descartado. A versão entregue coincide com normal/lib e matrix/report.json; functional/, allocation-checks/ e final-docs.json documentam sua validação. O JSON final e os trials atuais pertencem à versão entregue. initial-report.json é a primeira medição exploratória.

Passaram 11 cenários gerais e 10 verificações de alocação, incluindo ordens de publicação, três arquivos, pedidos pendentes sobrepostos, volume quase cheio, espelho no mesmo slot, ACK com falha, retenção de leitores e consumo da reserva única. As 48 comparações de seções estáticas com o baseline de oito setores foram iguais; pico interno de pilha de 378 bytes.

A hora FAT foi fixada em 12:00:00 apenas nas cópias COMPARISON_CPU_ONLY do benchmark. As fontes normais, a configuração de oito setores e os timestamps da biblioteca entregue foram preservados. Não atribuir diferenças entre rodadas históricas ao código sem comparação pareada. Os tamanhos de código x64 são somas de seções dos objetos, não tamanho final do firmware PIC24. A diferença para o upstream permanece e não foi atribuída integralmente à segunda FAT.
