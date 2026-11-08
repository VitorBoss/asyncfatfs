# Investigação de alocação em lote em FAT32 8.3

Data: 16/11/2026. Setores e clusters de 512 bytes; duas FATs; cache de quatro setores; API original; LFN/exFAT/diário desabilitados.

Atualização posterior: o agrupamento conservador descrito aqui foi implementado e testado em [FAT32_83_PENDING_ALLOCATIONS.md](FAT32_83_PENDING_ALLOCATIONS.md). Este documento preserva a investigação anterior à implementação.

O lote contíguo oferece uma redução grande de I/O em princípio, mas não foi habilitado na biblioteca: preparar vários clusters antecipadamente amplia a quantidade de espaço que uma queda pode deixar sem dono. A alternativa conservadora a investigar na máquina de estados é agrupar alocações já pendentes de escritores distintos, mantendo o limite atual de reserva antecipada. Essa alternativa ainda não está implementada nem tem ganho de throughput demonstrado.

## Evidência e limites

O modelo executável [fat32_batch_model.py](fat32_batch_model.py) compara duas sequências, para lotes de 1, 2, 4, 8, 16, 32 e 64 clusters. Foram examinados 5.418 estados de interrupção: todos os 129 prefixos de setor alinhados a entradas de quatro bytes, em cada gravação de cada sequência. Resultado bruto: `tests/build/fat32-83-batch-investigation-20261116/model.json`.

O modelo usa duas imagens FAT independentes, uma cadeia existente de um cluster e um lote inteiramente contido no mesmo setor FAT que sua cauda. Verifica ciclos, vínculos para entradas livres, destinos fora da faixa e alocações sem caminho a partir do arquivo. Também verifica, antes de cada submissão, se o destino de cada vínculo já estava alocado nas duas imagens duráveis.

Isso isola a ordenação da alocação. Não simula a biblioteca, falhas do backend, cache, dados, diretório, recuperação na montagem, tempo ou rasgos dentro de uma entrada FAT. Não constitui prova de tolerância a qualquer gravação parcial de setor. Os números abaixo são contagens do protocolo ideal, não novos benchmarks da biblioteca ou de cartão SD.

Reprodução, usando Python 3:

```text
python tests/fat32_batch_model.py --output tests/build/fat32-83-batch-investigation-20261116/model.json
```

## Sequência que reduz as gravações

1. Encontrar N entradas livres contíguas e impedir que outro escritor as escolha.
2. Preparar todas como EOC no cache; gravar a primária e a secundária; aguardar ambos os ACKs.
3. Preparar os vínculos internos e o vínculo da cauda antiga para o lote; gravar ambas as cópias; aguardar ambos os ACKs.
4. Aceitar os dados e publicar somente o tamanho efetivamente persistido, mantendo as barreiras de dados/diretório atuais.

São quatro gravações de setor FAT por lote quando todas as entradas, inclusive a cauda anterior, estão na mesma página. Se a cauda estiver em outra página, há escritas adicionais. Fragmentação, fronteiras de página, pressão de cache, limpeza de sobras e vários escritores reduzem o ganho. Uma página FAT32 de 512 bytes contém 128 entradas; não é possível aplicar arbitrariamente o mesmo custo a um lote que atravesse páginas.

| Clusters por lote | Writes das duas FATs por lote | Writes FAT por cluster | Redução ideal frente a ~2 writes/cluster atuais | Máximo de clusters sem dono no modelo, por cópia |
| --- | --- | --- | --- | --- |
| 1 | 4 | 4 | não melhora | 1 |
| 2 | 4 | 2 | 0% | 2 |
| 4 | 4 | 1 | 50% | 4 |
| 8 | 4 | 0,5 | 75% | 8 |
| 16 | 4 | 0,25 | 87,5% | 16 |
| 32 | 4 | 0,125 | 93,75% | 32 |
| 64 | 4 | 0,0625 | 96,875% | 64 |

A comparação com ~2 writes/cluster usa o pipeline existente, que já sobrepõe o EOC da reserva única ao vínculo corrente. O modelo isolado de N=1 não contém essa sobreposição; portanto seus quatro writes não representam o custo atual da biblioteca.

Na medição anterior, um arquivo de 1 MiB exigiu 2.065 gravações por FAT: 4.130 nas duas cópias, para 2.048 clusters de dados. Um lote ideal de oito reduziria a parte FAT a aproximadamente 1.024 writes antes dos custos adicionais acima. Isso não autoriza calcular um throughput real a partir dessa divisão: o padrão de dados, as leituras, os multiblocos e as esperas também mudariam. A referência medida permanece [FAT32_83_FAT_WRITE_COST.md](FAT32_83_FAT_WRITE_COST.md).

## Por que o lote antecipado não atende ainda à restrição de persistência

A sequência em duas etapas não apresentou vínculos inválidos nos prefixos alinhados examinados. Porém, depois de confirmar os EOCs e antes de confirmar o vínculo da cauda, uma queda deixa até N clusters alocados e inacessíveis a partir do arquivo. São N entradas órfãs, não uma cadeia recuperável por um ponteiro conhecido.

O pipeline atual possui uma reserva global de um cluster, além das alocações em andamento dos escritores. Aumentar essa reserva para oito amplia sua exposição específica de 512 bytes para 4 KiB. A limpeza em `fsync` ou `fclose` não resolve uma queda anterior. Guardar início/quantidade apenas na RAM também não permite recuperar depois de reiniciar. Não foi presumido que a biblioteca já prometa ausência absoluta de órfãos; o problema é ampliar a janela existente sem uma recuperação equivalente.

Combinar EOCs e vínculos em uma única etapa também foi testado. Todos os tamanhos de lote produziram vínculos sem destino previamente durável e prefixos com cadeia apontando para entrada livre. Uma gravação de setor não pode ser tratada como atômica para justificar esse atalho.

Vincular uma cauda longa antes de usá-la troca os órfãos por capacidade excedente ligada ao arquivo. Isso exige limpeza persistente de sobras, tratamento de arquivo vazio, fechamento, sincronização, interrupção de mídia e montagem após queda. O leitor FAT atual segue a cadeia e pode reutilizar capacidade extra; isso, sozinho, não implementa a limpeza nem preserva a verificação atual de cadeia sem clusters excedentes após fechamento. Não é uma substituição já validada.

## RAM: o que pode e o que não pode ser reaproveitado

Não foram alterados arquivos da biblioteca nesta investigação; o acréscimo de RAM de produção é zero. O script usa memória somente no computador de teste.

`afatfsAppendFreeCluster_t` já contém `previousCluster`, `searchCluster`, a união `progress` de 32 bits, `phase` e `wrapped`. Na via FAT32 normal, há espaço conceitual em `progress` para representar um intervalo ou flags de confirmação em fases específicas. Isso é um candidato a reutilização, não uma medição de uma implementação nova: a mesma estrutura é prefixo de operações de diretório e serve a outras configurações.

A união global `initState` só existe com freefile ou exFAT; ela não oferece armazenamento gratuito na configuração mínima. Ativar o diário ou o freefile existentes acrescenta estado e não satisfaz automaticamente o orçamento atual. Um novo registro persistente exige projeto de recuperação, provisionamento compatível com volumes existentes e proteção contra rasgos do próprio registro. Não se deve usar silenciosamente FSInfo, setor de boot de backup ou setor reservado de terceiros como diário.

## Estratégia conservadora recomendada para um protótipo na biblioteca

Agrupar somente pedidos de alocação que já estejam em andamento, sem reservar um lote especulativo por escritor. Preservar a reserva global única. Quando vários pedidos estiverem prontos e suas entradas coincidirem na mesma página FAT, preparar seus EOCs juntos; aguardar as duas cópias; depois preparar seus vínculos juntos e aguardar novamente. Um escritor isolado continua na estratégia atual, sem esperar por outro pedido.

Isso pode amortizar gravações entre escritores sem aumentar deliberadamente a quantidade de pedidos ou reservas. Contudo, não está demonstrado que sempre tenha menos I/O que o pipeline atual: dois escritores podem estar em fases diferentes ou em páginas distintas, e a reserva única já reduz parte dos EOCs. Com blocos de 257 bytes, um escritor pode esperar sua alocação antes de o outro produzir um pedido agrupável. O ganho precisa ser medido no escalonamento real.

O protótipo precisa resolver estes pontos antes de ser aceito:

- Usar os estados existentes por operação para registrar quais imagens de EOC/vínculo entraram em cada submissão e quais cópias deram ACK. Não inferir isso apenas porque uma página está limpa: ela pode ter sido modificada enquanto uma imagem anterior estava em trânsito.
- Manter a geração completa de mídia usada pelos callbacks; não roubar bits dessa proteção para identificar lotes.
- Não bloquear o primeiro escritor à espera de um segundo. O agrupamento deve aproveitar pedidos já disponíveis, com progresso imediato quando não houver parceiro.
- Excluir diretórios, FAT16 e configurações opcionais inicialmente; preservar seus prefixos e fluxos existentes.
- Cancelamento, erro de ACK, mídia removida e perto de volume cheio devem manter a mesma propriedade de posse/liberação de cada pedido, incluindo a reserva global.
- Confirmar tamanho das estruturas, RAM estática e pilha em PIC24 normal/short-enum e x86/x64. O reaproveitamento de uma união não prova que a pilha permanecerá igual.
- Executar a auditoria de ordenação por preimagem, espelhamento, falhas de ACK, idle, três escritores, fronteiras FAT, perto de cheio, sync/close e remontagem, além de comparar I/O/throughput com dois escritores e blocos de 257 bytes.

Essa é uma hipótese de otimização compatível com as restrições a perseguir; não uma otimização implementada. O lote especulativo contíguo oferece ganho maior, mas requer um mecanismo de posse/recuperação persistente que ainda não foi demonstrado sem RAM adicional e sem ampliar a exposição a perdas de espaço. A biblioteca permanece com a estratégia validada anteriormente.
