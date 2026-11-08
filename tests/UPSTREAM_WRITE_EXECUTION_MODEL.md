# Modelo de execução da escrita no upstream, etapa por etapa

Referência: `upstream/master`, commit `60c5f3580152410cb08342942cbbe3a30a449c24`.
Arquivo analisado: `lib/asyncfatfs.c`, blob Git `81a8b854a345e329e0c289798ff6d82ef31dcf4f`.
O snapshot local `tests/build/cache8-write-20261118/upstream/lib/asyncfatfs.c` coincide com esse blob.
As linhas abaixo pertencem ao upstream, não ao arquivo atual da biblioteca.

Escopo: FAT32, nomes 8.3, oito setores de cache, clusters de 512 bytes, freefile e logging desligados.
Este é um modelo do trabalho executado pelo código, não uma atribuição de ciclos a funções.

A [comparação com a versão atual](CURRENT_WRITE_EXECUTION_COMPARISON.md) acompanha
essas mesmas etapas e identifica otimizações já existentes e hipóteses para as escritas.

## 1. Entrada em fwrite — linha 3095

1. Testa se o modo permite WRITE ou APPEND. Se não permitir, retorna zero.
2. Chama `afatfs_fileIsBusy()`. Se existir operação pendente, retorna zero.
3. Calcula o deslocamento dentro do setor com módulo 512.
4. Zera o contador de bytes aceitos.
5. Enquanto houver bytes, calcula `min(512 - deslocamento, len)`.
6. Determina se esse trecho substitui o setor inteiro.
7. Chama `afatfs_fileLockCursorSectorForWrite()`.

Retornar zero por busy não executa todo o caminho de escrita. O custo total depende de quantas vezes
o aplicativo repete a chamada enquanto processa os polls.

## 2. Obtenção da página gravável — linha 1847

### Já existe writeLockedCacheIndex

8. Calcula o setor físico do cursor.
9. Valida que o descritor bloqueado corresponde ao setor calculado.
10. Obtém o ponteiro do buffer por índice. Não procura vítima nem lê o cartão.

### Não existe página bloqueada

8. Verifica o fim da cadeia alocada.
9. Se precisar crescer, chama `afatfs_appendFreeCluster()`. Pode iniciar e tentar continuar
   a alocação nessa mesma chamada. Se não concluir, devolve NULL ao fwrite.
10. Calcula o setor físico, o deslocamento e os limites do setor.
11. Monta flags WRITE e LOCK.
12. Para escrita parcial, adiciona READ quando houver bytes anteriores ao cursor no setor
    ou quando o teste de limite do código indicar dados posteriores a preservar.
13. Chama `afatfs_cacheSector()`.
14. Se a página não estiver disponível, devolve NULL.
15. Em sucesso, converte o ponteiro em índice e guarda `writeLockedCacheIndex`.

## 3. Aquisição no cache — linhas 907 e 746

16. `afatfs_cacheSector()` valida que não está sendo solicitada escrita no setor zero.
17. Chama `afatfs_allocateCacheSector()`, que valida o limite físico do setor.
18. O alocador percorre descritores. Ao encontrar o setor não EMPTY, atualiza o timestamp e retorna.
    Durante a mesma busca também avalia páginas vazias e vítimas de substituição.
19. Num miss, escolhe página vazia, depois descartável limpa, depois a limpa elegível mais antiga.
    Páginas bloqueadas ou retidas não são vítimas elegíveis na seleção de páginas sincronizadas.
20. Se não houver vítima, retorna -1; cacheSector devolve IN_PROGRESS.
21. Se escolheu uma vítima, reinicializa seu descritor.
22. cacheSector examina o estado:
    - READING: devolve IN_PROGRESS.
    - EMPTY com READ: tenta `sdcard_readBlock()`, marca READING se aceito e devolve IN_PROGRESS.
    - EMPTY sem READ: inicializa os hints da página e continua pelo caminho gravável.
    - WRITING ou IN_SYNC com WRITE: marca DIRTY, atribuindo timestamp e incrementando o contador quando necessário.
    - DIRTY: aplica LOCK/RETAIN solicitados, entrega o ponteiro e retorna SUCCESS.

Com oito setores, cada varredura visita no máximo oito descritores, mas um hit pode terminar antes.
Miss de cache não significa necessariamente leitura: um setor novo inteiramente sobrescrito dispensa READ.

## 4. Cópia e avanço — continuação do fwrite

23. Se o ponteiro for NULL, interrompe o laço e retorna os bytes aceitos até ali.
24. Executa memcpy do trecho para a página bloqueada.
25. Soma o trecho ao contador aceito.
26. Chama `afatfs_fseekInternal()` mesmo quando o trecho termina dentro do setor.
27. Se o seek ficar pendente, interrompe o laço.
28. Caso contrário, desconta len, avança o buffer, zera o deslocamento e tenta o próximo trecho.

Aceitar bytes no cache não confirma gravação no dispositivo.

## 5. Seek — linhas 2045, 1925 e 1988

29. fseekInternal tenta `afatfs_fseekAtomic()`.
30. Dentro do mesmo setor: soma o deslocamento ao cursor e retorna imediatamente.
31. Ao cruzar setor: libera o write lock e a retenção de leitura existentes.
32. Calcula tamanho do cluster e posições relativas; verifica os limites do seek atômico.
33. Ao cruzar cluster: chama `afatfs_fileGetNextCluster()`, que usa a FAT no modo normal.
    Isso pode resultar em hit de cache, leitura pendente ou falha.
34. Se conseguiu, atualiza cluster anterior, cluster atual e cursor.
35. Se não conseguiu completar atomicamente, fseekInternal registra a operação SEEK,
    seu deslocamento e callback, retornando IN_PROGRESS.
36. A continuação do seek retoma esse trabalho nos polls seguintes, atualiza logicalSize com o máximo
    entre tamanho anterior e cursor, limpa a operação pendente e executa o callback quando presente.

Com clusters de 512 bytes, cada fronteira de setor também é fronteira de cluster.

## 6. Crescimento — linhas 1610, 1505, 1128 e 1439

37. appendRegularFreeCluster verifica busy, prepara o estado e tenta a continuação.
38. FIND_FREESPACE procura uma entrada FAT livre. A busca pode varrer entradas e precisar de leituras.
39. Quando encontra, atualiza o hint de alocação, cursorCluster e physicalSize;
    no primeiro cluster também define firstCluster.
40. UPDATE_FAT1 grava o marcador de fim no novo cluster com FATSetNextCluster.
41. Se existe cluster anterior, UPDATE_FAT2 grava o vínculo do anterior para o novo.
42. UPDATE_FILE_DIRECTORY chama saveDirectoryEntry: adquire o setor do diretório com READ|WRITE,
    publica physicalSize no modo normal e atualiza os campos do primeiro cluster.
43. COMPLETE limpa a operação de append e retorna SUCCESS.

As fases podem avançar com `goto doMore` no mesmo chamado quando o cache atende imediatamente.
IN_PROGRESS interrompe o avanço e deixa a fase para uma tentativa posterior.

**UPDATE_FAT1 e UPDATE_FAT2 não significam primeira e segunda cópias da FAT.**
São, respectivamente, terminar o novo cluster e ligar o cluster anterior.
Nesse caminho upstream, FATSetNextCluster calcula `fatSectorToPhysical(0, ...)` e altera a FAT primária.
O sucesso dessa função indica alteração aceita no cache; a continuação não insere uma espera por
ACK das duas FATs entre essas fases. Não transplantar esse comportamento para a versão atual,
que mantém garantias adicionais de espelhamento e ordenação.

## 7. Poll e gravação física — linhas 3556, 820, 689 e 658

44. O aplicativo chama afatfs_poll; ele chama sdcard_poll para avançar o backend.
45. Se o backend estiver pronto, chama afatfs_flush.
46. Havendo páginas sujas, flush percorre o cache procurando a DIRTY desbloqueada com timestamp mais antigo.
47. Selecionada a página, cacheFlushSector pode anunciar a sequência com sdcard_beginWriteBlocks,
    conforme o hint, e chama sdcard_writeBlock.
48. IN_PROGRESS: decrementa dirtyEntries, marca WRITING e sinaliza flush em andamento.
    SUCCESS imediato: decrementa e marca IN_SYNC. BUSY/FAILURE não consomem a página nesse switch.
49. O callback de escrita limpa cacheFlushInProgress e procura o descritor pelo setor/estado.
    ACK válido marca IN_SYNC; falha com buffer NULL volta a DIRTY e incrementa o contador.
    Se a página foi novamente marcada DIRTY enquanto estava em voo, não a considera sincronizada.
50. Depois de tentar flush, poll despacha INITIALIZATION ou READY.
51. Em READY, fileOperationsPoll visita CWD e os handles abertos, chamando fileOperationContinue.
    O dispatcher seleciona a continuação pelo opcode; inclusive handles sem trabalho são visitados.
    Há uma passagem desse escalonador por poll pronto no upstream.

Não existe correspondência fixa entre fwrite, poll e writeBlock: várias alterações podem atingir
a mesma página suja antes de uma gravação; tentativas busy podem não produzir nenhum I/O.

## 8. Fechamento — linhas 2913 e 2857

52. fclose verifica se pode iniciar o fechamento, reconcilia logicalSize com o cursor e registra a operação.
53. fcloseContinue atualiza o diretório com logicalSize no modo FOR_CLOSE para arquivos graváveis.
54. Libera retenção do diretório e locks do cursor, invalida o handle, limpa a operação e chama o callback.
55. O experimento de desempenho continua drenando o cache/backend para contabilizar a gravação restante.
    O callback de fechamento upstream não deve ser interpretado como a confirmação das duas FATs
    usada pela implementação atual.

## 9. Aplicação aos blocos do benchmark

- **257 bytes, cursor inicialmente zero:** o primeiro trecho cabe no setor; na chamada seguinte,
  se houver progresso disponível, copia os 255 bytes restantes e depois tenta os 2 bytes no setor seguinte.
  Nos demais blocos o deslocamento muda; não assumir sempre essa divisão.
- **4096 bytes, cursor alinhado:** são oito trechos lógicos de 512 bytes. Cada um passa por lock/cópia/seek;
  a chamada pode retornar antes dos oito, exigindo repetição com os bytes restantes.
- **Dois escritores:** cada handle conserva seu cursor/operação/lock, mas compartilha as oito páginas,
  a FAT, o diretório, o backend e o escalonador. Multiplicar por dois um custo por chamada não modela
  as mudanças de cache nem a intercalação de pedidos.

## 10. Modelo de trabalho para futuras otimizações

Registrar separadamente: tentativas fwrite e recusas busy; bytes copiados; reutilizações de lock;
hits/misses e descritores examinados; setores lidos; clusters/entradas FAT examinados;
alterações de EOC/vínculo/diretório; continuations por opcode/fase; polls;
tentativas de submissão, blocos efetivamente escritos e callbacks.

Modelo conceitual: `CPU = trabalho de entrada + cópia + cache + travessia/alocação +
escalonamento + submissão/callbacks + fechamento`. Cada parcela depende dos eventos acima,
incluindo repetições sem progresso. Não atribuir os ciclos agregados já medidos a essas parcelas:
faltam contadores e tempos exclusivos por etapa. Para quantificá-las, instrumentar uma execução
separada e evitar somar tempos inclusivos de chamadas aninhadas.

Este mapa é a referência estrutural. `UPSTREAM_WRITE_CPU_MODEL.md` conserva apenas a referência
numérica agregada anterior; não substitui o percurso de execução descrito aqui.
