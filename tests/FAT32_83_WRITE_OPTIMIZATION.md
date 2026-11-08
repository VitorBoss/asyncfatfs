# Redução de gravações FAT32 com nomes 8.3

> **Resultado hist�rico substitu�do.** A atualiza��o conjunta de EOC e v�nculo descrita abaixo pressupunha uma imagem completa de setor. O teste posterior de pr�-imagem detectou que uma grava��o parcial poderia publicar um v�nculo antes do EOC. Essa t�cnica foi removida e substitu�da pela reserva antecipada com EOC previamente dur�vel. Consulte `FAT32_83_PIPELINE.md` para a implementa��o e os resultados atuais.

Data: 16/11/2026, America/Sao_Paulo.

A atualização conjunta do EOC e do vínculo anterior, quando ambos estão no mesmo setor FAT, reduz em aproximadamente 40% as gravações de metadados e 33% o total de setores escritos com clusters de 512 bytes. O throughput no modelo de latência aumenta de 46% a 49%. O espelhamento das FATs, a publicação posterior do tamanho e as barreiras entre setores diferentes são preservados. A diferença de desempenho em relação ao upstream não foi inteiramente eliminada.

## Causa identificada

A alocação comum sem diário gravava o EOC do novo cluster nas duas FATs, drenava o cache, gravava o vínculo do cluster anterior nas duas FATs e drenava novamente antes de publicar o novo tamanho no diretório. São aproximadamente cinco gravações de metadados para cada novo cluster, além dos dados. O espelhamento explica parte do custo, mas a duplicação do setor entre EOC e vínculo é evitável.

Para FAT32, cada setor de 512 bytes contém 128 registros. Quando os registros do cluster anterior e do novo cluster pertencem ao mesmo setor, as duas alterações podem entrar na mesma imagem de setor em cada FAT, sem ceder controle à máquina assíncrona entre elas. A imagem que contém o vínculo também contém seu EOC. A drenagem antes da publicação no diretório permanece.

Quando os registros estão em setores diferentes, o EOC continua precisando ser persistido antes do vínculo. Diretórios mantêm seu fluxo de inicialização e zeragem. A alocação com diário pronto mantém o agrupamento já existente; a nova condição cobre o caminho sem essa proteção.

## Resultados antes e depois

Mesma carga do comparativo upstream: 1 MiB por arquivo, um ou dois arquivos alternados na mesma thread, cache de quatro setores, três handles, API original, sem LFN/exFAT/freefile/diário. Sete repetições de cada cenário e perfil. Valores de metadados e escrita total são setores de 512 bytes. A taxa modelada é MiB/s de payload agregado.

| Cluster B | Bloco B | Arquivos | Meta antes | Meta depois | Writes antes | Writes depois | MiB/s antes | MiB/s depois | Ganho |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 10240 | 6178 | 12288 | 8226 | 0.101 | 0.151 | 49.2% |
| 512 | 257 | 2 | 20479 | 12419 | 24575 | 16515 | 0.097 | 0.142 | 45.8% |
| 512 | 4096 | 1 | 10240 | 6178 | 12288 | 8226 | 0.102 | 0.152 | 49.3% |
| 512 | 4096 | 2 | 20479 | 12417 | 24575 | 16513 | 0.101 | 0.151 | 48.6% |
| 4096 | 257 | 1 | 1280 | 774 | 3328 | 2822 | 0.470 | 0.580 | 23.5% |
| 4096 | 257 | 2 | 2559 | 1555 | 6655 | 5651 | 0.365 | 0.427 | 17.2% |
| 4096 | 4096 | 1 | 1280 | 774 | 3328 | 2822 | 0.470 | 0.581 | 23.5% |
| 4096 | 4096 | 2 | 2559 | 1555 | 6655 | 5651 | 0.470 | 0.580 | 23.3% |

No caso de 512 bytes, blocos de 257 bytes e um arquivo, as gravações de metadados caem de 10.240 para 6.178; as gravações totais caem de 12.288 para 8.226. A leitura continua em 50 setores durante a fase de escrita. O throughput modelado passa de 0,101 para 0,151 MiB/s.

As contagens de leitura das fases de escrita não aumentaram em nenhum dos oito cenários do perfil 1. As fases de leitura mantiveram as mesmas contagens de setores nos dois perfis, inclusive o caso de dois leitores que já apresentava maior I/O que o upstream no relatório anterior.

## Condições da medição

O backend é assíncrono com mídia em RAM. No perfil 1, cada poll avança 10 µs virtuais; leitura de setor custa 200 µs, escrita isolada 800 µs e escrita múltipla 400 µs, com 100 µs de início/fim de sequência. As taxas são resultados desse modelo, não velocidades medidas de cartão físico. O perfil 0 também foi executado e seus tempos de host estão no JSON; a conclusão de ganho usa contagens e tempo virtual determinísticos.

A escrita inclui criação, dados, fechamento e drenagem. O desmonte não pode gerar gravações adicionais. A leitura ocorre após remontagem, com conferência do tamanho e conteúdo. Uma auditoria independente percorre a FAT primária e verifica conteúdo, término da cadeia e igualdade das duas FATs.

## Validação

- 224 execuções de volume antes/depois, com 448 fases medidas. Todos os conteúdos foram validados; as FATs ficaram iguais nos 224 volumes. I/O e tempos virtuais foram idênticos nas sete repetições por cenário.
- Auditoria adicional após cada gravação concluída de setor FAT: 56.012 imagens de setor verificadas, cobrindo uma e duas cadeias, clusters de 512/4.096 bytes e travessias entre setores FAT. Em cada cópia da FAT, nenhum vínculo persistido pode apontar para um registro ainda livre.
- Controle negativo: uma cópia isolada que omite o EOC na fronteira entre setores foi rejeitada pelo auditor. O código defeituoso existe somente na pasta de evidências.
- Matriz completa de regressão: sete configurações, com falhas de envio/ACK, remontagem, acesso compartilhado, remoção e desligamento seguro.
- Testes de cache e múltiplos blocos: pre-erase destrutivo, BUSY/FAILURE, locks e interrupções aprovados.
- Os 24 layouts de estruturas ficaram idênticos aos anteriores, incluindo PIC24 normal/enums curtos, x86 e x64. Nenhum campo, buffer, variável estática ou alocação foi acrescentado à biblioteca. A RAM estática padrão permanece 2.494 bytes no PIC24, 2.474 com enums curtos, 2.612 no x86 e 2.720 no x64.

O teste de prefixos de escrita não cobre setores parcialmente gravados. FAT32 sem diário continua sem recuperação atômica de múltiplos setores, e as duas FATs podem divergir transitoriamente durante uma queda. O ganho não depende de retirar a cópia secundária ou assumir atomicidade entre as duas FATs.

## Custo restante e próximo ponto de investigação

O caminho comum ainda publica o tamanho por cluster e persiste ambas as FATs. O custo observado fica próximo de três gravações de metadados por cluster de 512 bytes, com gravações adicionais nas fronteiras dos setores FAT e no fechamento. O upstream obtém números menores mantendo a segunda FAT desatualizada e agrupando alterações de outra forma.

Um ganho adicional exige agrupar publicações de tamanho ou várias alocações entre pontos duráveis, com rastreamento explícito das dependências entre FAT, diretório e dados. Simplesmente remover a drenagem final permitiria publicar um tamanho cujo encadeamento ainda não chegou ao meio; isso não é equivalente à otimização implementada. A atribuição quantitativa do custo restante exige variantes próprias, e não foi inferida apenas da contagem total.

## Evidências

- Implementação: `lib/asyncfatfs.c`, fase `AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1`.
- Auditor de ordem: `tests/upstream_comparison.c`, opção de compilação `COMPARISON_CHECK_ALLOCATION_ORDER`.
- Fontes completas antes/depois, CSVs, logs e controle negativo: `tests/build/fat32-83-allocation-optimization-20261116/`.
- Dados consolidados e hashes: `tests/FAT32_83_WRITE_OPTIMIZATION.json`.
