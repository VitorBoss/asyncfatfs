# Agrupamento de alocações pendentes entre escritores

Data: 16/11/2026. FAT32 8.3, clusters/setores de 512 bytes, API original, cache de quatro setores e até três handles; sem LFN/exFAT/diário.

O agrupamento está implementado em `lib/asyncfatfs.c`, na via mínima que já utiliza a reserva antecipada única. Ele reúne pedidos de alocação que já existem, sem criar um lote especulativo. O ganho observado com dois escritores e blocos de 257 bytes é pequeno; o pipeline anterior já amortizava grande parte dos EOCs. A leitura remontada conserva seu I/O.

## Comportamento e persistência

Uma barreira de alocação que termina de drenar o cache confirma os EOCs/vínculos de todos os escritores que já estavam esperando. A função que avança esses estados não modifica o cache. Assim, todas as confirmações referentes à imagem antiga são registradas nas fases existentes antes de preparar uma imagem nova.

Um escritor com destino já durável pode aguardar o EOC de outro pedido já pendente. Uma segunda passagem limitada do escalonador permite que vínculos prontos sejam preparados antes da descarga de fundo. Não há espera pela chegada de um pedido futuro; sem parceiro, a via anterior continua. Durante uma gravação ainda em andamento no backend, permanece a passagem simples do escalonador, evitando consultas repetidas sem possibilidade de completar uma barreira.

As propriedades preservadas são:

- Cada operação continua solicitando um cluster; a reserva antecipada continua sendo uma única entrada global, identificada no campo existente.
- Um vínculo somente é preparado depois de seu destino estar durável nas duas FATs. O agrupamento não combina EOCs novos e seus vínculos antes dessa confirmação.
- Os vínculos aguardam os ACKs das duas cópias antes de liberar a alocação/publicação posterior. Escrita em trânsito, página novamente suja ou ACK com falha impedem a passagem da barreira.
- Nenhum bit da geração de mídia foi reutilizado. Callbacks, detecção de mídia, sync/close e liberação da reserva conservam seus mecanismos anteriores.
- Não foram acrescentados registros de operação, campos, buffers ou alocações de heap. Os estados existentes distinguem EOC pendente, vínculo pronto e vínculo pendente.

O teste de ordenação verifica a imagem durável anterior a cada write: os destinos dos vínculos precisam já estar alocados. Também verifica a cadeia após cada gravação e se o tamanho publicado cabe na cadeia durável. Isso valida a ordenação usada pelo agrupamento; não estabelece uma garantia nova de atomicidade contra qualquer rasgo dentro de uma entrada FAT.

## Como foi comparado

Fontes anteriores e posteriores estão preservadas em `tests/build/fat32-83-pending-batch-20261116/{before,after}/lib`. O [JSON](FAT32_83_PENDING_ALLOCATIONS.json) contém hashes de todos esses arquivos, contagens, taxas e medições de memória. A base anterior é o checkout imediatamente antes desta implementação, incluindo as otimizações prévias de cache/reserva; não é upstream/master.

Cada arquivo tem 1 MiB. Foram executadas sete repetições por cenário, com um ou dois escritores, blocos de 257/4096 bytes, clusters de 512/4096 bytes e dois perfis de latência. São 448 volumes e 896 fases nas quatro combinações de versão/escalonamento. Cada volume é auditado diretamente e remontado para conferir tamanho e conteúdo; as duas FATs devem terminar iguais, sem clusters excedentes. O fechamento e a descarga final entram no tempo de escrita; desmontar depois não pode acrescentar gravações.

O controle `serial` conserva o harness anterior: alterna os arquivos, mas termina cada bloco antes de tentar o próximo escritor. O modo `interleaved` tenta atender todos os escritores antes de aguardar progresso, permitindo que pedidos fiquem simultaneamente pendentes. A mudança no harness vale igualmente para as versões anterior e posterior; não se deve atribuir ao agrupamento uma diferença entre os dois escalonamentos.

O throughput modelado usa o perfil 1: leitura 200 µs, gravação isolada 800 µs, bloco em múltiplos 400 µs, início/fim 100 µs e poll de 10 µs. O perfil 0 usa um poll por transferência e permite comparar o custo de execução no computador. Nenhuma taxa é uma medição em cartão físico ou firmware PIC24.

## Resultado com blocos de 257 bytes

R/W contam setores de 512 bytes durante a escrita, incluindo metadados e fechamento. Throughput é agregado para todos os arquivos. As contagens foram idênticas nas sete repetições.

| Escritores/escalonamento | R antes/depois | W antes/depois | Metadados W antes/depois | MiB/s modelados antes/depois |
| --- | --- | --- | --- | --- |
| 1, ambos os controles | 50 / 50 | 6.197 / 6.197 | 4.149 / 4.149 | 0,199663 / 0,199663 |
| 2, blocos concluídos em sequência | 98 / 98 | 12.422 / 12.422 | 8.326 / 8.326 | 0,199227 / 0,199227 |
| 2, pedidos sobrepostos | 162 / 130 | 12.422 / 12.356 | 8.326 / 8.260 | 0,199895 / 0,201085 |

No cenário que permite agrupar pedidos, são **19,75% menos leituras**, **0,53% menos gravações totais**, **0,79% menos gravações de metadados** e **0,60% mais throughput modelado**. Cada FAT passa de 4.161 para 4.128 gravações: 33 a menos por cópia. O diretório continua com quatro gravações. As 4.096 gravações de dados permanecem; não houve supressão do espelho nem transferência de trabalho para depois do fechamento.

O efeito é modesto: as duas FATs ainda exigem 8.256 gravações para 4.096 clusters de dados, aproximadamente duas gravações por cluster no conjunto. A reserva única do pipeline anterior já ocupava as oportunidades que um lote maior exploraria. Agrupar pedidos efetivos, sem aumentar a especulação, não reproduz a redução teórica de 75% do lote antecipado de oito clusters.

A leitura após remontagem permanece em 2.066 setores para um arquivo e 4.130 para dois, sem gravações. No perfil de latência, as taxas continuam em 2,202837 e 2,203808 MiB/s, respectivamente.

### Custo de execução e controles adicionais

Na mediana de sete repetições do perfil 0, a escrita com dois pedidos sobrepostos e blocos de 257 bytes passa de **162,084 para 156,215 MiB/s no computador**, redução de **3,62%**. No perfil 1, a taxa de execução do simulador passa de 16,048 para 13,813 MiB/s, redução de 13,93%; esse perfil executa quase um milhão de polls para representar aproximadamente dez segundos de latência, sem esperar dez segundos reais. Esse custo de CPU não está incorporado às latências fixas do throughput modelado. As taxas do computador incluem geração/verificação de dados e instrumentação do harness, não somente a biblioteca.

Portanto, o ganho confirmado é em I/O e no modelo de dispositivo, com uma contrapartida de CPU. Não foi demonstrado ganho de tempo total em hardware; a redução de 0,60% no modelo pode ser absorvida pelo custo de execução em um alvo mais lento.

O controle com blocos de **4.096 bytes e dois pedidos sobrepostos piorou**: W sobe de 12.422 para 12.668 setores (+1,98%) e o throughput modelado cai de 0,204687 para 0,200622 MiB/s (−1,99%). O controle sequencial de 4.096 bytes mantém seu I/O e sua taxa modelada. Todos os casos com clusters de 4.096 bytes, fora do escopo do agrupamento, mantêm suas contagens antes/depois. A implementação não oferece ganho universal conforme o tamanho dos pedidos e seu escalonamento.

## Memória

| ABI | RAM estática antes/depois | Estado global antes/depois | Handle antes/depois |
| --- | --- | --- | --- |
| PIC24, enums normais | 2.494 / 2.494 B | 2.488 / 2.488 B | 76 / 76 B |
| PIC24, enums curtos | 2.474 / 2.474 B | 2.468 / 2.468 B | 74 / 74 B |
| x86 | 2.612 / 2.612 B | 2.604 / 2.604 B | 96 / 96 B |
| x64 | 2.720 / 2.720 B | 2.704 / 2.704 B | 120 / 120 B |

A RAM estática inclui os globais graváveis da biblioteca, não os buffers do backend/harness nem a pilha da aplicação. O cache e os descritores mantêm seus tamanhos e alinhamentos.

O código PIC24 gerado foi inspecionado em ambos os modos de enum: nenhum prólogo de função existente aumentou. `afatfs_poll` e `afatfs_fileOperationsPoll` mantêm frame local zero; `afatfs_appendRegularFreeClusterContinue` mantém 26 bytes, incluindo os registradores salvos. As cinco consultas novas usam 0, 6, 8, 6 e 8 bytes. A maior combinação nova é a consulta de grupo de 8 bytes chamando a consulta de parceiro de 6 bytes, com os respectivos endereços de retorno; os novos caminhos são mais rasos que os caminhos existentes de FAT/cache. A consulta de gravação em andamento fica fora de linha para evitar que o compilador preserve um endereço adicional no frame do poll, especialmente com enums curtos. As duas passagens do escalonador são sequenciais, sem recursão. Isso não mede a pilha completa do backend, interrupções ou callbacks da aplicação.

Compiladores: MSVC `/O2` para os objetos/benchmarks x86/x64; XC16 2.10 para PIC24FJ256GA702 com `-Os`, sujeito às opções desabilitadas pela licença disponível. A comparação usa exatamente o mesmo compilador/opções nas duas versões.

## Validação e reprodução

O teste novo [fat32_pending_allocations.c](fat32_pending_allocations.c) prepara dois pedidos com EOCs duráveis, confirma que ambos chegam à espera dos vínculos e injeta repetidamente falhas de ACK na primária e na secundária. Nenhum escritor pode ultrapassar a barreira enquanto a cópia falha; depois da recuperação, ambos são fechados e remontados com conteúdo e tamanho conferidos.

A suíte de alocação inclui o teste novo, auditoria por preimagem, idle, dois/três pedidos sobrepostos, três escritores no escalonamento anterior, volume quase cheio, hints de metadados, reconstrução do espelho e liberação da reserva sob ACKs com falha. Os testes gerais de FAT16/FAT32 e das configurações opcionais também passaram, incluindo fechamento, falhas de backend, remoção de mídia e remontagem. As verificações de layout cobrem 24 combinações de configuração/ABI/enum; as sondas antes/depois desta mudança medem especificamente a configuração mínima nos quatro ABIs acima.

```powershell
# O diretório before/lib deve conter o snapshot anterior preservado.
tests/run_pending_allocation_comparison.ps1 -Repetitions 7
python tests/pending_allocation_report.py --directory tests/build/fat32-83-pending-batch-20261116 --output tests/FAT32_83_PENDING_ALLOCATIONS.json
tests/run_fat32_allocation_checks.ps1 -OutputDirectory tests/build/fat32-83-pending-batch-20261116/checks-release
tests/run_exfat_tests.ps1
tests/run_layout_tests.ps1
```

O gerador de resultados exige sete repetições com contagens determinísticas, igualdade de memória e ausência de aumento nos prólogos PIC24 compartilhados. Um ensaio em hardware continua necessário para converter o pequeno ganho de I/O em uma conclusão sobre throughput físico.
