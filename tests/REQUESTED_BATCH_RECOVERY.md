# Alocação de até oito clusters por pedido e recuperação de volume sujo

Implementado no pipeline nativo FAT32 8.3, clusters de 512 bytes, cache oito setores. AFATFS_REQUESTED_BATCH_CLUSTERS=8 por padrão; um desativa lotes. Apenas clusters cobertos pelo pedido atual são alocados, limitados a uma página FAT e ao espaço livre contíguo. Reserva especulativa global continua única; o lote é propriedade do escritor, registrado no estado já existente.

## Mitigações

- O bit padrão de encerramento limpo do FAT32 é limpo e confirmado nas duas FATs antes dos EOCs do primeiro lote. A sincronização só publica o bit limpo depois de dados, vínculos e diretório confirmados, sem outro lote pendente. A preparação de novos lotes é bloqueada enquanto essa publicação está em andamento.
- Um volume sujo passa por recuperação antes de ficar READY. As referências das duas FATs e os primeiros clusters de todos os arquivos/diretórios protegem alocações vivas. Só cabeças sem referência são liberadas; ambas as FATs são confirmadas antes de uma nova varredura poder liberar os sucessores.
- A recuperação usa um setor do cache como mapa de 4096 clusters, liberado ao terminar, e o espaço da união de operações do diretório para os cursores. A árvore é percorrida sem recursão. Não há buffer estático novo.
- Páginas em trânsito não podem ser alteradas; falhas de leitura/envio/ACK são repetidas. A FAT primária é a autoridade na reconciliação: alocações faltantes da cópia secundária são inicializadas antes da publicação dos vínculos. Entradas reservadas/bad e os nibbles altos são preservados.

## Contagem de metadados

Upstream local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Arquivos de 1 MiB; nenhuma medição de CPU nesta rodada. Valores são setores de 512 bytes, perfil de latência 0.

| Ritmo | Escritores/bloco | Antes | Protótipo sem recuperação | Final mitigado | Upstream | Redução final/antes |
|---|---|---:|---:|---:|---:|---:|
| serial | 1/257 | 4133 | 4133 | 4133 | 2083 | 0.00% |
| serial | 2/257 | 8326 | 8326 | 8326 | 4165 | 0.00% |
| serial | 1/4096 | 4133 | 1363 | 1367 | 741 | 66.92% |
| serial | 2/4096 | 8324 | 2754 | 2758 | 1461 | 66.87% |
| interleaved | 1/257 | 4133 | 4133 | 4133 | 2083 | 0.00% |
| interleaved | 2/257 | 8323 | 8323 | 8323 | 1696 | 0.00% |
| interleaved | 1/4096 | 4133 | 1363 | 1367 | 741 | 66.92% |
| interleaved | 2/4096 | 8193 | 1985 | 1799 | 1538 | 78.04% |

O valor histórico 8259 →1985 é do protótipo anterior à melhoria de pares sujos. A coluna Antes usa a versão segura imediatamente anterior. O bit de volume sujo e sua confirmação acrescentam metadados; o número final é o medido acima, não uma promessa de manter exatamente 1985.

## I/O total do caso intercalado 2/4096

| Versão | Leituras | Escritas | FAT1 | FAT2 | Diretório |
|---|---:|---:|---:|---:|---:|
| before | 34 | 12289 | 4095 | 4095 | 3 |
| prototype | 770 | 6081 | 863 | 863 | 259 |
| after | 771 | 5895 | 770 | 770 | 259 |
| upstream | 453 | 5634 | 831 | 0 | 707 |

As leituras aumentam porque os lotes e os dados competem pelas oito páginas de cache. O upstream não grava a segunda FAT neste workload, portanto não oferece a mesma garantia de espelhamento. A versão final também impede alterações em páginas em trânsito; essa mudança de escalonamento e a base posterior ao protótipo permitem obter menos gravações que o protótipo, apesar dos marcadores adicionais.

## RAM e pilha

Máximo interno da matriz de 48 configurações: **378 bytes**. Recursos dos objetos de biblioteca: `{"32": {"before": {"static_ram_bytes": 4760, "code_bytes": 26923}, "after": {"static_ram_bytes": 4760, "code_bytes": 31611}}, "64": {"before": {"static_ram_bytes": 4872, "code_bytes": 36680}, "after": {"static_ram_bytes": 4872, "code_bytes": 42660}}}`.

## Recuperação e limites

Passaram 82 cortes nas gravações e 58 cortes durante a recuperação, incluindo ambas as metades dos setores, mais oito cenários de falha de envio/leitura/ACK com retries. Arquivos confirmados na raiz e em subdiretório foram preservados; FATs iguais e ausência de alocações órfãs foram auditadas no volume sintético. A campanha passou nos três perfis de leitura (padrão, multibloco e multibloco com consulta), incluindo escritores mistos 257/4096 e novo lote enfileirado durante a confirmação do marcador limpo.

A recuperação libera alocações sem dono, não tenta salvar seu conteúdo como arquivos perdidos. Cadeias ainda ligadas a arquivos são preservadas, incluindo espaço pré-alocado além do tamanho lógico. fwrite continua sendo aceitação em cache; fsync/fclose confirmam o tamanho real e os dados.

Esta recuperação não substitui um fsck completo: não resolve ciclos órfãos ou corrupção arbitrária de diretórios/FAT. Estruturas inválidas interrompem a montagem em vez de liberar dados ambíguos. A varredura usa memória limitada e pode ser demorada em volumes grandes/fragmentados; somente volumes sujos a executam. Não foi medido tempo de recuperação em cartão físico.

Evidências: tests/build/requested-batch-safe. Os snapshots e os contadores instrumentados pertencem ao harness e não ao orçamento de RAM/pilha da biblioteca.
