# Escrita atual versus upstream: execução etapa por etapa

A etapa do escalonador recebeu posteriormente um atalho para alocações pendentes;
veja [WRITE_DISPATCH_CPU.md](WRITE_DISPATCH_CPU.md). O snapshot do modelo abaixo é histórico.

Atualização posterior: a redução das cópias repetidas do espelho foi implementada
e testada em [FAT_ENTRY_COPY_CPU.md](FAT_ENTRY_COPY_CPU.md). Este modelo conserva
o snapshot anterior identificado abaixo; a seção 8 registra as hipóteses daquela análise.

Análise de 18/11/2026. Escopo: API original, FAT32 8.3, cache de oito setores,
clusters de 512 bytes, um/dois escritores, pedidos de 257/4096 bytes.
LFN, exFAT, journal, freefile e logging desligados. As fases opcionais presentes
no texto-fonte não devem ser contabilizadas como trabalho executado nesse perfil.

Referências locais verificadas:

- Atual: `lib/asyncfatfs.c`, blob do conteúdo de trabalho
  `19f9f2dab0ee12bf07c93f0d0260bab6585ff6fc` (inclui alterações não commitadas).
- `upstream/master`: `60c5f3580152410cb08342942cbbe3a30a449c24`,
  blob de `lib/asyncfatfs.c`: `81a8b854a345e329e0c289798ff6d82ef31dcf4f`.
- A numeração U abaixo corresponde a [cada etapa do modelo upstream](UPSTREAM_WRITE_EXECUTION_MODEL.md).
  As linhas da versão atual referem-se ao conteúdo acima; podem mudar com edições futuras.

Este documento modela operações e caminhos condicionais. Não atribui os ciclos
agregados do benchmark a funções individuais. Uma etapa pode executar várias vezes
por retries, ou ser inteiramente evitada por um hit ou pela reserva.

## 1. Entrada e página gravável

Código atual: `fwrite:6099`, `fileLockCursorSectorForWrite:3738`.

| Etapa U | Execução atual e comparação |
|---|---|
| 1 | Antes do teste WRITE/APPEND, valida sessão, handle, buffer e proteção de escrita global/do arquivo. Falhas registram erro. Trabalho adicional com comportamento observável. |
| 2 | Testa busy; registra BUSY na recusa e limpa o erro na entrada válida. Upstream apenas recusa. |
| 3 | Calcula `cursorOffset % 512`, como upstream. |
| 4 | Zera bytes aceitos, como upstream. |
| 5 | Calcula o mínimo entre espaço no setor e bytes restantes, como upstream. |
| 6 | Determina substituição integral de setor, como upstream. |
| 7 | Obtém página pelo helper agora forçado inline; a validação permanece. O efeito depende do compilador. |
| 8, lock existente | Calcula setor físico. Há especialização para cluster de um setor e inline seletivo no XC16. |
| 9, lock existente | Confirma correspondência do descritor com assert preservado; assert inline. |
| 10, lock existente | Retorna buffer pelo índice; não busca cache nem lê disco, como upstream. |
| 8, sem lock | Verifica fim da área alocada, como upstream. |
| 9, sem lock | Inicia/continua append; pode retornar NULL enquanto espera confirmações adicionais. Ver seção 4. |
| 10, sem lock | Calcula endereço físico e limites, como upstream, com a especialização citada. |
| 11 | Solicita WRITE e LOCK, como upstream. |
| 12 | Mantém o teste READ para preservar conteúdo parcial. Escrita integral dispensa leitura em ambas. |
| 13 | Adquire cache; existem validações e tratamento de pressão adicionais. |
| 14 | Em indisponibilidade, retorna NULL sem copiar dados, como upstream. |
| 15 | Converte ponteiro em índice e guarda lock. Inline dessa conversão depende da configuração de leitura multibloco; não presumir que esteja forçado no perfil padrão. |

## 2. Aquisição de cache

Código atual: `cacheSector:1889`, `allocateCacheSector:1380`,
`cleanFatMirror:1818`. O fast path READ simples também atende consultas da FAT
feitas durante escrita; não atende diretamente WRITE|LOCK.

| Etapa U | Execução atual e comparação |
|---|---|
| 16 | Valida sessão e read-only, além de proibir WRITE no setor zero. READ simples pode retornar pelo fast path, preservando bounds e timestamp. |
| 17 | No caminho geral, valida bounds no alocador inline. Prepara recuperação da primária pelo espelho somente para READ da FAT primária em FAT32. |
| 18 | Primeiro procura hit sem avaliar vítimas. Upstream combina hit e avaliação de vítimas na mesma varredura. Menos trabalho por descritor no hit atual. |
| 19 | No miss, faz segunda varredura: vazio, descartável limpa, limpa elegível mais antiga. Mantém prioridade do slot EMPTY correspondente. O miss pode examinar mais descritores que upstream. |
| 20 | Sem vítima, tenta liberar pressão. Para FAT, usa flush de background filtrado antes do flush geral; devolve IN_PROGRESS. Não cria buffer extra. |
| 21 | Antes de reinicializar a vítima, procura espelho limpo confirmado se elegível. Pode reconstruir a primária em RAM sem leitura; funciona inclusive reutilizando o slot do espelho. Hit não executa essa busca. |
| 22 | Mantém máquina EMPTY/READING/WRITING/IN_SYNC/DIRTY e flags de lock/retenção. READ faltante pede backend; WRITE marca sujo. Caminhos opcionais de journal/leitura multibloco não pertencem ao padrão analisado. |

## 3. Cópia e cursor

Código atual: `fwrite:6165–6204`, `fseekAtomic:3820`,
`fseekInternalContinue:3884`, `fseekInternal:3948`.

| Etapa U | Execução atual e comparação |
|---|---|
| 23 | NULL interrompe o laço, como upstream. Se nenhum byte foi aceito, distingue BUSY de NO_SPACE. |
| 24 | Executa memcpy dos dados e marca `modified=true`. O hook exFAT é inativo nesse perfil. |
| 25 | Soma bytes aceitos, como upstream. |
| 26 | Se o pedido termina dentro do setor, soma diretamente ao cursor e retorna. Evita fseekInternal, fseekAtomic e o restante do laço para esse caso. |
| 27 | Na fronteira de setor, mantém fseekInternal e a espera se pendente. |
| 28 | Quando pode continuar, atualiza len/buffer/deslocamento, como upstream. |
| 29 | Nas fronteiras, tenta seek atômico, como upstream. |
| 30 | O seek genérico ainda suporta movimento interno ao setor; fwrite normalmente já resolveu esse caso no fast path. |
| 31 | Libera lock/retenção ao cruzar setor. A política de descarte de páginas consumidas de leitores puros não se aplica ao escritor. |
| 32 | Calcula posições/tamanho e valida limites atômicos, como upstream. |
| 33 | Consulta próximo cluster; READ simples da FAT pode usar fast hit atual. Cache ausente ainda pode provocar espera. |
| 34 | Atualiza cluster anterior, atual e offset, como upstream. |
| 35 | Se necessário, registra SEEK pendente e retorna IN_PROGRESS, como upstream. |
| 36 | Continua nos polls, reconcilia tamanho e conclui operação/callback. O fast path parcial não deve ser interpretado como publicação imediata do tamanho em disco. |

## 4. Crescimento e alteração das FATs

Código atual: `appendRegularFreeCluster:3419`, continuação `:3091`,
`FATWriteSector:2265`, `FATSetNextCluster:2306`, `reserveNextCluster:2504`,
`groupDurableAllocations:3026`, `saveDirectoryEntry:2863`.

| Etapa U | Execução atual e comparação |
|---|---|
| 37 | Prepara append e tenta avançar imediatamente, preservando execução assíncrona. |
| 38 | Primeiro tenta consumir a reserva única, verificando que as páginas FAT correspondentes não estão sujas/em voo. Se não houver reserva utilizável, procura entrada livre. Upstream sempre segue a busca normal. |
| 39 | Define cursor, tamanho físico e primeiro cluster. Reserva consumida já tem EOC durável em ambas as FATs: pula sua regravação e segue para vínculo/publicação. |
| 40 | Sem reserva consumível, grava EOC novo em AMBAS as FATs. Tenta preparar um único EOC futuro na mesma página. Pode ceder a escritores já pendentes e espera NEW_CLUSTER_DURABLE antes do vínculo. Upstream altera apenas a primária sem essa barreira. |
| 41 | Se houver anterior, grava vínculo em ambas as FATs. Coordena EOCs de pares já pendentes; tenta reserva e espera FAT_DURABLE. Upstream não espera ACK das duas cópias entre essas fases. |
| 42 | Só após a barreira apropriada modifica diretório. Mantém publicação de physicalSize no modo normal, acrescenta atributo archive quando modificado e timestamp. Background pode postergar página de diretório dentro das regras de idade/estado; sync explícito preserva publicação ordenada. |
| 43 | Limpa append e devolve sucesso; `goto doMore` percorre fases prontas na mesma chamada. Esperas retornam IN_PROGRESS. |

Cada `FATSetNextCluster` atual faz, em ordem:

1. Valida cluster e calcula setor/índice da entrada.
2. Adquire primária com READ|RETAIN|METADATA, retirando WRITE nessa aquisição.
3. Obtém descritor da primária e adquire a página espelho antes de alterar qualquer entrada.
4. Libera a retenção temporária. Se o espelho não estiver disponível, retorna sem
   marcar as páginas sujas nessa tentativa. A primária não pode ser expulsa durante a aquisição.
5. Em sucesso, marca as duas páginas DIRTY.
6. Altera a entrada primária, preservando os quatro bits altos da entrada FAT32.
7. Copia **512 bytes inteiros** da primária para o espelho com memcpy.
8. Retorna sucesso de cache. A continuação ainda precisa obter as confirmações físicas.

As fases UPDATE_FAT1/UPDATE_FAT2 continuam significando EOC/vínculo, e não cópia
primária/espelho. Tanto EOC quanto vínculo passam pelo procedimento de duas cópias.
Preparar a reserva chama novamente FATSetNextCluster; pode haver outra cópia de
512 bytes antes de a mesma imagem FAT ser finalmente enviada ao backend.

A reserva usa um bit de `lastClusterAllocated`, sem novo buffer. Agrupamento atende
somente pedidos já pendentes, sem esperar um futuro escritor. Depois da barreira
compartilhada, todos os waiters elegíveis avançam antes de nova alteração nas páginas:
uma página limpa posterior não pode substituir o ACK de uma imagem anterior.

## 5. Poll, seleção de página e backend

Código atual: `poll:6768`, `fileOperationsPoll:6407`,
`flushCacheFiltered:1494`, `cacheFlushSector:1282`, callback `:1243`.

| Etapa U | Execução atual e comparação |
|---|---|
| 44 | Valida sessão e avança backend. STOPPING ainda permite progresso do backend. |
| 45 | No pipeline, atualiza idade da reserva e considera liberação ociosa. Se escrita de alocação está em voo, executa uma passagem de operações e retorna. Caso contrário, tenta flush prévio quando não há grupo pendente. |
| 46 | Seleciona DIRTY desbloqueada mais antiga, com exclusão de diretório nas barreiras de alocação e adiamento limitado no background. No padrão, o teste de adiamento vem antes do teste DIRTY; nas builds de leitura multibloco há ordem alternativa medida. |
| 47 | Submete página pelo backend, mantendo início de escrita multibloco conforme hint. Metadados e barreiras podem interromper continuidade física; pedido de 4096 bytes não garante uma única sequência física de dados. |
| 48 | Mantém transições conforme resultado da submissão. Não equiparar aceitação assíncrona a ACK. |
| 49 | Callback associa conclusão à página; falha recoloca trabalho pendente e redirty não vira confirmação indevida. Essas conclusões alimentam as barreiras ausentes no upstream. |
| 50 | Despacha INITIALIZATION ou READY/CLOSING, com decisões do pipeline antes/depois da passagem. |
| 51 | Visita slots, mas chama dispatcher apenas para operações ativas. Pode fazer segunda passagem quando o grupo pendente a justifica. Se teste é falso, retorna e evita repetir consulta idêntica; depois de segunda passagem precisa consultar novamente, pois as operações podem ter mudado. |

O teste de grupo com cache oito rejeita cedo sem dirty, com flush ativo, fora de
FAT32/cluster de um setor; depois procura dois arquivos normais com append ativo.
Ele é um filtro de escalonamento, não uma confirmação de persistência.

O [A/B existente](POLL_SECOND_PASS_AB.md) mostrou que a segunda passagem
incondicional consumiu mais CPU nos 24 casos, sem mudar I/O. No padrão:
serial +3,59–6,81%; pedidos intercalados +1,05–5,03%. Preservar a condição é uma
otimização já sustentada por medição, não uma hipótese futura.

## 6. Fechamento

Código atual: `fclose:5803`, `fcloseContinue:5757`,
`syncFileContinue:5635`.

| Etapa U | Execução atual e comparação |
|---|---|
| 52 | Valida início, reconcilia tamanho e agenda close. |
| 53 | Primeiro libera lock, devolve reserva não consumida e drena cache; depois publica tamanho lógico do diretório. Drena novamente, verifica journal quando habilitado, backend e término das sessões multibloco aplicáveis. Upstream apenas prepara atualização de diretório nessa etapa. |
| 54 | Somente após sync concluído libera retenções/handle e chama callback. Upstream libera antes da confirmação equivalente. |
| 55 | Drenagem final do benchmark continua útil, mas não torna o contrato do close upstream igual ao atual. Comparar CPU total inclui essa diferença real de trabalho. |

## 7. Como isso aparece nos quatro cenários

| Cenário | Trabalho dominante a investigar |
|---|---|
| Um escritor/257 | Mais entradas em fwrite por byte. Fast path parcial e lock reutilizado ajudam; validações continuam por tentativa. Alocação ainda ocorre a cada 512 bytes, não a cada pedido. |
| Dois escritores/257 | Acrescenta alternância de locks, disputa das oito páginas e coordenação de pares. Agrupamento só existe quando ambos têm append pendente; dois handles abertos não bastam. |
| Um escritor/4096 | Amortiza entrada em fwrite; ainda há oito segmentos e fronteiras de cluster por pedido completo. FAT/barreiras passam a pesar relativamente mais. |
| Dois escritores/4096 | Mesmas fronteiras, com pressão/agrupamento dependentes da ordem de pedidos. Medir pedidos serializados e sobrepostos separadamente. |

O [comparativo com cache oito](CACHE8_WRITE_COMPARISON.md) já mostra que não se trata
apenas do custo de entrada: para um escritor/4096 foram 6.181 escritas atuais contra
2.789 upstream; para dois/4096, 12.420 contra 5.557. No caso um/257 foram 6.181
contra 4.131. São contagens daquela rodada, não novas medições deste documento.
Não atribuir toda a diferença à segunda FAT: ordem de publicação, barreiras,
repetições do escalonador e oportunidades diferentes de coalescer páginas também contam.

## 8. Otimizações identificadas e ordem de investigação

As propostas abaixo **não foram implementadas nem medidas nesta análise**.
O ganho máximo de cada uma ainda depende de contadores por etapa.

| Prioridade | Proposta concreta | Condição para preservar comportamento e memória |
|---|---|---|
| 1 | Reduzir cópias integrais repetidas para o espelho durante preparação de EOC/reserva/vínculo na mesma página. Hoje cada FATSet copia 512 bytes. | Provar que as demais entradas do espelho já correspondem à primária antes de substituir memcpy por atualização localizada. Página espelho EMPTY pode conter lixo; página em voo tem propriedade do backend. Não basta testar que existe descritor, nem copiar só quatro bytes incondicionalmente. Sem novo bitmap/buffer estático. |
| 2 | Reutilizar aquisição do par FAT quando reserveNextCluster trabalha no mesmo setor recém-adquirido. Hoje a reserva repete FATGet/FATSet, buscas e cópia. | Helper interno limitado à operação corrente, com propriedade/estados validados, sem guardar ponteiro entre polls. Manter validação de livre/reservado, reserva única, marcação DIRTY e ACK antes do uso. Medir pilha em cascata. |
| 3 | Reduzir exames de diretório durante seleção de flush no padrão. Hoje deferDirectoryPage pode executar antes de rejeitar página não DIRTY. | Mudança precisa de novo A/B: uma ordem alternativa já existe para leitura multibloco e não foi universalmente melhor. Não mudar timestamps, seleção da vítima ou limites de adiamento. |
| 4 | Reduzir repetição de varreduras durante barreiras que ainda não podem concluir. | Usar apenas informação válida dentro da mesma passagem, sem indicador persistente novo e sem presumir ACK após alteração. Preservar passagem necessária à coordenação de dois escritores; não remover a consulta posterior à segunda passagem. |
| 5 | Especializar avanço sequencial em fronteiras de setor/cluster de 512 bytes para evitar aritmética genérica redundante. | Manter travessia FAT correta em cadeias fragmentadas, lógica de erro/seek pendente e reconciliação de tamanho. Contar consultas antes; compilador pode já eliminar parte da aritmética. |
| 6 | Diminuir custo do miss de cache sem penalizar hit. Atualmente miss pode fazer duas buscas. | Preservar prioridade do EMPTY correspondente, LRU, retain/lock e captura do espelho antes de reinicializar vítima. Testar hits/misses e todos os quatro cenários; cache oito limita o ganho absoluto. |

Oportunidades 1/2 atacam CPU mesmo quando a quantidade de I/O permanece idêntica.
Para reduzir I/O, o espaço permitido é compartilhar imagens entre alterações já
pendentes e aproveitar a reserva existente. Não unir EOC e vínculo em uma única
imagem ainda não confirmada: isso eliminaria a ordenação exigida. Também não remover
a segunda FAT, aumentar reserva, antecipar diretório ou concluir close antes dos ACKs.

## 9. Validação necessária na próxima implementação

Instrumentar em build separada: tentativas/busy de fwrite; reutilizações de lock;
hits/misses e descritores examinados; aquisições de pares FAT; número/bytes de memcpy
do espelho; EOCs novos/reservados/consumidos; fases retomadas sem progresso; avaliações
de adiamento de diretório; passagens do escalonador; submissões e ACKs por dados,
FAT primária, FAT espelho e diretório. Contadores de teste não entram na RAM de produção.

Depois, A/B com oito setores, um/dois escritores e 257/4096, pedidos serializados e
intercalados, incluindo perfis multibloco existentes. Separar CPU de I/O e latência;
não somar tempos inclusivos aninhados. Comparar RAM estática nos ABIs já utilizados,
pilha interna PIC24 normal/short estritamente abaixo de 400 bytes, testes de
espelhamento/reserva/pressão/falha e persistência aplicáveis. A rodada existente com
oito setores registrou máximo opcional de 378 bytes; esta edição documental não
recompilou nem produziu uma nova medição de pilha.

Nesta tarefa foram analisados os caminhos do código e as evidências existentes.
Nenhum código de produção, configuração, RAM ou garantia de persistência foi alterado.
