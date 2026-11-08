# Checkpoint do AsyncFatFS para continuar em outro chat

Atualizado em **18/11/2026**, fuso do usuário **America/Sao_Paulo**.

Este arquivo reúne o contexto da conversa, decisões aceitas, implementação atual, correções, medições e evidências. Não é uma solicitação para refazer os testes nem para iniciar uma nova implementação.

## 1. Estado do repositório e ponto de retomada

- Workspace: `C:\Users\user\Github\asyncfatfs`.
- Branch verificada: `master`.
- HEAD verificado nesta atualização: **`638d8f4bcd21f77c65dd14f8e31a9df264dc5f39`**, mensagem `.`. O commit anterior é `a504b8a` (`New functions`).
- Referência de comparação local: **`upstream/master` = `60c5f3580152410cb08342942cbbe3a30a449c24`**. Não houve atualização remota dessa referência nesta rodada.
- O checkout contém alterações **ainda não commitadas** da última otimização: `lib/asyncfatfs.c`, `tests/check_monolith_docs.py`, mais os novos `tests/cache_index_cpu_report.py` e `tests/CACHE_INDEX_CPU.md`. Esta atualização também modifica `CHECKPOINT.md`. Conferir `git status` novamente antes de agir; o usuário pode fazer commits entre turnos.
- A última tarefa de implementação/validação foi concluída: **redução do custo de conversão de endereços do cache nas configurações de leitura multibloco**, preservando o código gerado da configuração padrão. Ver seção 12 e [CACHE_INDEX_CPU.md](tests/CACHE_INDEX_CPU.md).
- Não há uma nova tarefa de implementação pendente. A solicitação atual é somente deixar este checkpoint para outro chat. Não criar commit, publicar ou alterar a configuração por conta própria.

## 2. Direção e restrições acordadas com o usuário

1. Preservar a estrutura original da biblioteca: cache assíncrono, máquinas de estado, callbacks e backend de setores.
2. Minimizar RAM, sem heap nem novos buffers de setor quando o armazenamento existente pode ser reutilizado com segurança.
3. O padrão solicitado é **API original e nomes 8.3**. API de compatibilidade FatFS, LFN e exFAT são opcionais e ficam desabilitados por padrão. As últimas otimizações focaram FAT32 com clusters de 512 bytes, um/dois leitores ou escritores e blocos de 257/4096 bytes.
4. As novas APIs foram autorizadas **nos limites atuais**: nomes ASCII, setores de 512 bytes, endereços/tamanhos de 32 bits e um volume. Não foi autorizada uma ampliação implícita para Unicode completo, LBA64 ou múltiplos volumes.
5. O usuário autorizou um diário persistente exFAT e, posteriormente, um diário FAT32 para recuperar metadados após quedas. Os dois motores foram unificados.
6. O usuário pediu que `afatfs_fileIsBusy` voltasse a ser interna. Ela é `static`; **não faz parte da API pública**. As consultas públicas globais são `afatfs_isIdle` e `afatfs_canPowerOff`.
7. Não transformar a biblioteca em implementação multithread. As operações simultâneas testadas são operações assíncronas intercaladas na mesma thread.
8. Manter a pilha interna em cascata **estritamente abaixo de 400 bytes** no PIC24, incluindo chamadas internas e endereços de retorno. O usuário delimitou esse teto às chamadas da biblioteca: exclui backend, callbacks da aplicação e rotinas do compilador/libc. Quatro bytes isolados não são críticos; o teto em cascata é a restrição.
9. Manter a organização **AsyncFS → LFN → exFAT → journal** em `asyncfatfs.c`. O usuário confirmou que “dicionário” significava journal de persistência. Não recriar os fragmentos `.inc` de produção.
10. Não aumentar RAM estática, remover a segunda FAT, ampliar a reserva única de alocação nem enfraquecer ACKs, barreiras, checkpoints ou regras de persistência para obter ganhos de CPU/I/O.

### Convenções obrigatórias

Ler e obedecer [AGENTS.md](AGENTS.md) e `.clang-format` antes de editar código. Em particular:

- Nomes descritivos de **pelo menos três caracteres para ponteiros e parâmetros de funções**.
- Uma instrução executável por linha; não juntar atribuições com `;` na mesma linha.
- Chaves em todos os `if`, `else`, `for` e `while`, inclusive uma instrução; estilo Linux. Preservar cadeias normais `else if`.
- Uma variável por declaração, inclusive membros de structs/unions.
- Múltiplos membros Booleanos de estruturas de runtime devem usar bitfields C99 `bool : 1` adjacentes, preservando alinhamento natural.
- Agrupar membros por alinhamento; flags de byte e arrays de tamanho ímpar em bytes perto do final quando isso reduzir padding.
- Não usar bitfields como representação de disco. **Não reordenar registros de disco/protocolos** nem usar `packed` para economizar RAM de runtime.
- Preservar o alinhamento explícito dos buffers de setor e do scratch, além dos prefixos obrigatórios de suboperações no offset zero.
- Medir antes/depois em 16/32/64 bits quando alterar layouts. Não somar novamente componentes que já estão incluídos no estado global.

## 3. Configuração atual e limites reais

Configuração padrão em `lib/asyncfatfs_config.h`, incluído por `asyncfatfs.c` e pela API:

```c
#define AFATFS_NUM_CACHE_SECTORS 4
#define AFATFS_MAX_OPEN_FILES 3
#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 2
#define ENABLE_AFATFS_DMA_CACHE 1
```

`ASYNCFS_ENABLE_FATFS_API` tem valor padrão `0`; `ASYNCFS_ENABLE_LFN`, `ASYNCFS_ENABLE_EXFAT`, `AFATFS_USE_FREEFILE` e logging introspectivo ficam indefinidos/desabilitados. `AFATFS_EXTERNAL_CONFIG` permite fornecer `afatfs_config.h`; há variantes em `tests/config/`. Para LFN/exFAT, não usar `#define ... 0` como forma de desabilitar: os guardas usam `defined`.

- API de compatibilidade: `ASYNCFS_ENABLE_FATFS_API=1`.
- Leitura multibloco opcional: definir `AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT=2` ou maior e implementar o contrato de sessões do backend. Sem essa macro, o backend original continua suficiente.
- Consulta opcional de sessão: `AFATFS_READ_SESSION_QUERY=1`; exige leitura multibloco e `sdcard_isReadBlockInSequence()`. Padrão `0`. A consulta deve ser pura e não iniciar/estender sessões nem transferir dados.
- O pipeline de alocação é selecionado internamente nas configurações mínimas elegíveis; preserva uma única reserva não ligada à cadeia e as duas FATs. Não habilitá-lo à força em configurações opcionais incompatíveis.

- LFN habilitado ativa o diário FAT32 automaticamente, salvo `AFATFS_DISABLE_FAT_JOURNAL`.
- FAT16 continua sem diário. FAT32 sem diário conserva ordenação, mas não recuperação atômica de metadados em múltiplos setores.
- Setores do backend: 512 bytes. Um backend diferente precisa emular esse tamanho.
- CWD é global. Operações de nomes trabalham no diretório corrente; não há resolução geral de caminhos absolutos nem movimentação geral entre diretórios.
- Um volume lógico; `afatfs_chdrive` aceita somente drive 0; code page da API é ASCII/20127.
- `ASYNCFS_ENABLE_LBA64` não é suporte funcional: a compilação é rejeitada porque backend e finder ainda usam 32 bits.
- Freefile é uma otimização FAT16/FAT32 para um proprietário contíguo por vez; não se aplica ao exFAT.

## 4. Evolução funcional realizada

### 4.1 exFAT inicial e interoperabilidade

O projeto começou com FAT e uma implementação exFAT inicial/incompleta. O trabalho completou e corrigiu descoberta/montagem, bitmap, FAT, arquivos contíguos e fragmentados, entradas secundárias, NoFatChain, VDL e metadados. Foram mantidas as máquinas de estado originais.

- Formatos externos de `mkfs.exfat`/exfatprogs e FATtools foram usados para detectar incompatibilidades.
- A tabela up-case do próprio volume é validada/descomprimida, mantendo somente a faixa ASCII necessária à API; busca e hash exFAT usam essa tabela.
- `fat_unicodeToUpperCase` não é função morta: há usos em `lib/fat_standard.c`, além dos testes. Ela não substitui a tabela up-case específica do volume exFAT nem significa suporte completo a Unicode na API.
- Entradas secundárias exFAT benignas, NoFatChain e VDL são preservados em rename. Conjuntos acima do limite existente de 19 entradas são rejeitados antes de gravar.
- TexFAT e estados externos ambíguos não são silenciosamente tratados como volumes comuns recuperáveis.

Evidências históricas: [EXFAT_INTEROP.md](tests/EXFAT_INTEROP.md), [EXFAT_POWERCUT.md](tests/EXFAT_POWERCUT.md), [WINDOWS_VHD_VALIDATION.md](tests/WINDOWS_VHD_VALIDATION.md), [API_VALIDATION.md](tests/API_VALIDATION.md).

### 4.2 API, buffers, prefetch e truncamento

- Adicionado `afatfs_fprefetch`: antecipa a leitura no cluster corrente quando o arquivo permite leitura, não está ocupado e há conteúdo elegível. A liberação de um setor completamente lido aciona prefetch. Não atravessa especulativamente a próxima cadeia de clusters.
- `afatfs_getFreeBufferSpace` deixou de ser uma estimativa baseada só no número de páginas sujas: conta páginas realmente reutilizáveis e caudas graváveis sem contar uma página duas vezes.
- `afatfs_getFileFreeBufferSpace(file)` inclui o espaço da cauda parcialmente preenchida pertencente ao próprio arquivo, sem novo armazenamento de controle.
- `afatfs_fileIsBusy` foi temporariamente exposta durante a evolução e **retornada ao escopo interno por solicitação do usuário**.
- `afatfs_ftruncate` passou a refletir o truncamento no **cursor atual**, como a semântica solicitada do FatFS. Não confundir com truncar sempre em zero. Não reduz/estende indiscriminadamente ao chamar no EOF; usar os testes e a implementação para os casos de cursor/limites.
- Foram implementadas alternativas com requests/códigos de erro/bytes transferidos, mantendo as APIs legadas.

APIs adicionadas/expandidas em `lib/asyncfatfs.h` incluem:

- `afatfs_open`, `close`, `sync`, `truncate`, `read`, `write`.
- `fstat`, `stat`, `opendir`, `readdir`, `closedir`, `rewinddir`, `findfirst`, `findnext`.
- `rename`, `unlink`, `rmdir`, `chmod`, `utime`.
- `getfree`, `getlabel`, `setlabel`, `getcwd`, `chdrive`, `setcp`.
- `expand`, `gets`, `puts`, `printf`, `forward`, `rewind`.
- `mkfs`, `fdisk` com opções explícitas e rejeição de formatação/particionamento de um volume montado.

Consultar o header para assinaturas exatas. Uma `afatfsRequest_t` é do chamador e precisa permanecer viva, assim como nomes, buffers e objetos de saída, até concluir a operação. Há um request ativo global; não inferir que as APIs com request possam ser disparadas arbitrariamente em paralelo.

Detalhes importantes:

- `close` e `sync` com request aceitam handles de diretório devolvidos por `open`; isso corrigiu uma falha encontrada na primeira validação FAT32/LFN nativa.
- Read/write/truncate continuam restritos a arquivos normais.
- `afatfs_expand(..., allocate=true)` atualiza o tamanho lógico na implementação atual e posiciona o cursor em zero. **Não presumir que uma pré-alocação deixa o tamanho lógico em zero**; essa suposição foi corrigida no teste concorrente.
- Rename usa dois handles temporários. Com o limite padrão de três, não manter dois escritores e ainda exigir que rename obtenha dois handles livres. O teste final sobrepõe rename a um escritor, e depois usa dois escritores mais um leitor.
- Para montar um LFN em `fstat`, o objeto de saída deve permanecer o mesmo entre retornos `IN_PROGRESS`; isso evita outro buffer de 256 bytes por handle.

### 4.3 Proteções, idle, desligamento e remoção do cartão

A comparação anterior com FatFS motivou proteções de acesso/compartilhamento e durabilidade, com testes de modos/atributos somente leitura, leitores compartilhados, escritores exclusivos e fechamento FAT16/FAT32 aguardando gravações.

- `afatfs_isIdle()` verifica requests, enumeração, operações de arquivos, CWD, páginas sujas/bloqueadas, leituras/prefetch, gravações e estados pendentes do diário. Um sistema montado pode estar idle sem estar pronto para desligamento físico.
- **`afatfs_canPowerOff()` só é verdadeiro no estado `STOPPED`**, obtido após encerramento limpo. Não trocar isso por `isIdle`, por ausência momentânea de I/O ou por ausência de erro.
- Para desligar, continuar tentando `afatfs_destroy(false)` e processando a máquina até concluir; só então usar a autorização de power-off. `destroy(true)` é descarte forçado, não durabilidade.
- Erros de envio, leitura, ACK e finalização do backend mantêm dados pendentes e bloqueiam autorização de desligamento.
- `afatfs_notifyMediaRemoved()` é a notificação explícita de remoção: o Booleano de readiness do backend sozinho não identifica remoção física.
- Remoção invalida sessão/handles e qualquer autorização anterior de desligamento. Reinserção requer inicializar backend e chamar `afatfs_init()` para uma sessão nova.
- `sdcard_reset()` deve cancelar DMA/acessos pendentes a buffers **sincronamente**, sem iniciar novas operações. Callbacks atrasados precisam preservar o `callbackData` original.
- Cache e diário usam uma geração de sessão de 32 bits para ignorar callbacks antigos, inclusive após troca por cartão com mesma geometria/serial. Não há geração adicional por handle nem heap.
- FAT32 clean shutdown também aguarda atualizações de FSInfo e seus ACKs; testes conferem hints desconhecidos quando apropriado.

Os oito cenários exFAT que antes ficavam somente leitura após reinserção foram resolvidos pela **ordem de encerramento**, não pela aceitação permissiva de um log ambíguo:

1. Drenar dados/metadados e confirmar o diário vazio ainda pertencente à biblioteca.
2. Limpar o marcador dirty e aguardar o ACK.
3. Liberar ownership do diário e aguardar o ACK.
4. Finalizar o backend.

Os oito novos handoffs recuperam graváveis. Estados legados/externos `unowned + dirty`, cabeçalhos inválidos e situações ambíguas continuam protegidos em somente leitura, com zero gravações. A matriz documentada contém 186 cenários de remoção, além de verificações negativas. Ver [MEDIA_REMOVAL_VALIDATION.md](tests/MEDIA_REMOVAL_VALIDATION.md).

### 4.4 MBR, GPT e CRC

- MBR permanece compatível para FAT16/FAT32/exFAT habilitado.
- **GPT foi adicionado para exFAT**, sob `ASYNCFS_ENABLE_EXFAT`; isso não habilita FAT16/FAT32 em GPT nem volumes sem partição.
- MBR protetor tem precedência em tabelas híbridas. Procura Basic Data com boot exFAT, respeita atributo read-only e pula partições inadequadas.
- Valida CRC de cabeçalho e array inteiro, geometria, limites e sobreposição; aceita entradas maiores que um setor quando o tamanho segue o formato suportado.
- Usa GPT secundária quando a primária é inválida, sem reparar a tabela. Há limites na localização do backup quando o MBR está saturado e não existe cabeçalho primário válido; o backend não tem API de capacidade.
- GPT e volume precisam caber nos endereços atuais de 32 bits.
- CRC32 bit a bit foi substituído por **tabela `static const` de 256 entradas**, aproximadamente 1 KiB de armazenamento somente leitura, também usada pelo diário. Não transformar essa tabela em RAM inicializada.
- Scratch GPT já compartilha espaço com a tabela ASCII up-case, pois essas fases não coexistem.

### 4.5 LFN FAT16/FAT32

- LFN é opcional e **desligado por padrão**; esta seção descreve seu comportamento quando habilitado.
- Nomes folha ASCII imprimíveis até 255 caracteres; preservação de caixa, comparação sem diferenciar caixa e acesso por nome longo ou alias 8.3.
- Aliases numéricos gerados com verificação de colisões. Aceita aliases do Windows, inclusive os derivados de hash; não exigir igualdade entre o algoritmo de alias da biblioteca e o do Windows.
- Validação incremental das entradas UTF-16LE: ordinais, checksum, tipo, cluster e padding, inclusive em diretórios fragmentados. Cadeias inválidas recaem para a entrada curta.
- API Unicode completa não foi implementada; nomes não ASCII em disco podem ser acessados pelo alias curto.
- Criação publica LFN antes da entrada curta, preserva o marcador final e trata órfãos. Exclusão torna a entrada curta removida durável antes de liberar slots LFN.
- Crescimento de diretórios reserva/termina/zera o novo cluster antes de publicar o elo. Atualizações FAT16/FAT32 escrevem as duas FATs.
- Rename no mesmo setor publica/retira as entradas curtas juntas quando aplicável.
- Foram corrigidas referências a clusters livres e reservas oferecidas indevidamente a outro arquivo.

Sem diário, só as barreiras ainda deixavam 75 de 185 cortes com órfãos, FATs divergentes ou rename incompleto. O usuário escolheu adicionar o diário FAT32, em vez de aceitar esse limite.

## 5. Diário compartilhado e invariantes de recuperação

Arquivos principais:

| Arquivo | Papel |
| --- | --- |
| `lib/asyncfatfs_journal.h` | Enums e controlador de runtime compartilhado |
| Seção journal de `lib/asyncfatfs.c` | Motor comum: captura, índice em disco, CRC, I/O, checkpoint e replay |
| Adaptador FAT32 nessa seção | Descoberta/alocação/dirty markers específicos de FAT32 |
| Adaptador exFAT nessa seção | Integração do journal exFAT |

- Diário privado `AFATFS.JNL`, contíguo, hidden/system/read-only. Nome e alocação são reservados.
- Em FAT32, capacidade de `2 * fatSectors + 64` setores de metadados distintos; dois cabeçalhos alternados e um descritor + before-image por registro, arredondado a clusters. Precisa de espaço contíguo para provisionar.
- Sequência fundamental: **before-image durável → descritor durável → cabeçalho/publicação durável → permissão para alterar metadado → checkpoint**.
- Antes de escrever durante recovery, valida todos os registros/imagens publicados e seus destinos. Recovery pode sofrer outra queda e ser repetido.
- A cadeia contígua do diário FAT32 é validada nas duas FATs antes do replay.
- Clusters liberados não são reutilizados antes do checkpoint correspondente.
- Formatos de disco preservados: FAT32 `AFJFT001`/`AFFUNDO1`; exFAT `AFJNL002`/`AFUNDO02`. Não houve migração obrigatória ao unificar os motores.
- Usa **um buffer alinhado de 512 bytes** e a imagem original retida no cache. Foram eliminados buffers redundantes de before-image/header e um handle permanente de montagem exFAT.
- A montagem exFAT usa temporariamente `openFiles[0]` e o devolve antes de READY; não reduz o limite público de arquivos após montar.
- `afatfsJournal_t` mede atualmente **560 bytes** nas ABIs medidas, incluindo o buffer de 512 bytes.

### Enums e uniões de fases

Enums de inicialização, shutdown FAT32 e recuperação substituíram passos numéricos; os seletores continuam armazenados em bytes, sem inchar estruturas pelo tamanho de enum da ABI.

- `lifecycle`: `initPhase` / `shutdownPhase` compartilham um byte. `shutdownPhase` é resetado para `WAIT_COMMIT` na transição global `AFATFS_INITIALIZATION_DONE`, antes de READY.
- **Não resetar o workspace de captura nessa transição**: a inicialização de freefile/log pode já estar capturando metadados.
- `work` (12 bytes): setup / recovery / capture são exclusivos por tempo de vida.
- Recovery precisa de **destino, progresso e CRC simultaneamente**; esses três valores não podem ser sobrepostos entre si.
- Capture usa `scan` ou `imageCRC`, nunca ambos na mesma fase. `pendingCache` é inicializado com `-1` ao entrar na captura normal.
- `mount`: descoberta de entradas exFAT / `recoveryPhase` compartilham armazenamento.
- `phase` comum **não** foi sobreposto aos seletores de montagem/shutdown: erros de montagem e commits pendentes precisam coexistir com eles.
- `headerSlot` usa bitfield Booleano porque só representa os slots 0/1; nunca é uma representação em disco.

### Limites que não devem ser esquecidos

- A proteção começa **após provisionar com sucesso um diário confiável**. Queda durante a primeira reserva pode exigir reparo externo; não inferir recuperação segura de logs ausentes/ambíguos.
- FAT16 e FAT32 sem diário não adquirem automaticamente as garantias do FAT32 com diário.
- Protege metadados, não atomicidade de sobrescrita de payload no lugar nem dados ainda na RAM.
- ACK do backend deve representar escrita ordenada e durável. Não autorizar desligamento apenas porque uma transferência foi aceita.
- Windows não executa esse replay privado. Após queda de uma sessão da biblioteca, recuperar com a biblioteca antes de entregar o volume a outro escritor.
- Não apagar, mover, truncar ou desfragmentar `AFATFS.JNL` com outro driver.
- Corrupção arbitrária e escrita externa interrompida não são autorizadas para recuperação automática especulativa.

## 6. Otimizações de RAM realizadas

Além dos bitfields/alinhamento, as seguintes uniões aproveitam usos exclusivos:

- Criação: aliases/reserva FAT versus contadores exFAT.
- Criação LFN: parser versus contadores de reserva/publicação; reinicializar na troca de fase.
- Truncamento: contador de clusters a conservar / próximo cluster a liberar / início da devolução contígua ao freefile. O scratch de FSInfo continua com os valores que precisam coexistir separados.
- Expansão: conversão da cadeia versus progresso da zeragem do diretório; zeragem é inicializada depois de terminar a conversão.
- Montagem exFAT: validação de boot / busca no diretório / decodificação up-case.
- Inicialização global: montagem exFAT versus busca/preparo do freefile FAT, porque o caminho de freefile é pulado em exFAT.

Não foram sobrepostos: nome solicitado e alias curto durante publicação, índice de órfão LFN e progresso de escrita quando coexistem, entradas/posições exFAT permanentes, callbacks/campos de cursor necessários entre operações. Não aumentar complexidade para uniões que não reduzem `sizeof`.

### Medidas históricas de 11/11, configuração então padrão com LFN

`afatfs_t` já inclui cache, diário e handles internos. Valores em bytes:

| ABI | Global | `afatfsFile_t` | Diário |
| --- | ---: | ---: | ---: |
| PIC24/16 bits, enums normais | 4112 | 338 | 560 |
| PIC24/16 bits, enums curtos | 4092 | 336 | 560 |
| x86/32 bits | 4204 | 352 | 560 |
| x64/64 bits | 4272 | 368 | 560 |

A etapa de uniões fora do diário economizou mais 8/16/32 bytes globais no padrão 16/32/64 bits e até 48 bytes na configuração combinada LFN+exFAT. As últimas correções de I/O/progresso **não acrescentaram membros nem buffers**; os 24 layouts ficaram idênticos. Isso mede estruturas de runtime, não promete igualdade do tamanho de código ou de toda a pilha.

Relatórios sequenciais: [BITFIELD_ALIGNMENT.md](tests/BITFIELD_ALIGNMENT.md), [MEMORY_LAYOUT_REPORT.md](tests/MEMORY_LAYOUT_REPORT.md), [MEMORY_IO_OPTIMIZATION.md](tests/MEMORY_IO_OPTIMIZATION.md), [JOURNAL_LIFETIME_MEMORY.md](tests/JOURNAL_LIFETIME_MEMORY.md), [OPERATION_LIFETIME_MEMORY.md](tests/OPERATION_LIFETIME_MEMORY.md).

## 7. Otimizações de I/O e escrita em múltiplos blocos

### Por que o poll chama flush

`afatfs_poll` faz gravações de segundo plano progredirem. `afatfs_flushCache` tenta gravar o setor sujo desbloqueado mais antigo; não grava páginas limpas e não inicia outro flush enquanto há um em andamento. Os caminhos `#ifdef/#else` do poll são alternativos, não dois flushes consecutivos.

Mesmo assim, escoar cedo demais metadados que serão alterados novamente pode causar regravações. O trabalho passou a adiar flushes oportunistas somente nas fases elegíveis, preservando as barreiras explícitas.

### Política atual

- Bufferização de alocação FAT32 com diário válido, inclusive novo EOC e elo anterior quando seguro.
- Adiamento nas fases elegíveis de criação/publicação LFN/exFAT, conversão e liberação de cadeias, além da reserva inicial do diário.
- Pressão de cache força progresso. `blockAllocation` esperando checkpoint permite escoamento, evitando espera circular.
- Um arquivo apenas aberto/inativo não adia o flush.
- Diretórios, publicação, VDL, sync/close, checkpoint, dirty/clean markers e shutdown conservam as dependências de persistência.
- Agrupamento amplo de alocação, incluindo a busca de clusters, foi testado e descartado por aumentar leituras/I/O. Não voltar a adiar tudo indiscriminadamente.

### Múltiplos blocos

- `AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT` agora é aproveitado também sem freefile quando os próximos setores sujos/desbloqueados na **ordem original de gravação** são consecutivos fisicamente.
- Não há reordenação de FATs espelhadas/bitmap/journal só para formar lotes. Novos lotes usam no máximo as páginas de cache já prontas.
- Leituras ou endereços não consecutivos podem interromper a sequência pelo contrato do backend.
- Tratar `sdcard_beginWriteBlocks`: em BUSY/FAILURE, preservar o setor sujo e não enviar o bloco; FAILURE registra `AFATFS_ERROR_BACKEND_IO`.
- **Não anunciar metadados futuros ainda fora do cache**: um backend pode pré-apagar a faixa antes de suas imagens undo estarem duráveis. Hints antigos de FATFillWithPattern não podem autorizar isso.
- O hint futuro de append contíguo via freefile permanece para dados elegíveis, até o limite armazenado de 65535 blocos.
- Sync/close/shutdown aguardam também o fim da operação múltipla.
- Não tentar agrupar especulativamente imagem/descritor/cabeçalhos do diário mudando sua ordem ou adicionando buffers.

### Ganhos medidos e como interpretá-los

Streaming de 64 KiB por arquivo, chunks de 257 bytes, clusters de 512 bytes, incluindo fechamento/desmontagem; contagens finais preservadas após a última correção:

| Sistema | Escritores | Leituras | Gravações |
| --- | ---: | ---: | ---: |
| FAT32 | 1 | 222 | 451 |
| FAT32 | 2 | 1348 | 1099 |
| exFAT | 1 | 37 | 534 |
| exFAT | 2 | 1440 | 2453 |

A otimização anterior comparada a `fc6a293` havia reduzido FAT32 de 815 para 451 gravações (um escritor) e de 4407 para 1099 (dois), com tradeoff de leituras documentado. Primeira reserva do diário passou de 4381 para 2221 gravações FAT32 e de 561 para 284 exFAT.

Na medição de operações LFN com cluster de 512 bytes, criação passou de 58 para 40 setores gravados e rename de 54 para 45; a redução depende do cenário e inclui o diário. Não generalizar a todas as operações/tamanhos de cluster.

Na gravação de 64 KiB pré-alocados:

| Sistema | Setores gravados antes/depois | Inícios de sequência antes → depois | Blocos agrupados depois |
| --- | ---: | ---: | ---: |
| FAT32 | 137 / 137 | 137 → 43 | 127 |
| exFAT | 136 / 136 | 136 → 51 | 127 |

São **sequências múltiplas + escritas isoladas**, não redução das chamadas de payload necessárias nem medida de tempo físico do cartão. VHD/backend simulado não fornece ganho de tempo real de SD. Ver [IO_BATCHING.md](tests/IO_BATCHING.md) e sua validação posterior nativa.

## 8. Última validação nativa Windows e correção encontrada

Fonte desta rodada histórica: **[WINDOWS_FAT32_IO_BATCHING.md](tests/WINDOWS_FAT32_IO_BATCHING.md)** e JSON ao lado. Ela não valida automaticamente a revisão de 17/11. O usuário informou que trocou de branch e perdeu muitos testes, VHDs e outras imagens; verificar a existência dos artefatos antes de tentar reproduzir essa rodada.

- Windows `10.0.26300.0`; `fastfat.sys` `10.0.26100.9711`.
- Três VHDs fixos **novos**, 512 MiB cada, MBR, formatados pelo Windows: clusters de 512 bytes, 4096 bytes e padrão nativo (efetivamente 4096 bytes).
- **12 CHKDSK sem reparo**, todos exit code 0: baseline, depois da biblioteca, depois das alterações nativas e depois do retorno pela biblioteca.
- Nenhum `/f`, `/r`, `/x` ou reparo usado para obter sucesso.
- Nome longo/alias, conteúdo e tamanhos validados nos dois sentidos; nomes até 255 caracteres, truncamento, rename, exclusão, diretórios e padrões de enumeração.
- Três criações e três fechamentos LFN pendentes; rename com outro escritor; dois escritores intercalados com leitor/prefetch.
- Windows também fez gravações paralelas em tarefas sincronizadas por barreira e sobrescreveu arquivos da biblioteca, relidos na montagem seguinte.
- Biblioteca e Windows **alternam acesso exclusivo**; a biblioteca nunca escreve no arquivo bruto enquanto o VHD está montado.
- Auditoria administrativa posterior montou somente leitura, verificou mais quatro arquivos concorrentes por nome longo e alias distinto em cada VHD, desmontou e confirmou rodapés SHA-256 intactos.
- Contadores de múltiplos blocos são opcionais em `tests/sdcard_sim`, não membros da biblioteca.

| Caso final | Sequências múltiplas | Blocos em sequência |
| --- | ---: | ---: |
| Cluster 512 | 43 | 128 |
| Cluster 4096 | 73 | 197 |
| Padrão nativo | 73 | 197 |

### Bug real descoberto nessa rodada

A primeira rodada com cluster de 4 KiB travou ao criar um nome de 255 caracteres. O adiamento do flush deixava **três páginas sujas e só uma reutilizável**. A busca repetia o percurso da FAT e a leitura da entrada LFN; uma expulsava a outra. A alocação de página sempre conseguia reutilizar uma página limpa, então o mecanismo de flush por falta de cache nunca disparava.

Correção em `afatfs_deferBackgroundFlush` / `afatfs_directoryNeedsCache`:

- Nas fases `WRITE_FILE_ATTRIB` e `WRITE_FILE_ENTRY` de criação, permitir escoamento quando há menos de **duas páginas reutilizáveis**.
- Considerar também os criadores internos de freefile/log.
- **Não aplicar essa reserva globalmente a toda alocação**: uma tentativa intermediária aumentou as escritas de dois escritores de 1099 para 1645. A versão final restringiu a condição ao caso LFN e voltou a 1099.
- Nenhuma estrutura ou buffer adicionado. Há regressão em `tests/cache_batching.c` tanto para a criação com três páginas sujas quanto para preservar a bufferização normal de alocação FAT32.
- A criação que travava passou na mesma imagem de diagnóstico. A aprovação nativa final veio de outra rodada completa em **três VHDs novos**, não de reparar/reaproveitar a imagem falhada.

### Evidências finais locais

- Rodada nativa aprovada: `tests/build/windows-fat32-lfn/run-io-batching-native-final-20261110/`.
  - `report.json`, `workload.exe`, três VHDs, manifests/aliases e logs de comandos.
  - `cleanup-verification.json`: auditoria de conteúdo/aliases, desmontagem e rodapés.
- Regressões da versão final: `tests/build/windows-native-batching-final/`.
  - `regression.log`, `asan.log`, `layout/report.json`, `benchmark/`, `multi-*/`, `cache-*/`, `powercut/`.
- Resultados finais: seis configurações de regressão; oito testes legados **compilados**; ASan combinado LFN+exFAT+freefile; 24 layouts idênticos.
- FAT32 final: **3957 cortes de operação + 2142 cortes durante replay + 28 proteções + 86 cortes de provisionamento**, todos aprovados. Estes são testes simulados, não quedas físicas no driver Windows.
- Rodadas intermediárias de diagnóstico também permanecem em `tests/build/`; não confundir com a rodada final. Os arquivos dessa árvore são ignorados pelo Git e podem não existir em um clone novo.

## 9. Mapa de implementação e documentação

| Arquivo | Onde continuar a análise |
| --- | --- |
| `lib/asyncfatfs.c` | Configuração, runtime, cache, prefetch, FAT/exFAT, montagem, GPT, poll, idle/power-off, media sessions |
| `lib/asyncfatfs.h` | API pública, requests, informações, cursores e opções |
| `lib/asyncfatfs_config.h` | Padrões e seleção das funcionalidades opcionais |
| Seção LFN em `lib/asyncfatfs.c` | Parser/publicação/exclusão LFN e aliases |
| Fim da seção AsyncFS em `lib/asyncfatfs.c` | API opcional com requests e formatação |
| Seção exFAT em `lib/asyncfatfs.c` | Montagem, GPT e operações específicas de exFAT |
| `lib/asyncfatfs_journal.h` e seção journal em `lib/asyncfatfs.c` | Controlador, motor comum e adaptadores do diário |
| `lib/fat_standard.c/.h` | Registros FAT/exFAT e auxiliares de nomes/checksums |
| `lib/sdcard.h` | Contrato do backend e cancelamento/durabilidade |
| `doc/Using_asyncfatfs.md` | Integração, configuração, limites e contratos atuais |
| `tests/test_exfat.c`, `tests/fat_lfn.inc` | Regressão sintética atrasada/com falhas; o nome do teste não significa que só testa exFAT |
| `tests/cache_batching.c` | Backend destrutivo, BUSY/FAILURE, ACK, locks, ordem, leitura interrompendo lote e regressão de progresso |
| `tests/metadata_batch.c` | Comparativo de I/O de streaming |
| `tests/bitfield_layout.c` | Layout/flags/alinhamento, incluindo PIC24 C99 |
| `tests/test_exfat_external.c`, `tests/fat_lfn_native.inc`, `tests/sdcard_sim.c/.h` | Workload nativo e backend de imagem original instrumentado opcionalmente |

### Relatórios: mais recente versus histórico

Os documentos mais recentes são `tests/CACHE_INDEX_CPU.md` e `tests/OPTIONAL_STACK_BUDGET.md`. `tests/WINDOWS_FAT32_IO_BATCHING.md/.json` é a validação nativa histórica de 11/11. Cada relatório contém evidências de sua revisão, não aprovação automática de qualquer revisão futura.

- `API_VALIDATION.md`: APIs, fsck e interoperabilidade histórica; inclui ampliação MBR/GPT e limites.
- `EXFAT_INTEROP.md`: matriz externa exFAT; `EXFAT_POWERCUT.md`: evolução do diário exFAT.
- `MEDIA_REMOVAL_VALIDATION.md`: geração de sessão, contrato de cancelamento e oito handoffs.
- `LFN_VALIDATION.md/.json`: implementação LFN e primeira memória medida.
- `WINDOWS_FAT32_LFN_VALIDATION.md/.json`: primeira rodada nativa LFN, anterior às otimizações recentes.
- `FAT32_LFN_POWERCUT.md/.json`: implantação/unificação do diário, com contagens anteriores.
- `MEMORY_IO_OPTIMIZATION`, `JOURNAL_LIFETIME_MEMORY`, `OPERATION_LIFETIME_MEMORY`: etapas consecutivas de economia de RAM.
- `IO_BATCHING.md/.json`: agrupamento antes da correção de progresso encontrada no VHD.
- `WINDOWS_FAT32_IO_BATCHING.md/.json`: validação **final**, correção, memória e novas regressões.

**Cuidado:** alguns relatórios antigos afirmam que FAT16/FAT32 não têm diário ou mencionam buffers/tamanhos antigos. Isso descreve a revisão histórica daquele relatório. Para comportamento atual, usar código, documentação atual e relatório final; não transplantar afirmações antigas para o estado atual.

## 10. Como executar verificações no ambiente atual

Ambiente Windows/PowerShell, MSVC, Python e XC16 já disponíveis. Caminhos usados:

- Python: `C:\Users\user\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe`.
- MSVC: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat` e `vcvars32.bat`.
- clang-format: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin\clang-format.exe`.
- XC16: `C:\Program Files\Microchip\xc16\v2.10\bin\xc16-gcc.exe`; alvo PIC24 `24FJ256GA702`, C99, enums normais/`-fshort-enums`.
- Dependências de teste: FATtools 1.1.23 em `tests/build/vendor`, requisitos em `tests/requirements-exfat.txt`; exfatprogs Windows 1.3.2-1 em `tests/build/external-tools/` quando presente.

Rotinas usuais, na raiz do repositório:

```powershell
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/nova-rodada/layout
./tests/run_io_batching_tests.ps1 -OutputDirectory tests/build/nova-rodada
./tests/run_metadata_benchmark.ps1 -OutputDirectory tests/build/nova-rodada/benchmark
./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/nova-rodada/powercut -Torn
```

Para comparar antes/depois, salvar **toda `lib/` antes da mudança** em uma pasta de baseline e passar `-BaselineDirectory` aos dois runners de benchmark. Não usar só `git show HEAD` quando houver modificações locais relevantes. Não sobrescrever evidências antigas sem intenção.

Os seis casos de configuração são padrão, `fat_only`, `fat_lfn`, `exfat_only`, `exfat_freefile` e `lfn_exfat`. O layout mede x86/x64 e PIC24 normal/short, total 24. O teste específico também cobre `no_multiblock`.

ASan foi executado compilando `tests/test_exfat.c`, `lib/asyncfatfs.c` e `lib/fat_standard.c` com MSVC `/W3 /WX /fsanitize=address /Zi`, configuração externa `tests/config/lfn_exfat`, `TEST_LFN` e `AFATFS_USE_FREEFILE`, em diretório de objetos próprio.

### Windows/VHD

```powershell
./tests/run_windows_fat32_lfn_tests.ps1
# Opções: -Cases all|512|4096|default, -RunName run-..., -CompileOnly, -NoWait.
# Depois de concluir a mesma rodada, como administrador:
./tests/audit_windows_fat32_lfn.ps1 -RunName run-...
```

- O runner compila e pode lançar um worker administrativo oculto. `-NoWait` **não significa aprovação**: consultar `report.json` e término do worker.
- Criar/formatar/montar/consultar VHDs exige administrador; inclusive `Get-DiskImage` retornou acesso negado no processo não elevado.
- O usuário autorizou a validação VHD desta tarefa. As elevações usadas ficaram restritas aos VHDs isolados da pasta de testes; não equivalem a autorização para discos físicos ou formatação de outros volumes.
- Manter todas as verificações de caminho absoluto, vínculo entre VHD e disco, tamanho, barramento virtual, IsBoot/IsSystem, partição e rótulo.
- Não acessar o VHD bruto enquanto montado. Usar desmontagem em `finally`; uma auditoria somente leitura também precisa desmontar no fim.
- VHDs finais já ficaram desmontados. Não deixá-los montados ao terminar novos trabalhos.

Geradores como `windows_fat32_batching_report.py`, `io_batching_report.py`, `operation_lifetime_report.py` e `journal_lifetime_report.py` verificam relatórios e/ou hashes de rodadas específicas. Alguns defaults apontam para pastas datadas locais. **Não executá-los cegamente depois de mudar fontes para tentar renovar uma aprovação antiga**; gerar evidência nova e ajustar entradas conforme necessário.

### Limites das validações

- PIC24/16 bits: compilação cruzada e asserts de layout, não execução no hardware.
- Cartões físicos não foram testados; VHD não mede eletrônica, tempo ocupado real de SD nem perda de energia física.
- Cortes/torn writes e replay usam imagens/backend simulado; o teste de pré-apagamento destrutivo é separado.
- Os oito testes legados foram compilados na matriz final; não afirmar que todos foram executados nela. Execuções históricas específicas estão nos relatórios correspondentes.
- `fsck.exfat` teve falhas de execução no processo restrito; checks históricos concluídos fora dessa limitação exigiram exit code zero **e** resultado clean. Não aceitar só uma mensagem clean com processo que termina em erro.
- Não há `fsck.fat` instalado documentado nesta sessão; FAT32 foi conferido por auditor independente/FATtools e, na rodada nativa final, pelo CHKDSK sem reparos.

## 11. Recomendações para o próximo chat

1. Ler este arquivo, `AGENTS.md`, `git status` e o pedido novo do usuário antes de agir.
2. Preservar o padrão **API original/8.3**, com API FatFS, LFN e exFAT opt-in; não ampliar escopo para Unicode/LBA64/multivolume sem pedido.
3. Não remover flushes por parecerem redundantes sem identificar as dependências de durabilidade e de progresso do cache.
4. Não sobrepor variáveis que precisam sobreviver juntas entre retornos assíncronos.
5. Ao otimizar I/O, comparar leituras **e** gravações, um/dois escritores, montagem e conteúdo após remontar; menos escritas pode custar mais leituras.
6. Manter a correção de duas páginas para criação LFN, sem transformá-la numa limitação global de toda alocação.
7. Validar BUSY/FAILURE/ACK, cache pequeno, leitor retendo páginas, fragmentação, prefetch, checkpoints e `canPowerOff`.
8. Não tomar relatórios históricos como evidência de uma modificação futura. Conferir hashes/ABIs e produzir nova rodada pertinente.
9. O usuário prefere execução autônoma e não gosta de confirmações repetidas. Pedir apenas informação realmente necessária ou a elevação exigida pelo ambiente; nunca formatar disco físico como substituto do VHD de teste.
10. Não há correção adicional identificada e deixada pendente na última rodada. Esperar o próximo objetivo do usuário.

## 12. Estado consolidado de 17/11/2026

Esta seção registra o ponto de retomada mais recente. As medições nativas e de RAM com LFN das seções anteriores são históricas; não confundir aquela configuração com o padrão atual 8.3.

### 12.1 Escrita, reserva única e leitura multibloco

- Foram implementadas otimizações de metadados, pipeline com reserva antecipada única e agrupamento de alocações **já pendentes** entre escritores. Não criar lotes especulativos de vários clusters. EOCs e destinos precisam estar duráveis nas duas FATs antes de publicar vínculos; os vínculos também aguardam os dois ACKs.
- O agrupamento pode aproveitar pedidos simultaneamente pendentes, mas não espera por um escritor futuro. Nenhum novo campo, buffer ou heap foi acrescentado. A comparação precisa manter o mesmo escalonamento do harness nos dois lados.
- Relatórios: `FAT32_83_WRITE_OPTIMIZATION`, `FAT32_83_CACHE`, `FAT32_83_FAT_WRITE_COST`, `FAT32_83_PIPELINE`, `FAT32_83_BATCH_ALLOCATION` e `FAT32_83_PENDING_ALLOCATIONS`, em `tests/`. O documento de investigação de lote antecede a implementação de pedidos pendentes.
- `afatfs_fprefetch()` continua oportunista. O backend multibloco opcional pode usar uma extensão contígua verificada além do cluster atual, sem acrescentar buffers: cada setor ainda precisa de uma solicitação aceita e de seu callback. Sessões não podem escrever, pré-apagar ou possuir buffers futuros.
- O backend oferece `sdcard_beginReadBlocks()` e `sdcard_endReadBlocks()` quando o recurso está ligado. A consulta opcional `sdcard_isReadBlockInSequence()` somente informa se o próximo bloco pode ser aceito na sessão já delimitada; não muda estado ou contadores.
- Leituras não consecutivas e escritas encerram a sessão de leitura com segurança. Falhas, reset, remoção e callbacks antigos preservam a geração completa de mídia. Flush, idle e encerramento limpo precisam respeitar a parada pendente.
- A política de cache eliminou as **510 releituras adicionais** com dois leitores e blocos de 4096 bytes em clusters de 512 bytes: uma página consumida e liberada, de arquivo regular exclusivamente de leitura, vira candidata preferencial à substituição pelo bit `discardable` existente. Reter novamente remove a preferência. Não descartar páginas sujas, bloqueadas, retidas ou em voo.
- A inicialização do ponteiro do cache DMA foi corrigida para ocorrer depois de zerar o estado em `afatfs_resetMediaSession()`, com cancelamento do backend antes do reset. Não mover a atribuição de volta para antes do memset.
- Detalhes e comparações: [FAT32_MULTIREAD.md](tests/FAT32_MULTIREAD.md), [FAT32_TWO_READER_CACHE.md](tests/FAT32_TWO_READER_CACHE.md) e [UPSTREAM_FAT32_83_COMPARISON_UPDATED.md](tests/UPSTREAM_FAT32_83_COMPARISON_UPDATED.md). Taxas modeladas não são velocidade de cartão físico.

### 12.2 Monólito e agrupamento dos condicionais

- Os seis fragmentos de produção `asyncfatfs_api.inc`, `asyncfatfs_format.inc`, `asyncfatfs_lfn.inc`, `asyncfatfs_journal.inc`, `asyncfatfs_fatjournal.inc` e `asyncfatfs_exfatjournal.inc` foram incorporados e removidos. Os `.inc` de fixtures de teste permanecem válidos.
- Ordem: declarações/configuração comuns → **AsyncFS → LFN → exFAT → journal**. A API de compatibilidade e a formatação ficam ao final da seção AsyncFS, protegidas pelo recurso opcional. Tipos e protótipos comuns podem preceder as seções.
- Hooks privados com `AFATFS_INLINE` encaminham operações para as seções opcionais; macros constantes/no-op cobrem recursos desabilitados. Preservar a ordem de chamadas e seus efeitos, sem criar estado estático.
- O agrupamento mais recente moveu configuração do intervalo de truncamento, classificação NoFatChain e atualização de comprimentos para hooks exFAT. O número de grupos condicionais caiu de **243 para 239**. Na última otimização de CPU, a remoção de um guarda e a inclusão do guarda de inlining mantiveram esse total.
- Atenção à semântica existente: o bookkeeping de conclusão do truncamento atualiza campos exFAT também para volumes FAT em builds com exFAT habilitado. Não acrescentar um teste de tipo de filesystem nessa etapa como se fosse uma refatoração neutra.
- As etapas anteriores agruparam exclusão LFN, publicação de entry sets exFAT, tracking de dados escritos, sync, stat, MBR/GPT, validação de boot e montagem da raiz. Preservar a precedência de partições, rejeições de geometria, progressão por poll e barreiras de publicação.
- Evidências: `tests/build/section-hooks-2/` até `section-hooks-5/`; cada etapa preservou as 48 medições de RAM/pilha, passou nos 11 cenários funcionais e comparou seis listagens PIC24 nativas normalizadas.
- Todas as **295 definições de função**, com 294 nomes únicos por uma definição condicional alternativa, têm documentação verificada por `tests/check_monolith_docs.py`. O verificador reconhece `AFATFS_INLINE` como decoração de declaração. `doc/Doxyfile` foi atualizado; Doxygen não estava instalado, portanto não afirmar que HTML foi gerado.

### 12.3 Limite de pilha e redução nas opções

O adiamento de `afatfs_extendSubdirectory()` libera a cascata de criação/rename/montagem antes de entrar em alocação/cache/journal: a função prepara o estado existente e devolve `AFATFS_OPERATION_IN_PROGRESS`; o poll executa a continuação. Não reintroduzir a chamada imediata à continuação. `afatfs_appendFreeCluster()` incorpora o dispatcher ao chamador, removendo um wrapper na cascata mais profunda.

Pico entre enums normais/curtos e perfis single/multi/query, XC16 2.10, PIC24FJ256GA702, C99, `-Os`:

| Configuração | Antes da redução | Atual |
| --- | ---: | ---: |
| default | 328 | 286 |
| fat_legacy | 324 | 282 |
| fat_only | 330 | 306 |
| fat_lfn | 406 | 338 |
| exfat_only | 444 | 368 |
| exfat_freefile | 474 | **398** |
| lfn_exfat | 462 | 386 |
| lfn_no_journal | 390 | 326 |

- As 48 variantes permanecem estritamente abaixo de 400 bytes; há somente **2 bytes de margem** na pior variante. RAM estática, alinhamento e seções graváveis permaneceram iguais.
- `tests/optional_stack_budget.py` compila a matriz e compara RAM com uma biblioteca baseline completa. `tests/pic24_stack_budget.py` soma conservadoramente crescimento fixo da pilha e retornos; não interpretar esse limite como medição dinâmica em hardware.
- O parser reconhece as duas grafias XC16 de ajuste fixo de SP, inclusive `add #N,w15`, e rejeita crescimento variável. `tests/test_pic24_stack_budget.py` verifica esse comportamento.
- `tests/deferred_directory_stack.c` verifica ausência de I/O inline ao iniciar crescimento, rejeição enquanto busy, progresso via poll, zeragem e conteúdo após remontar.
- Relatório: [OPTIONAL_STACK_BUDGET.md](tests/OPTIONAL_STACK_BUDGET.md). Evidência final mais recente: `tests/build/cpu-cache-index/final-matrix/report.json`.

### 12.4 Alterações do usuário em sdcard.h e correção dos testes

- O usuário confirmou que estava editando `lib/sdcard.h` em paralelo. Preservar essas edições; usar snapshots com o mesmo header antes/depois nas comparações, sem restaurar o arquivo por conta própria.
- Estrutura atual: `sdcardMetadata_t` contém `sdcardCID_t cid` seguido de `uint32_t numBlocks`. Os campos CID incluem `manufacturerID`, `oemID`, `prodName[5]`, dois nibbles de revisão, `prodSerial`, `prodYear` e `crc`.
- É uma estrutura de runtime com alinhamento natural, não o registro de 16 bytes no formato do cartão. Não adicionar `packed` nem usar a ordem dos bitfields como formato de disco.
- `tests/bitfield_layout.c` foi atualizado para os campos aninhados e verifica 256 combinações de revisão, independência dos nibbles, descarte de bits altos e preservação dos campos vizinhos. Verifica alinhamento natural do CID e alinhamento explícito do cache somente quando DMA está ligado.
- `tests/layout_memory_report.py` não afirma mais uma economia histórica fixa para o header antigo. As 24 variantes de layout passaram: 12 execuções host x86/x64 e 12 compilações PIC24 com enums normais/curtos.
- CID/metadados mediram **20/24 bytes no host**, alinhamento 4; **18/22 bytes no PIC24**, alinhamento 2. Evidência: `tests/build/sdcard-header-update/final-layout/report.json`. Esses tamanhos não são tamanhos de registros wire.
- A última otimização de CPU não alterou `sdcard.h`; foi conferida sua igualdade com o snapshot inicial.

### 12.5 Última otimização de CPU, mantida e ainda não commitada

`afatfs_getCacheDescriptorIndexForBuffer()` calcula o deslocamento do ponteiro, valida que está dentro do cache e usa divisão sem sinal por 512. Isso evita o ajuste de arredondamento da divisão com sinal nas configurações que ainda o usavam. A função usa `AFATFS_INLINE` **somente quando `AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT` está definido**.

- A incorporação também na configuração padrão foi avaliada e não mantida, devido a pequenas regressões. Na versão final, helper, cache, `fread`, `fwrite`, poll e prefetch padrão geram os mesmos bytes de código anteriores nos objetos x64 verificados. O tamanho de código padrão x86 também permanece igual.
- Não trocar `discardable = false` por uma leitura/teste do bit antes da atribuição: essa tentativa anterior ficou mais lenta, foi rejeitada pelo usuário e está documentada em `FAT32_READ_PHYSICAL_INLINE_CPU.md`.
- Outras otimizações aceitas e seus contextos estão em `FAT32_READ_CPU*.md`, `FAT32_READ_PREFETCH_CPU.md`, `FAT32_READ_QUERY_FIX.md`, `FAT32_READ_4096_FIX.md`, `FAT32_READ_CURSOR_CPU.md` e `FAT32_READ_PHYSICAL_INLINE_CPU.md`. Não generalizar especializações do pipeline mínimo a todos os perfis sem medir.

Medição final: MSVC x64 `/O2`, FAT32 8.3, clusters de 512 bytes, um/dois arquivos de 1 MiB, blocos de 257/4096 bytes; **21 repetições intercaladas**, 32 passagens de leitura, afinidade fixa e sem compilações concorrentes. Ciclos incluem backend simulado, polling e verificação dos dados. Valores positivos abaixo significam menos CPU:

| Perfil/caso | Redução sobre a versão anterior | Redução sobre upstream com 4 páginas |
| --- | ---: | ---: |
| Multibloco, 1 leitor/257 | 1,16% | 10,87% |
| Multibloco, 2 leitores/257 | 0,93% | 20,27% |
| Multibloco + consulta, 1 leitor/257 | 2,11% | 18,33% |
| Multibloco + consulta, 2 leitores/257 | **2,00%** | **19,08%** |
| Multibloco + consulta, 1 leitor/4096 | 0,45% | 21,32% |
| Multibloco + consulta, 2 leitores/4096 | 0,67% | 18,35% |

- A configuração padrão preserva o código anterior; não atribuir suas variações de tempo a uma otimização nova.
- Na escrita com consulta, a mediana melhorou 2,23% para um escritor/257 e 0,85% para dois/257. Dois escritores/4096 variaram **0,48% para pior**; o relatório não afirma ganho em todos os cenários. Diferenças próximas de zero devem ser tratadas como variação de medição.
- O consumo de CPU de escrita permanece maior que o upstream nessa comparação. O upstream deixa a segunda FAT diferente nesse workload; não comparar seu custo como se oferecesse o mesmo espelhamento. Upstream equalizado usa quatro páginas; o nativo, oito.
- RAM estática antes/depois: **2616 bytes x86**, **2728 bytes x64**, nos três perfis. PIC24 padrão: **2494 bytes com enums normais**, **2474 com enums curtos**, nos três perfis. São totais de armazenamento estático medidos, não somente `sizeof(afatfs_t)`.
- Código com multibloco cresceu **113 bytes x86** e **120 bytes x64**; o padrão não cresceu. Nenhum heap, campo, buffer ou estado adicional foi criado.
- Todas as contagens de I/O/metadados, polling, sessões e paradas multibloco coincidem antes/depois. As duas FATs passaram na auditoria. Nenhuma regra de reserva, publicação, ACK, flush ou journal foi alterada.
- A matriz final de 48 variantes preservou todos os limites de pilha e seções estáticas; pico **398 bytes**. Os 11 cenários funcionais com multibloco+consulta passaram, incluindo falhas de backend, remoção/reinserção, callbacks obsoletos, recuperação, ACKs atrasados, desligamento e crescimento adiado de diretório.

Relatório: [CACHE_INDEX_CPU.md](tests/CACHE_INDEX_CPU.md). Gerador/validador: `tests/cache_index_cpu_report.py`; runner: `tests/run_read_cpu_benchmark.ps1`.

Evidências locais em `tests/build/cpu-cache-index/`:

- `before/lib/`: snapshot completo anterior à última otimização.
- `after/lib/` e `final/lib/`: versão selecionada final; não confundir com `inline/lib/`, experimento intermediário.
- `report.json`: amostras finais, comparações, I/O, recursos, hashes, igualdade de código padrão e matriz PIC24.
- `final-matrix/report.json`: matriz final de 48 variantes com baseline.
- `inline-functional/`: os 11 cenários com multibloco+consulta; o condicionamento final do inline não muda o código desse perfil.
- `final-docs.json`: cobertura Doxygen da fonte final, SHA-256 `a6b7a0ce4d1fbb3bd028cd480375728c99c230040b9cd8b311a91a5434d8a407`.
- `inline-experiment.json` é o experimento anterior com inline também no padrão; não é o resultado selecionado. `matrix/` é uma execução exploratória interrompida, não aprovação da fonte final.

### 12.6 Comandos atuais e cuidados para retomar

```powershell
python tests/check_monolith_docs.py --output tests/build/nova-rodada/docs.json
python tests/optional_stack_budget.py --library lib --baseline <baseline-lib> --output tests/build/nova-rodada/stack
./tests/run_multiread_tests.ps1 -OutputDirectory tests/build/nova-rodada/functional -ReadSessionQuery
python tests/cache_index_cpu_report.py --directory tests/build/cpu-cache-index --repetitions 21 --output tests/build/cpu-cache-index/report.json --markdown tests/CACHE_INDEX_CPU.md
```

- `run_multiread_tests.ps1` cobre `default`, `fat_legacy`, `fat_only`, `fat_lfn`, `exfat_only`, `exfat_freefile`, `lfn_exfat`, `focused`, `backend`, `integration` e `deferred_directory`. Não depende dos VHDs perdidos.
- O gerador de CPU pressupõe snapshots, objetos, executáveis e trials já preparados; não executá-lo sobre fontes novas para renovar uma aprovação antiga. O runner escolhe `before/lib`, `after/lib` e `upstream/lib` conforme a variante; conferir os diretórios e flags antes de medir.
- `tests/build/` é ignorado pelo Git. Muitos artefatos antigos foram perdidos na troca de branch; ausência de VHD/imagem/dependência não comprova regressão da biblioteca. Informar a limitação e usar regressões sintéticas pertinentes; não alegar execução nativa que não ocorreu.
- Não rerodar scripts exploratórios de agrupamento em `tests/build/` sobre a fonte atual: alguns leem um baseline congelado e sobrescrevem `asyncfatfs.c`, apagando melhorias posteriores.
- Não há sessão de teste em execução nem implementação deixada pendente ao atualizar este checkpoint. Não criar commit, reiniciar benchmarks ou alterar configuração sem uma nova tarefa que os justifique.

## 13. Atualização de 18/11/2026: cópias localizadas do espelho FAT

Esta seção atualiza o ponto de retomada acima. A configuração atual usa **oito setores de cache**, por solicitação do usuário. A referência local upstream/master continua `60c5f3580152410cb08342942cbbe3a30a449c24`; não houve fetch nesta tarefa.

- Implementação mantida em `afatfs_FATWriteSector()` e `afatfs_FATSetNextCluster()`: no pipeline nativo FAT32, o helper devolve por um bool local se o espelho já estava DIRTY antes da marcação. Nesse caso, o chamador copia quatro bytes por entrada. Espelho novo/limpo/em outro estado recebe somente a cópia integral atualizada de 512 bytes. FAT16, builds sem pipeline e preenchimentos de várias entradas conservam cópia integral. Nenhum novo campo ou buffer estático; chamadores que copiam a página inteira passam NULL no novo parâmetro interno `mirrorDirty`.
- A validade do caminho DIRTY depende do contrato dos chamadores: adquirir o par antes de alterar e atualizar ambas as páginas antes de devolver controle. Não introduzir um chamador que altere apenas a primária e depois suponha que DIRTY significa igualdade. ACKs, marcação DIRTY, reserva única e barreiras EOC → vínculo → diretório foram preservados.
- Relatório completo e reprodutível: [FAT_ENTRY_COPY_CPU.md](tests/FAT_ENTRY_COPY_CPU.md), JSON ao lado e `tests/fat_entry_copy_cpu_report.py`. Evidências finais congeladas: **`tests/build/fat-entry-copy-result/`**; `before/lib` é o baseline anterior desta tarefa, `normal/lib` é a fonte final sem hora fixada, `after/lib` tem somente a hora de benchmark controlada. `fat-entry-copy-cpu`, `fat-entry-copy-final` e `fat-entry-copy-selected` são tentativas anteriores, não a aprovação da fonte final. Upstream canônico foi conferido pelo blob `81a8b854a345e329e0c289798ff6d82ef31dcf4f`.
- CPU MSVC x64 /O2, 21 repetições pareadas, arquivos de 1 MiB por escritor, clusters 512, cache oito: padrão por bloco teve reduções **−0,50% (1/257), −2,11% (2/257), +0,13% (1/4096), +0,07% (2/4096)**. Valores negativos significam mais CPU. Pedidos intercalados ficaram próximos de zero: −0,45%, +0,69%, +0,05%, −0,20%, na mesma ordem. Leitura multibloco habilitada: ganhos de escrita 1,92–3,21%; com consulta, 1,84–3,37%. Não anunciar ganho universal: o controle de leitura padrão de um leitor/4096 regrediu **10,14%**, apesar de fread/cache/poll sem mudança de algoritmo; a causa não foi isolada. Os arranjos anteriores também apresentaram essa regressão e estão documentados no relatório.
- I/O, metadados, polling, sessões e tempo virtual foram idênticos antes/depois em todos os casos. O custo adicional ao upstream permanece: no padrão por bloco, +78,35%, +94,02%, +76,20%, +84,74% de CPU, respectivamente. O upstream não atualiza a segunda FAT nesse workload e não oferece as mesmas barreiras.
- RAM estática padrão antes/depois: x64 4872 B, x86 4760 B, PIC24 normal 4614 B, short 4586 B. RAM não aumentou. Pilha padrão PIC24 normal 270 → **274 B**, short 264 → 268 B; 48 variantes com oito setores passaram com seções estáticas iguais e máximo opcional de **378 B**, abaixo de 400. Código x64 padrão 36190 → 36261 B (+71). Upstream PIC24 foi recompilado nesta rodada: RAM 4544/4520 B e pilha 200/202 B.
- Passaram 11 cenários funcionais, 10 checks de alocação, auditorias das duas FATs e cobertura Doxygen (295 definições). O teste `fat32_cache_mirror.c` conta cópias e verifica vizinhos, bits altos, espelho novo envenenado, reparação limpa e falha de ACK seguida de atualização localizada/retry. Testes usam mídia sintética; não dependem dos discos perdidos. `lib/sdcard.h` não foi editado.
- O [modelo etapa por etapa](tests/CURRENT_WRITE_EXECUTION_COMPARISON.md) conserva o snapshot anterior e agora aponta para a implementação medida. Outras hipóteses daquele modelo continuam sem implementação nesta tarefa.

## 14. Correção posterior: padrão restaurado e cópia localizada preservada no multibloco

Esta seção substitui a seleção de perfil da seção 13. As regressões foram corrigidas por especialização de compilação, mantendo **cache oito**:

- Sem `AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT`, a aquisição FAT volta à assinatura interna original e `FATSetNextCluster` copia o setor inteiro. O parâmetro `mirrorDirty` e o bool local não são compilados. Com leitura multibloco, com/sem consulta, permanece a cópia localizada da seção 13. `AFATFS_FAT_MIRROR_STATE(parameter)` controla somente o argumento/parâmetro interno; não é uma nova opção pública e não acrescenta estado.
- Evidência forte: todas as seções de instruções dos objetos x64 (biblioteca e harness) **e dos executáveis ligados** são idênticas entre padrão final/original anterior à cópia localizada, multibloco final/versão recebida e multibloco+consulta final/versão recebida. No x86, o objeto padrão também é idêntico ao original. Assim a correção não depende de escolher amostras favoráveis ou adicionar padding; preserva exatamente o código multibloco já aceito.
- [MIRROR_PROFILE_FIX.md](tests/MIRROR_PROFILE_FIX.md), JSON e `tests/mirror_profile_fix_report.py` registram a rodada final: 21 repetições pareadas MSVC x64 /O2, 1 MiB por arquivo, 32 passagens de leitura, um/dois escritores e 257/4096. Redução frente à recebida: **9,06% na leitura 1/4096** e **1,25% na escrita 2/257**. Frente ao original, +0,02% e −0,24%, respectivamente, com código idêntico (variação de medição). Não misturar os percentuais históricos 10,14%/2,11% com essas amostras.
- RAM estática preservada em todos os ABIs medidos e nas 48 variantes PIC24. Padrão PIC24: pilha normal 274 → **270 B**, short 268 → **264 B**; máximo opcional **378 B**. Código x64 padrão 36261 → 36190 B, multibloco 37680 → 37680 B, consulta 37441 → 37441 B. Nenhum buffer, reserva ou garantia de persistência mudou.
- I/O, metadados, polling, sessões e tempo virtual idênticos à recebida e ao original em todos os perfis/casos. Duas FATs e ordenação EOC/vínculo/diretório permanecem. Upstream local ainda `60c5f3580152410cb08342942cbbe3a30a449c24`; diferenças de I/O frente a ele continuam documentadas.
- Passaram **11 testes funcionais + 12 de alocação**, incluindo `mirror_cache`, `mirror_cache_multi`, `mirror_cache_query`; cada um verifica a estratégia correspondente, vizinhos/bits altos, ACK falho/retry e primária expulsa com espelho sujo. O runner de alocação agora cobre os três perfis. A validação Doxygen reconhece o parâmetro interno condicionado e mantém 295 definições.
- Evidências finais: **`tests/build/mirror-profile-fix/`**. `before/lib` é a recebida regredida, `reference/lib` é a anterior à cópia localizada, `normal/lib` é a fonte final sem hora fixada, `after/lib` é a cópia do benchmark. `matrix-work/tests/config_common.h` usa oito setores. O runner de CPU aceita agora `reference_no_query` para comparar o perfil correspondente corretamente. `lib/sdcard.h` não foi editado.
- Não reativar a cópia localizada no padrão sem novo A/B. A causa microarquitetural exata da regressão de leitura não foi isolada; o caminho corrigido foi restaurado e verificado por identidade de código, sem alteração do algoritmo de leitura.

## 15. CPU da escrita: continuação direta de APPEND no escalonador

Última implementação mantida: em `afatfs_fileOperationsPoll()`, somente no pipeline nativo, com cache >= 8 e sem leitura multibloco, arquivos ativos em `APPEND_FREE_CLUSTER` com tipo diferente de NONE chamam diretamente `afatfs_appendRegularFreeClusterContinue()`. O dispatcher genérico permanece para os demais casos, inclusive tipo inválido. Arquivos sem operação continuam com uma única checagem. Ordem dos handles, quantidade de passagens, fases de alocação, reserva única, duas FATs e barreiras não mudaram. O cache padrão continua em **oito setores**.

- Relatório final: [WRITE_DISPATCH_CPU.md](tests/WRITE_DISPATCH_CPU.md), JSON e `tests/write_dispatch_cpu_report.py`. Evidências selecionadas: **`tests/build/cache8-append-dispatch/`**; before/lib é a seção 14, normal/lib é a fonte final sem hora fixada, after/lib tem somente a hora controlada do benchmark. Upstream/master local foi novamente conferido: `60c5f3580152410cb08342942cbbe3a30a449c24`, blob C `81a8b854a345e329e0c289798ff6d82ef31dcf4f`.
- MSVC x64 /O2, 21 repetições pareadas, 1 MiB por arquivo, clusters 512, cache oito, afinidade fixa e sem compilações concorrentes nas medições. Redução de CPU padrão por bloco: **3,20% (1/257), 3,59% (2/257), 3,45% (1/4096), 3,09% (2/4096)**. CPU adicional ao upstream caiu, na mesma ordem: 81,50 → 76,58%; 94,15 → 86,83%; 74,15 → 68,30%; 86,02 → 79,95%. A diferença restante continua grande; não atribuir todo o custo à segunda FAT.
- Pedidos intercalados: reduções de escrita **5,48%, 9,86%, 4,57%, 2,46%**, na mesma ordem. Não misturar amostras dos dois escalonamentos nem percentuais de rodadas anteriores.
- Os objetos x64 da biblioteca/harness e os executáveis ligados multibloco, com/sem consulta, têm todas as instruções **idênticas antes/depois**. Os ganhos anteriores desses perfis foram preservados; variações pequenas das novas medições entre pares idênticos são controles de ruído, não ganhos/regressões novos.
- Controles de leitura padrão por bloco: +1,70%, +0,59%, +0,99%, −0,01% de redução (mesma ordem de blocos/arquivos). Com pedidos intercalados: +1,00%, +2,11%, +0,67%, +1,67%. Não houve retorno da grande regressão em um leitor/4096.
- I/O, metadados, polling, sessões e tempo virtual idênticos antes/depois em todos os casos/perfis. A auditoria das duas FATs passou. RAM estática preservada: x64 4872 B, x86 4760 B, PIC24 normal 4614 B/short 4586 B. Código padrão x64 **36190 → 36036 B** (−154), x86 **26412 → 26244 B** (−168). Código multibloco inalterado.
- Passaram **11 cenários funcionais + 12 checks de alocação** e as **48 variantes PIC24** com seções estáticas iguais. Pilha padrão permanece 270 B normal/264 B short; máximo opcional **378 B**, abaixo de 400. Doxygen permanece 295 definições. `lib/sdcard.h` não foi editado.
- Tentativas descartadas: `tests/build/cache8-flush-filter/` e `tests/build/cache8-flush-eligibility/`. Os dois filtros reduziam CPU de escrita, mas regrediam leitura 1/4096 em 12,45% e 13,53%. **Não são a fonte mantida**; suas amostras estão separadas no JSON final. Não restaurar os filtros a partir desses snapshots.
- A identidade do código padrão com o original mencionada na seção 14 passa a ser histórica: a seção 15 modifica o padrão com o atalho medido. A identidade dos dois perfis multibloco com a versão aceita permanece válida.

## 16. Relação I/O × CPU e amplificação no harness intercalado

Investigação apenas de medição/testes: **a biblioteca da seção 15 não foi modificada**. [IO_CPU_RELATION.md](tests/IO_CPU_RELATION.md), JSON e `tests/io_cpu_relation_report.py` contêm 21 repetições pareadas novas e auditorias separadas de metadados/trabalho do harness. Evidências: **`tests/build/io-cpu-relation/`**. Cache oito, FAT32 8.3, cluster 512, um/dois arquivos de 1 MiB, upstream local `60c5f3580152410cb08342942cbbe3a30a449c24`.

- O excesso de transferências de escrita é **inteiramente metadados**: dados de usuário são escritos uma vez (2048/4096 setores) nas duas implementações. Serial 2/4096: atual FAT primária 4160, espelho 4160, diretório 4; upstream 746, 0, 715. Excesso total 6863 setores = +3414 primária +4160 espelho −711 diretório. Portanto não é limitado à segunda FAT: a frequência de publicação/ordenação da primária também pesa.
- No serial, razões de I/O total atual/upstream: 1,494× (1/257), 1,502× (2/257), 2,196× (1/4096), 2,221× (2/4096). Razões de CPU: 1,774×, 1,858×, 1,699×, 1,810×. Não há proporcionalidade única. Com 2/257 há 8158 polls sem conclusão de setor na atual versus 32 no upstream e 20603 tentativas fwrite versus 8323. Esses polls não são espera física medida: CPU_ONLY usa um poll por transferência no simulador.
- **Achado importante do benchmark:** `COMPARISON_INTERLEAVED_REQUESTS` histórico regenera os bytes ainda não aceitos em cada retry. Com 2/4096, a atual gera 27,93× o payload e upstream 11,74×. Esse trabalho da aplicação de teste entra nos ciclos medidos e amplifica a diferença entre bibliotecas que exigem números diferentes de tentativas. Não tratar CPU intercalada histórica como CPU exclusiva da biblioteca.
- Novo modo diagnóstico opcional `COMPARISON_PREPARED_INTERLEAVED`: prepara padrões distintos por arquivo uma vez por pedido, em buffers locais do harness Windows. No A/B, **I/O, polling, tentativas fwrite e chamadas pump permanecem exatamente iguais por versão**; geração cai para 1× em ambas. Em 2/4096 intercalado, CPU extra ao upstream caiu de **122,42% para 32,63%**, sem melhoria no código FAT. Diferença absoluta dos ciclos/B: 22,4812 → 1,4887 (93,38% menor). Esse delta inclui a mudança de preparação/cache dos buffers; não é uma atribuição de tempos exclusivos de funções.
- Não misturar números de destinos serial/intercalado: no intercalado 2/4096, atual R=34, W dados=4096, FAT1=4128, FAT2=4128, diretório=291, polls=12937; upstream R=453, dados=4096, FAT1=831, FAT2=0, diretório=707, polls=6410. Os modos histórico e preparado têm esses mesmos contadores. A atual anuncia 288 sequências/576 setores multibloco de escrita nesse caso; contagens de setores não equivalem a comandos físicos ou programas NAND.
- Auditoria separada `COMPARISON_METADATA_TRACE` + `COMPARISON_CPU_WORK_AUDIT`: META/META_READ/FAT_NOOP/WORK contam os destinos, bytes gerados, tentativas fwrite e chamadas pump. **Timings instrumentados foram descartados**. A CPU veio de executáveis sem esses contadores. Foram verificadas igualdade de I/O entre auditoria e CPU, integridade completa dos arquivos e igualdade das FATs atuais. Nenhuma imagem FAT atual byte-idêntica foi regravada nos casos auditados; isso não prova optimalidade nem abrange no-ops de diretório.
- Runner: `tests/run_io_cpu_relation.ps1`. Constrói CPU/auditoria para serial, intercalado histórico e intercalado preparado, nas duas versões; CPU roda após terminar todas as compilações. O JSON registra hashes de fontes, harness original/modificado, instruções dos objetos e todos os dados. Os objetos de biblioteca são idênticos entre modos/CPU/auditoria por versão. RAM/cache/pilha/persistência de produção permanecem os da seção 15; a mesma fonte/headers e a matriz anterior <400 B foram conferidos, não recompilados.
- `tests/upstream_comparison.c` ganhou somente macros opcionais de diagnóstico, desligadas por padrão. O modo histórico continua disponível para reproduzir estudos anteriores. Os três buffers de 4096 bytes e contadores novos pertencem ao **harness**, não à biblioteca ou ao teto de pilha interna PIC24. `lib/sdcard.h` não foi editado.
- Para futuras comparações de CPU entre códigos com I/O/retries diferentes, usar também buffers preparados e registrar trabalho do harness. Não estimar uma fração exata de CPU por I/O dividindo ciclos por setores/polls: isso mistura cópia, preparação de dados, retomadas, callbacks e garantias diferentes de persistência. O A/B serial da seção 15 continua válido; a nova investigação qualifica sobretudo a interpretação da CPU intercalada entre bibliotecas distintas.

## 17. Auditoria do excesso de gravações de metadados

Investigação diagnóstica sem modificar a biblioteca da seção 15. Relatório [METADATA_WRITE_AUDIT.md](tests/METADATA_WRITE_AUDIT.md), JSON e scripts `tests/metadata_write_audit.py`, `tests/run_metadata_write_audit.ps1`, `tests/metadata_write_report.py`. Evidências congeladas: **`tests/build/metadata-write-audit/`**. Cache oito, FAT32 8.3, clusters 512, mesmo upstream local da seção 16.

- Excesso confirmado: **+98,42% a +476,85%** nos dois ritmos e dois perfis de latência. Serial perfil 0, atual/upstream: 1/257 = 4133/2083; 2/257 = 8326/4165; 1/4096 = 4133/741; 2/4096 = 8324/1461 metadados. Distinguir razão e aumento: 557,76% do upstream significa **+457,76%**, não +557,76%.
- Custo dominante: confirmação FAT por cluster, além do espelho. Serial 2/4096: primária 4160 imagens contra 746 upstream, espelho outras 4160; cerca de **1,97 entradas alteradas por imagem primária** contra **6,90**. Dados idênticos: 2048 setores por arquivo. Preservar EOC confirmado nas duas FATs antes do vínculo e vínculo confirmado antes do diretório. Próximo alvo: compartilhar imagens de alocações já solicitadas dentro da mesma fase de durabilidade, sem ampliar a reserva.
- Ineficiência localizada na espera pela reserva em FIND_FREESPACE: `afatfs_flushCache()` sem filtro envia diretório antigo. Intercalado 2/4096: **288 gravações** vieram desse caminho, todas com idade inferior a 64; outras três são fechamento/sincronização. Não eram pressão do cache nem expiração normal.
- Variante diagnóstica **reserve_wait** troca apenas esse flush por `afatfs_flushAllocationCache(file)`, preservando a espera das duas FATs em IN_SYNC. Nos dois perfis, intercalado 2/4096: diretório **291 → 3**, metadados **8547 → 8259 (−3,37%)**, FAT1/FAT2 inalteradas em 4128 cada. Intercalado 2/257: 8324 → 8323. Casos seriais inalterados. **Não promovida à produção**: ainda faltam avaliações de CPU, pilha, fragmentação, pressão e falhas. Timings instrumentados descartados.
- Controles: remover reserva piora serial 1/4096 de 4133 para 8193 metadados; remover agrupamento piora intercalado 2/257 e não supera reserve_wait em 2/4096; flush filtrado adicional sob pressão de dados não muda contagens.
- Passaram **96 cenários de escrita**: seis variantes, dois ritmos, duas latências, um/dois escritores e 257/4096, com releitura integral, conteúdo, igualdade das FATs atuais e auditoria EOC/vínculo/diretório por gravação. Submissões/conclusões reconciliadas; bases perfil 0 reproduzem a seção 16. Não houve injeção de falhas. Produção, RAM e pilha permanecem inalteradas; `lib/sdcard.h` não foi editado.

## 18. Flush filtrado da reserva incorporado e validado

Implementado em `afatfs_appendRegularFreeClusterContinue()`: a espera de páginas FAT da reserva em FIND_FREESPACE chama `afatfs_flushAllocationCache(file)`. Mantém o diretório antigo buffered em FAT32/cluster de um setor; demais geometrias preservam o fallback. A checagem IN_SYNC de ambas as FATs, reserva única, EOC confirmado → vínculo confirmado → diretório, cache oito e organização das seções permanecem. Esta seção promove a variante diagnóstica da seção 17 à biblioteca; não modifica os demais experimentos.

- Relatório [RESERVE_FLUSH.md](tests/RESERVE_FLUSH.md), JSON, preparador/validador `tests/reserve_flush_report.py` e runner `tests/run_reserve_flush.ps1`. Evidências: **`tests/build/reserve-flush/`**; before/lib é a seção 15, normal/lib é a fonte final de produção; cpu-before-source/lib e cpu-after-source/lib controlam apenas a hora FAT do benchmark. Upstream/master local conferido: `60c5f3580152410cb08342942cbbe3a30a449c24`.
- MSVC x64 /O2, **21 repetições pareadas**, serial e intercalado com buffers preparados uma vez por pedido, perfis padrão/multibloco/multibloco+consulta, um/dois escritores e 257/4096. CPU sem compilações concorrentes e sem contadores instrumentados; auditorias de I/O/ordenação separadas conferem os contadores das medições. Não interpretar como tempos PIC24/cartão físico.
- Caso padrão intercalado 2/4096: CPU **6,0248 → 5,8637 ciclos/B**, redução pareada **2,71%**; upstream 4,5621, diferença restante **+28,53%**. Metadados **8547 → 8259 (−3,37%)**, diretório **291 → 3**, FAT1/FAT2 **4128 cada, inalteradas**. Escritas totais **12643 → 12355 (−2,28%)**, leituras 34; upstream R=453/W=5634/metadados=1538. Multibloco preservado em 288 sessões/576 setores. Intercalado 2/257 economiza um metadado; demais casos mantêm I/O.
- Não há ganho geral de CPU: casos seriais mantêm I/O e apresentam variações/aumentos, chegando a **+3,60%** em um escritor/4096 com consulta multibloco. Não ocultar esse custo nem atribuir a esta mudança uma redução da amplificação dominante da FAT. No intercalado 2/4096, reduções pareadas opcionais: 1,58% sem consulta e 4,02% com consulta.
- RAM estática idêntica antes/depois: x64 **4872 B** (upstream 4784), x86 **4760 B** (upstream 4668), PIC24 normal **4614 B** (upstream 4544), short **4586 B** (upstream 4520). Código x64 padrão 36036 → 36060 B; x86 26244 B inalterado. As **48 configurações/perfis/ABIs PIC24** passaram, sem aumento de RAM; pilha padrão **270/264 B**, máximo opcional **378 B**, incluindo chamadas internas/PCs e excluindo backend/aplicação/libc conforme o orçamento existente.
- Passaram **15 testes de alocação/cache**, sete suítes funcionais e 11 suítes em cada perfil multibloco (com/sem consulta). Incluem quase cheio, três escritores, agrupamento, FAT primária/espelho, ACKs falhos/retry e releitura. Novo `tests/fat32_reserve_wait.c`, incorporado ao runner de alocação nos três perfis, verifica diretório sujo excluído, reserva retida sob falhas de envio/ACK de cada FAT e conteúdo após remount; força alocação não contígua e mudança de página FAT. Controle negativo na biblioteca anterior falha na asserção do diretório sujo. Não houve campanha física de corte de energia. Doxygen: 295 definições; `lib/sdcard.h` não foi editado.

## 19. Origem dos metadados no poll e controles de escalonamento

Investigação sem alterar a biblioteca da seção 18. Relatório [POLL_METADATA.md](tests/POLL_METADATA.md), JSON e preparador/validador `tests/poll_metadata_audit.py`. Evidências: **`tests/build/poll-metadata/`**; runner reutilizado `tests/run_metadata_write_audit.ps1`, passando as variantes do manifest. Cache oito, FAT32 8.3, cluster 512, dois ritmos, duas latências, um/dois escritores e 257/4096. Upstream local conferido novamente: `60c5f3580152410cb08342942cbbe3a30a449c24`.

- No padrão nativo, `afatfs_poll()` executa `afatfs_flushCacheFiltered(0, true)`; as chamadas literais a `afatfs_flushCache()` no poll pertencem a ramos condicionais de journal/sem pipeline. `afatfs_fileOperationsPoll()` avança as máquinas de estados e dispara os flushes de barreiras, sem regravar uma página apenas por ser chamado.
- Omissão do flush anterior ou de ambos os flushes de background **não elimina nenhuma gravação das FATs nos 16 casos**; as barreiras assumem os envios. Poupa zero/um setor de diretório. Não adotar: esses controles alteram o escoamento em idle e não resolvem o custo dominante.
- A segunda passagem condicional melhora a coalescência: intercalado 2/4096, FAT1 **4128 → 4160** e metadados **8259 → 8291** ao removê-la; FAT2 permanece 4128. Histograma: 32 imagens de duas entradas viram 64 imagens de uma entrada, sem no-ops. Intercalado 2/257 adiciona um metadado. Segunda passagem incondicional ou três passagens nesse ponto mantêm exatamente o I/O da base nos 16 casos.
- Atribuição base intercalada 2/4096: FAT1/FAT2 **256/288** submetidas pelo background anterior, **3839/3840** na primeira passagem, **33/0** na segunda. Diretório: duas submissões na primeira passagem e uma fora do poll. Atribuição identifica o emissor; o A/B demonstra que suprimir background transfere esses envios às barreiras, sem poupar FAT. Os caminhos inflight e background final não emitiram metadados na base desses workloads; não concluir que são globalmente dispensáveis.
- Candidata **sync_filter_defer**, apenas nas cópias: filtra o diretório antigo da drenagem de dados/FAT na fase 1 de fsync/close e permite deferi-lo no background nas fases 0/1, preservando idade máxima 64 e o flush/ACK explícito da fase final. Reduz **um setor de diretório em cada um dos 16 casos** (0,012–0,024% dos metadados), FATs inalteradas. Filtro da fase 1 sozinho ajuda apenas parte dos casos. Não promovida: faltam CPU, pilha e falhas específicas; o ganho é pequeno.
- Controle **adopt_pending_reservation** transfere o EOC ainda pendente ao escritor que já solicitou alocação, consumindo a reserva global e passando por NEW_CLUSTER_DURABLE antes do vínculo. Não melhorou: intercalado 2/4096 resulta também em 8291 metadados. Não adotar essa variante.
- Passaram **160 cenários de escrita e 160 releituras**, dez variantes incluindo upstream; as atuais auditam EOC/vínculo/diretório por gravação e igualdade das duas FATs. Histogramas e submissões/conclusões reconciliados; base perfil 0 reproduz a seção 18. Sem injeção de falhas nesta rodada; timings instrumentados descartados. RAM/cache/pilha/persistência de produção permanecem da seção 18 (máximo interno 378 B), sem recompilar a matriz inalterada. `lib/sdcard.h` não foi editado.
- O próximo alvo dominante continua sendo compartilhar alterações **já solicitadas dentro da mesma fase de durabilidade**, preservando EOC confirmado nas duas FATs antes do vínculo e vínculo confirmado antes do diretório. Aumentar passagens do escalonador, isoladamente, não amplia esse agrupamento nos casos medidos.

## 20. Razão quantitativa da amplificação FAT e controle de cacheTimer

Usuário pediu adiar a candidata de fechamento e focar somente na contagem de metadados. **Nenhuma mudança na biblioteca nem medição de CPU nesta rodada.** Relatório [FAT_METADATA_TRANSITIONS.md](tests/FAT_METADATA_TRANSITIONS.md), JSON e `tests/fat_transition_audit.py`; evidências **`tests/build/fat-transition/`**. Cache oito, FAT32 8.3, cluster 512; mesmo upstream local `60c5f3580152410cb08342942cbbe3a30a449c24`.

- A causa mensurada além da FAT2 é a publicação por cluster com confirmação de EOC e vínculo em fases separadas, enquanto upstream acumula alterações no cache antes de concluir a operação. Serial 1/4096: atual **2065 FAT1**, upstream **378**; as mesmas quantidades de dados. Atual registra **2049 FREE→EOC +2047 EOC→LINK +1 EOC→FREE =4097 transições**, média **1,984 entradas/imagem**. Upstream: **362 FREE→EOC +1686 FREE→LINK +361 EOC→LINK =2409**, média **6,373**. As 1686 entradas FREE→LINK não passaram por um EOC persistido em imagem separada naquele modelo de backend.
- O total atual fecha por fase: **17 imagens NEW_CLUSTER_DURABLE +2047 FAT_DURABLE +1 CLOSE =2065**. Os 17 envios EOC ocorrem uma vez em cada um dos 17 setores FAT usados: inicial e 16 mudanças de página. Com 2048 clusters de payload, são **2048 +16 mudanças de página +1 limpeza da reserva**. Igual com pedidos 257/4096. Traço inicial: EOC(3)/EOC(4) antes de bytes aceitos; após 512 bytes, vínculo 3→4 e EOC(5); após 1024, vínculo 4→5 e EOC(6). Upstream aceita 3072 bytes antes da primeira conclusão FAT1, cuja imagem já contém 3→4→5→6→7→8→9→EOC.
- Serial 2/4096: excesso de **6863 setores =+3414 FAT1 +4160 FAT2 −711 diretório**. `pre_background` envia 8257/8324 metadados na base, mas omiti-lo mantém as **8320 gravações FAT (4160 por cópia)**, transferidas para `first_operations`. Portanto é o emissor frequente de um trabalho imposto pelas barreiras; sua frequência isolada não explica a quantidade adicional. Não atribuir fração de CPU a esse ponto com estes contadores.
- Controle do avanço no upstream, sem alterar um byte da biblioteca: wrapper limita a chamada efetiva a 512 bytes e preserva os pedidos externos/pump. Em serial 1/4096, FAT1 **378 →450**, ainda distante de 2065; em 2/4096, **746 →929**, contra 4160. Tamanho de retorno isolado não reproduz as barreiras ACK da atual. Nenhuma proposta de remover essas barreiras foi adotada.
- Sobre a pergunta do usuário: `afatfs_releaseIdleReservation()` incrementa `cacheTimer` em cada poll que entra no caminho de avanço, inclusive sem acesso ao cache. `cacheTimer − accessTimestamp >=64` retira a deferral de background **do diretório**: permite o envio, não obriga imediatamente; não é o critério de elegibilidade das FATs. Acessos reais e marcação de páginas também incrementam esse relógio lógico. O mesmo relógio participa da decisão de devolver a reserva em idle.
- Quatro controles separados (base, sem incremento por poll, sem liberação em idle mantendo incremento, sem ambos): **FAT1/FAT2/diretório e todas as categorias de transição permanecem idênticas nos 16 cenários contínuos**. Não explica o excesso desses workloads. Não extrapolar a workloads com pausas: envelhecimento do diretório e devolução da reserva podem provocar I/O em idle, ainda não quantificado nesta rodada.
- Controle adicional do backend: histórico amostra o buffer na conclusão; alternativa congela seus 512 bytes na submissão, mantendo ponteiro original no callback. **As contagens de FAT1/FAT2/diretório são iguais nos 48 casos desse controle**. Na atual não houve mutação de buffer em trânsito; upstream teve mutações FAT/diretório, alterando algumas categorias de transição entre modelos. Números destacados de transições em 1/4096 perfil 0 são idênticos nos dois modelos. JSON preserva ambos; não usar transições de um modelo como se fossem universalmente as do driver físico.
- Passaram **176 cenários de escrita e 176 releituras**: 48 de transições/base/sem background/upstream, 64 controles de timer, 16 de retorno upstream limitado e 48 de captura na submissão. Conteúdo integral conferido; variantes atuais auditam EOC/vínculo/diretório por gravação e igualdade das FATs. Categorias de transição reconciliadas com histogramas e contagens de imagens; payload aceito confere com fwrite. Sem injeção de falhas ou CPU. Produção e orçamento interno máximo **378 B** permanecem da seção 18; `lib/sdcard.h` não foi editado. Candidata de fechamento permanece adiada, conforme solicitado.

## 21. Agrupamento com ambas as páginas FAT sujas incorporado

Implementado e validado em `afatfs_appendRegularFreeClusterContinue()`, sem mudar estruturas: se a reserva suja bloqueia outro escritor que já solicitou alocação, o novo caminho preserva a reserva global e procura um cluster livre para esse pedido. Exige outro APPEND elegível, nenhum flush em andamento e **ambas as páginas da reserva em DIRTY**, verificado por `afatfs_reservationPairDirty()`. Se uma cópia está limpa/ausente/em trânsito, conserva a espera anterior. O novo alvo continua passando por NEW_CLUSTER_DURABLE antes do vínculo e FAT_DURABLE antes do diretório. Continua um alvo pendente por escritor e **uma reserva especulativa global**.

- Relatório [FAT_DIRTY_GROUPING.md](tests/FAT_DIRTY_GROUPING.md), JSON e validador/preparador `tests/fat_dirty_grouping_report.py`. Evidências **`tests/build/fat-batch/`**: raw/base/lib é a seção 20; raw/fresh_pair_dirty/lib é a candidata inicial; normal/lib é a produção final. Fonte final inclui Doxygen e **fallback para a reserva ao esgotar a busca alternativa**, evitando falso ENOSPC no fim da FAT. Falhas fatais mantêm o tratamento anterior.
- Comparação de metadados, clusters 512, cache oito, arquivos de 1 MiB, dois ritmos e duas latências, um/dois/três escritores. Ganhos em pedidos intercalados: **2/4096: 8259 →8193 (−0,799%)**, FAT1/FAT2 **4128 →4095 cada**; **3/257: 12356 →12258 (−0,793%)**, FAT1/FAT2 **6176 →6127 cada**; **3/4096: 11844 →11740 (−0,878%)**, FAT1/FAT2 **5920 →5868 cada**. Diretório inalterado. Demais cenários mantêm contagem, inclusive todos os seriais e 2/257. Não houve medição ou alegação de ganho de CPU; a grande diferença para upstream continua.
- A fonte final, com fallback, repetiu **48 cenários de escrita e 48 releituras antes/depois** (24 configurações × duas versões), conferindo conteúdo, ambas as FATs, EOC/vínculo/diretório por gravação e histogramas. Outros **48 cenários de escrita/releitura** congelam o buffer na submissão: mesmo I/O e **zero mutações em trânsito**. Resultados finais em adopted/adopted-three e adopted-snapshot/adopted-snapshot-three; controles anteriores permanecem separados.
- RAM estática inalterada: **x64 4872 B; x86 4760 B; PIC24 normal 4614 B, short 4586 B**. A matriz **matrix-production**, recompilada da fonte final, passou nas **48 configurações/perfis/ABIs**: pilha padrão **270/264 B**, multibloco **274/268 B**, máximo opcional **378 B**, incluindo chamadas internas/PCs e excluindo backend/aplicação/libc conforme orçamento existente. Nenhum campo, array ou buffer novo de produção. Cache padrão continua oito; `lib/sdcard.h` não foi editado.
- Passaram **18 testes de alocação/cache** na produção e sete suítes funcionais padrão/opcionais. Novo `tests/fat32_fresh_pair.c`, integrado ao runner de alocação em padrão/multibloco/consulta, verifica espera durante I/O e após ACK de apenas uma cópia; reserva única preservada; EOCs próprios dos pedidos agrupados; três ACKs falhos com escrita parcial/retry em cada FAT; vínculos/diretório retidos; fallback de busca esgotada sem falso disco cheio; conteúdo de oito arquivos após remontagem e limpeza da reserva. Doxygen: **296 definições**, 295 nomes únicos. Não equivale a uma prova de todos os cortes físicos.
- A investigação rejeitou lotes de vários clusters por escritor: embora até oito clusters por pedido reduzissem 2/4096 intercalado de 8259 para 1985 metadados, os testes `fat32_batch_cut.c`/`run_fat_batch_cuts.ps1` mostraram até **16 EOCs sem vínculo** após corte e reboot com dois escritores. Não incorporados. `fat32_pending_bound.c` demonstra, com fragmentação válida e chamadas públicas, o limite original alcançável de **um EOC por escritor + uma reserva** (3 para dois escritores, 4 para três), preservado pela candidata adotada. Os protótipos experimentais e seus geradores permanecem fora da biblioteca.
- A regra de manter **as duas páginas DIRTY** é essencial: permitir acréscimos quando apenas a FAT2 estava suja causou regravações adicionais da FAT1. Adiar barreiras/priorizar passagens, isoladamente, não trouxe ganho nos casos principais. A candidata de fechamento da seção 19 continua adiada.

## 22. Lotes solicitados incorporados com recuperação de volume sujo

Por solicitação explícita do usuário, o protótipo histórico **8259 →1985** foi incorporado e suas falhas de alocação sem vínculo foram mitigadas. Esta seção substitui a decisão de não incorporar lotes da seção 21. `AFATFS_REQUESTED_BATCH_CLUSTERS` aceita 1..8, padrão **8**; 1 desativa novos lotes mas mantém a recuperação de sessões anteriores. Apenas o pipeline nativo FAT32/cluster de 512 bytes participa. Mantidos API original 8.3, cache oito, ambas as FATs e uma reserva especulativa global. Os clusters do lote pertencem ao pedido atual do escritor, usando os campos existentes da operação, sem aumentar sua estrutura.

- EOCs dos clusters solicitados continuam confirmados nas duas FATs antes dos vínculos; vínculos confirmados antes do diretório. Antes do primeiro lote, o bit FAT32 padrão de encerramento limpo é limpo e confirmado nas duas cópias. fsync/fclose só publicam o marcador limpo depois das barreiras existentes, sem outras operações pendentes. Preparação/vínculos de lotes esperam enquanto o marcador limpo está sendo publicado. Nenhum formato privado novo de disco.
- Montagem nativa FAT32/cluster 512 verifica os dois marcadores. Volume sujo passa por recuperação antes de READY: reconcilia a FAT secundária a partir da primária em duas etapas (alocações antes dos vínculos), marca referências de ambas as FATs e das entradas de todos os diretórios, libera somente cabeças não referenciadas e confirma ambas as cópias antes de uma nova passagem poder liberar sucessores. Protege cadeias vivas, inclusive diretórios aninhados. Preserva nibbles altos e entradas reservadas/bad. Falhas de backend mantêm retries e impedem desligamento limpo.
- Recuperação reutiliza **um setor do cache** como bitmap de 4096 clusters e a união de operação de currentDirectory para cursores; travessia iterativa sem recursão. O bitmap não é gravado. Ao terminar, libera o setor e restaura o diretório raiz. Páginas realmente WRITING não aceitam alterações; o caminho FAT também espera se qualquer cópia envolvida está em trânsito. Esses bloqueios corrigem as mutações de buffers em trânsito encontradas durante a mitigação.
- Relatório [REQUESTED_BATCH_RECOVERY.md](tests/REQUESTED_BATCH_RECOVERY.md), JSON, `tests/requested_batch_report.py`. Evidências **tests/build/requested-batch-safe/**: before/lib congela a seção 21; prototype/lib congela o protótipo histórico; normal/lib e after/lib são a produção final; audit e three contêm comparações instrumentadas. Upstream local conferido `60c5f3580152410cb08342942cbbe3a30a449c24`. Arquivos de 1 MiB, dois ritmos, duas latências; **80 escritas e 80 releituras** abrangendo antes/protótipo/final/upstream e controles com três escritores. Conteúdo integral, igualdade das FATs atuais e ordem EOC/vínculo/diretório conferidos.
- Resultado intercalado **2/4096: 8193 →1799 metadados (−78,04%)**; protótipo histórico 1985, upstream 1538. Final: FAT1 **770**, FAT2 **770**, diretório **259**; antes 4095/4095/3. Escritas totais **12289 →5895 (−52,03%)**, leituras **34 →771**, upstream R453/W5634. A redução adicional frente ao protótipo ocorre com a base posterior e os bloqueios de páginas em trânsito, apesar do custo dos marcadores. Não houve medição de CPU nesta rodada. O upstream não grava FAT2 neste workload.
- Serial **1/4096: 4133 →1367** metadados; **2/4096: 8324 →2758**. Casos principais de 257 bytes mantêm contagem: um escritor 4133, dois seriais 8326/intercalados 8323. Três escritores são registrados no JSON; não presumir ganho em pedidos menores que dois setores. O lote só cobre setores completos já solicitados, respeitando página FAT, espaço contíguo e limites do arquivo.
- RAM estática inalterada: x64 **4872 B**, x86 **4760 B**, PIC24 normal **4614 B**, short **4586 B**. Código /O2 x64 **36680 →42660 B**, x86 **26923 →31611 B**. Matriz final de **48 configurações/perfis/ABIs** passou com mesmas seções estáticas; pilha interna padrão/multibloco/consulta **282/276 B**, máximo opcional **378 B**, incluindo PCs internos, excluindo backend/aplicação/libc conforme orçamento existente.
- `tests/fat32_requested_recovery.c`, integrado ao runner de alocação nos três perfis, passou **82 cortes de escrita +58 cortes da própria recuperação +8 falhas/retry** por perfil, incluindo prefixos e sufixos de meio setor. Verifica imutabilidade dos buffers, ausência de órfãos, FATs iguais e dados previamente confirmados na raiz/subdiretório. Também passaram escritores mistos 257/4096 e novo lote enfileirado durante publicação do marcador limpo. Os 18 testes anteriores de alocação/cache e sete suítes funcionais passaram; perfil padrão com lotes desativados também passou. Doxygen: **305 definições**, 304 nomes únicos. `lib/sdcard.h` não foi editado.
- Limites explícitos: recuperação não é fsck geral, não elimina ciclos órfãos nem resolve corrupção arbitrária. Estruturas inválidas detectadas interrompem a montagem. A primária é a autoridade da reconciliação. A varredura com memória limitada pode ser lenta em volumes grandes/fragmentados; não há medição em cartão físico. Cadeias ligadas a arquivos, inclusive cauda pré-alocada, são preservadas. fwrite continua aceitação em cache; fsync/fclose confirmam dados e tamanho lógico exato. Um corte sem sincronização pode expor a cauda pelo mecanismo preexistente de publicação de tamanho físico, agora ampliada pelo lote. A campanha sintética não prova todas as falhas físicas possíveis.

## 23. Cache de escrita compartilhado: 771 →35 leituras, sem ampliar reservas

Usuário confirmou incluir pedidos de 257 bytes e escrita byte a byte, preservando **uma única reserva especulativa mesmo que o ganho varie**. A política de cache compartilhada foi aplicada aos caminhos de fwrite/fputc e modos nativos w/a/w+/x/a+/r+. Pedidos pequenos continuam sem lotes antecipados de vários clusters; não há previsão de dados futuros ou novas reservas.

- `afatfs_fileUnlockCacheSector()` marca páginas de escritores FAT32/cluster 512 como discardable. Apenas páginas IN_SYNC, sem lock/retenção, podem ser reaproveitadas; DIRTY/WRITING continuam protegidas. Em `afatfs_allocateCacheSector()`, durante sessão de lotes marcada suja, a expulsão de uma FAT limpa é adiada se dados sujos liberados podem abrir espaço. Em `afatfs_cacheSector()`, pressão grava primeiro a página discardable suja/destravada mais antiga. Sem página liberável, mantém o fallback anterior. Não há pin permanente, membro novo ou buffer novo.
- O custo anterior veio da competição entre FAT/diretório e páginas de dados recém-gravadas: FATs limpas eram expulsas enquanto dados sujos ainda ocupavam o cache. Marcar somente os dados descartáveis reduziu 771 →739, mantendo 1799 metadados; reter FAT indiscriminadamente reduziu leituras mas aumentou publicações de diretório nos pedidos pequenos. A produção limita a retenção às sessões de lote e libera dados sob pressão; variantes rejeitadas permanecem isoladas em audit/released, pinned e pressure.
- Relatório [WRITE_CACHE_BATCH.md](tests/WRITE_CACHE_BATCH.md), JSON e preparador/validador `tests/write_cache_batch_report.py`. Evidências **tests/build/write-cache-batch/**: before/lib congela a seção 22; normal/lib é a produção final; modes/modes-extra têm fontes instrumentadas e inventários. Arquivos de 1 MiB, duas latências, dois ritmos, um/dois escritores, 257/4096 bytes. Backends padrão, leitura multibloco, multibloco+consulta e escrita exclusivamente monobloco; modos de abertura w/a/w+/x/a+/r+ e adaptador público fputc. As 40 execuções passaram **320 escritas e 320 releituras**. Entre 160 pares antes/depois, **72 melhoraram** leituras ou metadados; os demais mantiveram contagens, sem regressões de I/O medidas. Conteúdo integral, FATs iguais e ordem EOC/vínculo/diretório conferidos por gravação.
- Caso padrão intercalado **2/4096: R771 →35 (−95,46%), metadados 1799 →1560 (−13,29%), W5895 →5656 (−4,05%)**. FAT1/FAT2 770 →643 cada; diretório 259 →274. Leituras finais: FAT1 33, FAT2 1, diretório 1. Frente ao upstream local já medido `60c5f3580152410cb08342942cbbe3a30a449c24`, R453/W5634/metadados1538, restam **22 escritas a mais (+0,39%)**, atualizando as duas FATs (upstream não grava FAT2 no workload). Nenhuma medição de CPU ou cartão físico nesta rodada.
- **1/4096**, serial/intercalado: R499 →19, metadados 1367 →1154. **2/4096 serial**: R516 →35, metadados 2758 →2369. Demais modos/perfis de blocos grandes repetem a redução principal; r+ tem quatro publicações adicionais idênticas antes/depois para criação/fechamento antes da reabertura (1803 →1564). Pedidos 257 e fputc mantêm I/O. Suporte multibloco continua habilitado quando configurado, mas o escalonamento reduz dados transmitidos nessas sessões no caso principal de 3585 →253 setores; não inferir throughput físico somente das contagens reduzidas.
- RAM estática inalterada: x64 **4872 B**, x86 **4760 B**, PIC24 **4614/4586 B**. Código /O2 x64 42660 →42465 B; x86 31611 →32354 B. Matriz final de **48 configurações/perfis/ABIs** passou com seções estáticas iguais. Pilha padrão/multibloco/consulta **282/278 B**, máximo opcional **378 B**, incluindo PCs internos e excluindo backend/aplicação/libc conforme orçamento estabelecido.
- Passaram **21 testes de alocação/cache**, incluindo nos três perfis **82 cortes de escrita, 58 cortes durante recuperação, oito falhas/retry** por perfil; imutabilidade de buffers em trânsito, arquivos confirmados raiz/subdiretório, escritores mistos e marcador limpo em concorrência. Sete suítes funcionais padrão/opcionais passaram. Doxygen mantém 305 definições/304 nomes únicos; `git diff --check` limpo. Nenhuma alteração em `lib/sdcard.h`.
- Barreira EOC nas duas FATs →vínculos nas duas FATs →diretório, recuperação antes de READY e publicação limpa mantidas. Os limites da recuperação da seção 22 permanecem. A política melhora o uso dos mesmos oito setores, sem mudar a quantidade de alocações pertencentes a cada pedido.

## 24. Ajustes pequenos de 257 bytes, superados pela coleção da seção 25

Antes da exigência de ganho superior a 50%, foram adotados prioridade a alocações de pares já pendentes sobre a reserva opcional e flush filtrado do diretório antigo em sync/close. Relatório histórico `tests/SMALL_WRITE_PIPELINE.md/json`; evidências `tests/build/small-write-pipeline/`. A base imediatamente anterior à seção 25 ficou em **4132 metadados /6180 escritas** para 1/257 e **8320 /12416** para 2/257 intercalado. O usuário considerou a redução insuficiente e pediu mais de 50%. Protótipos de espera removida e flag de reserva durável não foram incorporados. A imagem segura dessa etapa está congelada em **tests/build/small-write-collection/before/lib**.

## 25. Coleção de pedidos pequenos: meta superior a 50% validada

Implementado no pipeline nativo FAT32 8.3, clusters 512, **cache oito setores**, duas FATs e reserva especulativa global única. `AFATFS_SMALL_WRITE_WINDOW_CLUSTERS=16` por padrão; zero conserva a alocação anterior. O limite de pedidos grandes continua `AFATFS_REQUESTED_BATCH_CLUSTERS=8` (um desativa os lotes). Cada cluster da coleção pertence a um pedido já recebido: nunca é alocado para dados futuros.

- A janela admite novos fwrite/fputc enquanto acumula EOCs nas mesmas páginas FAT. Com vários escritores, posições intercaladas na mesma página evitam colisões entre suas janelas, sem reservar os intervalos. A busca conserva o primeiro espaço livre e recorre a qualquer espaço da página se a posição preferida estiver ocupada. Fecha ao atingir o limite, em sync/close ou em até 64 passagens internas ociosas; gravações ainda em trânsito não contam como ociosidade. Consultas de capacidade reconhecem a cauda gravável durante a coleção.
- EOCs confirmados nas duas FATs →vínculos confirmados nas duas FATs →diretório. A coleção publica tamanho lógico exato após os dados aceitos estarem confirmados. Callback de sync/close fica atrás da alocação, usando uma estrutura com prefixo append que cabe na união de operação já existente (assert de tamanho). Flags novos usam bits nos bytes existentes. Sem heap ou buffer de setor novo. Uma imagem de diretório já exata evita regravação sem modificação.
- O marcador FAT32 sujo é confirmado antes da primeira janela. Para sessões que usaram coleção, fsync confirma dados/tamanho, mas deixa o marcador conservadoramente sujo até close; evita round trips dos cabeçalhos em cada fsync. Reinício após fsync pode fazer recuperação; dados confirmados continuam preservados. Close/desligamento limpo aguardam os ACKs antes de permitir power-off. Páginas de backend em trânsito não são alteradas.
- Relatório final **[SMALL_WRITE_COLLECTION.md](tests/SMALL_WRITE_COLLECTION.md)** e JSON, validador `tests/small_collection_report.py`. Evidências **tests/build/small-write-collection/**: before/lib é a seção 24; adopted/lib é a produção final; modes/modes-extra contêm fontes instrumentadas e inventários, adopted-matrix é a matriz final. SHA-256 de asyncfatfs.c **adeefb00598f8bea5748676c2518e30f287e56edf2cc614b9e25938f17116a4a**. Upstream local continua `60c5f3580152410cb08342942cbbe3a30a449c24`; esta rodada compara diretamente com before.
- Arquivos de 1 MiB por escritor, latência 0: **1/257: W6180 →2730 (−55,83%), metadados 4132 →682 (−83,49%), R17 →19**. **2/257 intercalado: W12416 →4947 (−60,16%), metadados 8320 →722 (−91,32%), R33 →70**. Serial 2/257: W12421 →4946 (−60,18%), metadados 8325 →721, R33 →71. Escritas incluem dados e metadados; os ganhos superiores a 50% não são apenas de metadados. Há regravações de setores parciais: caso 2/257 intercalado tem 4225 escritas de dados para 4096 setores de payload. O leitor de relatórios aceita e registra esse custo explicitamente, mantendo o teste estrito antigo como padrão para outros relatórios.
- **4 KiB preservado**: 1/4096 W3202/metadados1154/R19; 2/4096 intercalado W5656/metadados1560/R35. Passaram **40 execuções, 320 escritas e 320 releituras**, com conteúdo integral, FATs iguais e auditoria de EOC/vínculo/diretório por gravação. Nos **80 pares de 257 bytes**, todos superaram 50% de redução das escritas totais, abrangendo dois ritmos, duas latências, um/dois escritores, w/a/w+/x/a+/r+/fputc e backends padrão, leitura multibloco, consulta e escrita monobloco. Nenhuma medição de CPU ou throughput físico nesta rodada.
- RAM estática inalterada: **x64 4872 B**, **x86 4760 B**, **PIC24 normal/short 4614/4586 B**. Código /O2 x64 **42446 →44912 B**, x86 **32519 →34602 B**. Matriz final **48 configurações/perfis/ABIs** com seções estáticas iguais: pilha padrão **316/310 B**, multibloco/consulta **322/314 B**, máximo opcional **378 B**. Inclui PCs internos e exclui backend/aplicação/libc conforme orçamento estabelecido.
- Passaram sete suítes funcionais e **28 testes de alocação/cache**, incluindo novo `small_near_full` com arquivo proprietário do restante do volume e escritor inativo. Verifica uso das lacunas, conteúdo distinto por setor, cadeia do arquivo ocupante intacta, FATs e remontagem; execução reforçada em near-full-verified. Campanhas em três perfis: lote grande **82 cortes de escrita +58 de recuperação +8 falhas/retry**; coleção pequena **76 +58 +8**. Incluem meia página, criação/extensão, prefixo previamente confirmado, raiz/subdiretório, buffers imutáveis, escritores mistos e clean-marker em concorrência.
- `fat32_small_sync.c` solicita fsync imediatamente após aceitar cada pedido (sem esperar a janela ficar ociosa), confere mídia após cada callback, injeta três ACKs falhos e reinicia sem fclose. Em 16 pedidos de 257 bytes por escritor: 1 escritor, FAT52 →36, diretório16 →16, total92 →76; dois escritores, FAT66 →36, diretório32 →23, total146 →107. Aqui a barreira a cada pedido impede o mesmo ganho da escrita contínua. Contagens são ACKs duráveis; tentativas falhas são adicionais nas duas versões.
- Testes antigos que presumiam dados ainda sujos após uma espera agora explicitamente sujam um byte para manter uma transferência real pendente antes de testar falhas/close. A reserva legada continua coberta com a coleção desativada no teste específico; as novas janelas têm campanhas próprias. Doxygen: **309 definições, 308 nomes únicos**; git diff --check limpo. `lib/sdcard.h` não foi editado. Permanecem os limites de recuperação da seção 22: não é fsck geral, pode demorar em volumes sujos grandes e a campanha não cobre toda falha física possível.

## 26. Comparação completa atual/upstream — 19/11/2026

Relatório [FULL_UPSTREAM_COMPARISON.md](tests/FULL_UPSTREAM_COMPARISON.md), JSON com todas as amostras e scripts full_upstream_report.py/run_full_upstream_comparison.ps1. Nenhuma alteração de produção nesta rodada. Upstream local 60c5f3580152410cb08342942cbbe3a30a449c24; SHA-256 bruto atual asyncfatfs.c 08f51da7ffb388f40e48295a450e7e1a14c74dab4eb60e0d05cab76918c7c5b4 (o hash da seção anterior normaliza CRLF para LF).

- Cache 8 nas duas versões, FAT32 8.3, 1 MiB/arquivo, um/dois arquivos, blocos 257/4096, clusters 512/4096, ritmos serial/intercalado. Cinco variantes por ritmo: atual padrão/multiread/query e upstream original/mínimo multibloco 2. Fontes congeladas em tests/build/full-upstream-20261119, hashes validados, data/hora FAT iguais nas cópias. Objetos da biblioteca idênticos entre auditoria/CPU/ritmos por variante.
- 21 repetições alternadas de CPU por configuração, afinidade fixa, aquecimento e sem compilações concorrentes. Total 3360 fases CPU e 320 fases auditadas de I/O. QueryThreadCycleTime inclui biblioteca, backend RAM, polling e aplicação; não representa ciclos exclusivos da biblioteca ou PIC24. Leitura CPU tem 16 passagens; I/O uma passagem de dados após remontagem. CPU usa latência 0; throughput virtual usa latência 1, separadamente do throughput host medido. Nenhuma medida física de SD.
- Cluster 512, intercalado, CPU upstream → atual ciclos/B: escrita 1/257 4,223 →5,424 (+28,45%); 2/257 4,001 →5,341 (+33,48%); 1/4096 3,616 →4,525 (+25,15%); 2/4096 4,570 →4,370 (−4,38%). No ritmo serial todas essas escritas custam mais CPU (+28,40% a +49,70%). Leituras intercaladas variam −4,07% a +1,77%; seriais têm regressões +25,01% a +38,72%. Não generalizar o resultado intercalado aos demais ritmos.
- Mesmo cluster/intercalado, I/O latência 1: 1/257 R35 →27/W2803 →2730/meta755 →682; 2/257 R451 →39/W5792 →4826/meta1696 →722; 1/4096 R21 →19/W2771 →3189/meta723 →1141; 2/4096 R453 →35/W5603 →5453/meta1507 →1357. Throughput virtual MiB/s respectivamente 0,445 →0,471; 0,423 →0,516; 0,450 →0,429; 0,437 →0,484. Menos I/O não implicou menos CPU. Contagens dependem da latência, pois esta altera o agrupamento do cache; comparar somente o mesmo perfil.
- Cluster 4096: escritas intercaladas custam +30,66% a +73,49% CPU. Leitura multibloco opcional aumenta throughput virtual de um leitor/cluster512, com custo adicional de CPU; dois leitores não ganham throughput neste modelo. Alterar somente o mínimo multibloco do upstream de quatro para dois não explica as diferenças principais.
- RAM x64 atual 4872 B/upstream 4784 B; código .text 44912/17355 B. As restrições anteriores eram de não aumentar RAM em relação à versão atual anterior, não igualdade com upstream. Reutilizada matriz da mesma fonte: 48 configurações, máximo interno 378 B, excluindo backend/aplicação/libc. Conteúdo e cadeia primária conferidos em todas as variantes; atual também tem igualdade das FATs e auditoria EOC/vínculo/diretório. Upstream não grava FAT2 neste workload e termina com espelhos diferentes; garantias não são equivalentes.

## 27. CPU de escrita: aquisição e espelhamento únicos por página de vínculos

Implementado somente no pipeline nativo, fase BATCH_LINKS de asyncfatfs.c. A janela de alocações já solicitadas permanece na mesma página FAT. Depois de seus EOCs terem ACK nas duas cópias, o código adquire o par uma vez, prepara todos os vínculos reversos e copia a imagem final uma vez para FAT2. Antes readquiria/marcava/copiava o par a cada vínculo. O vínculo do cluster anterior continua individual, pois pode pertencer a outra página. Preserva os quatro bits altos das entradas. Aquisição incompleta não modifica entradas ou progresso; a barreira FAT1/FAT2 →diretório continua obrigatória. Nenhum campo, buffer, heap ou reserva adicional. Cache oito setores e reserva especulativa única preservados.

- Relatório [SMALL_WRITE_CPU.md](tests/SMALL_WRITE_CPU.md), JSON com amostras/quartis e validador small_write_cpu_report.py. Evidências tests/build/small-write-cpu/final: before/lib congela a seção 26; normal/lib é a produção adotada; before-bench/after-bench têm somente relógio FAT normalizado. SHA-256 bruto atual asyncfatfs.c 71fb8618315d63891b57715983460a958686990f06dcf29db6f1417d22af4348; texto LF 950d4187eb51bc8d2e3cd55adf8dc34955a72dccad249438cfbca76c8c74d8e5.
- 21 repetições alternadas, MSVC x64 /O2, afinidade fixa e sem compilações concorrentes. Padrão/multiread/query, serial/intercalado, um/dois arquivos, 257/4096 bytes, clusters 512/4096. CPU inclui biblioteca/backend/polling/aplicação; não é PIC24 ou cartão físico. Objetos de biblioteca idênticos entre auditoria/CPU/ritmos por variante.
- Cluster 512, padrão, redução de CPU pela mediana das razões pareadas: serial 1/257 6,10% (medianas 5,454 →5,097 ciclos/B), 2/257 3,69% (5,841 →5,614); intercalado 1/257 2,34% (5,351 →5,252), 2/257 4,26% (5,306 →5,085). 4096 bytes também melhora: serial 7,59%/7,67%, intercalado 2,12%/1,96%. Ainda há diferença de CPU frente ao upstream; não foi eliminada.
- Um escritor/257 no perfil multiread intercalado teve dispersão alta na rodada ampla: pareado −4,53%, mas razão das medianas +6,50%. Conservar ambos no relatório, sem ocultar o caso. Confirmação independente com os mesmos executáveis, 31 pares AB/BA e três aquecimentos: 4,944 →4,694 ciclos/B, pareado −4,75%; dois escritores 4,877 →4,652, −4,62%. Blocos 4096 −6,02%/−5,44%. Há variações/regressões em outros cenários de CPU, explicitadas no relatório; não generalizar o ganho de clusters 512 para todos os caminhos.
- Todos os 192 pares A/B de fases auditadas preservaram exatamente I/O, metadados por FAT/diretório, polls e sessões/blocos multibloco em ambas as latências. Intercalado padrão/latência0: 1/257 R19/W2730/meta682; 2/257 R70/W4947/meta722; 1/4096 R19/W3202/meta1154; 2/4096 R35/W5656/meta1560. Continuam os ganhos superiores a 50% da seção 25. Conteúdo, FATs iguais e ordem de publicação verificados. Sem mudança no throughput virtual, pois os contadores são iguais.
- RAM estática x64 4872 B/x86 4760 B; PIC24 padrão normal/short 4614/4586 B, inalterada. Código x64 44912 →45076 B (+164), x86 34602 →34682 B (+80). Matriz de 48 configurações com seções estáticas iguais em todos os pares: pilha padrão 330/324 B, multiread/query 336/328 B, máximo opcional 378 B. Inclui PCs internos e exclui backend/aplicação/libc.
- Passaram os 28 testes de alocação/cache: três escritores, volumes quase cheios, reserva única, reconstrução do espelho, fsync imediato e dados em trânsito imutáveis. Três perfis: lote grande 82 cortes de escrita +58 de recuperação +8 falhas/retry; coleção pequena 76 +58 +8. Doxygen 309 definições/308 nomes únicos; git diff --check limpo. sdcard.h não foi editado.
- Alternativas de simplificação do fwrite e polling ficaram somente em snapshots experimentais, por ganhos pequenos/inconsistentes. Scripts small_write_cpu_experiment.py, small_write_batch_links_experiment.py e small_write_cpu_confirm.py preservam a preparação das etapas. O preparador final reutiliza executáveis anteriores equivalentes; o validador confere os objetos por variante antes de aceitar resultados.

## 28. CPU de escrita: acesso direto ao par FAT já DIRTY durante a coleção

Implementado em afatfs_collectNext(), somente no pipeline nativo. Uma passagem pelo cache procura primária e espelho DIRTY. Essas imagens já foram pareadas, não podem ser expulsas e nenhuma pertence ao backend. Após verificar a entrada livre e as reservas, prepara o EOC na primária e copia quatro bytes para FAT2, preservando os bits altos e as entradas vizinhas. Evita três percursos de aquisição e a cópia integral de 512 bytes no caso comum. Página ausente/limpa/em transferência conserva o caminho anterior. Mantém as três visitas lógicas do cache, seus timestamps, fsInfoDirty, writeTimestamp, cacheDirtyEntries e retainCount. Não altera janela, reserva, submissões ou barreiras EOC nas duas FATs →vínculos nas duas FATs →diretório. Sem novo campo, buffer ou heap.

- Relatório [SMALL_WRITE_PAIR_CPU.md](tests/SMALL_WRITE_PAIR_CPU.md) e JSON; validador small_write_pair_cpu_report.py. Evidências tests/build/small-write-cpu2/final: before/lib congela a seção 27, normal/lib é a produção; before-bench/after-bench têm só relógio FAT normalizado. SHA-256 bruto asyncfatfs.c 318dc1687ca5f508f70536eff9685a6c0ef3a5cbfb49dc6d3edce28058d7e2e8; texto LF 4b4114af67130c1230c8caabd223da99ee9610249f124b955e53242ef2ac4e5b. Upstream local 60c5f3580152410cb08342942cbbe3a30a449c24, cache oito, mínimo multibloco original quatro; atual usa dois e mantém as duas FATs, enquanto upstream não grava FAT2 no workload.
- 21 repetições alternadas, MSVC x64 /O2, afinidade fixa, aquecimento e sem compilações concorrentes. Um/dois arquivos de 1 MiB, 257/4096 bytes, clusters 512/4096, serial/intercalado, padrão/multiread/query. CPU inclui biblioteca/backend/polling/aplicação; throughput host não é cartão físico ou PIC24. Objetos idênticos entre auditoria/CPU/ritmos por variante. JSON contém medianas, quartis, amostras e os deltas pareados; não misturar razão das medianas com mediana das razões.
- Nos 12 casos de escrita 257/cluster512, CPU pareada caiu 5,80% a 9,43%. Padrão serial: 1/257 5,146 →4,761 ciclos/B (−7,72% pareado), 2/257 5,690 →5,308 (−6,39%). Padrão intercalado: 1/257 5,348 →5,015 (−7,40%), 2/257 5,098 →4,748 (−6,75%). Excesso das medianas sobre upstream intercalado: 28,34% →20,33% e 26,07% →17,42%; serial 27,21% →17,71% e 44,83% →35,09%. Ainda não eliminou a diferença.
- Multiread intercalado 1/257 −5,80%, 2/257 −5,84%; query −6,22%/−8,07%. Blocos 4096/cluster512 padrão também melhoraram na medição: serial −1,33%/−1,01%, intercalado −3,49%/−4,16%. O MSVC mudou a organização do código de fwrite (1658 →1289 bytes no objeto), contribuindo para efeitos fora do novo ramo; fread/poll padrão mantêm bytes idênticos. Não inferir o mesmo ganho de CPU no PIC24; há variações/regressões em outros cenários, registradas no relatório.
- Todos os 192 pares auditados preservaram exatamente I/O, metadados por FAT/diretório, polls e sessões/blocos multibloco, nas duas latências; throughput virtual igual. Padrão intercalado/latência0 continua 1/257 R19/W2730/meta682 e 2/257 R70/W4947/meta722; 1/4096 R19/W3202/meta1154, 2/4096 R35/W5656/meta1560. Conteúdo integral, igualdade das FATs e ordem EOC/vínculo/diretório conferidos.
- RAM estática igual: x64 4872 B, x86 4760 B, PIC24 padrão 4614/4586 B. Código x64 45076 →45327 B (+251); x86 34682 →34944 B (+262). Matriz de 48 configurações: seções estáticas iguais; pilha padrão 330/324 B, multiread/query 336/328 B, máximo opcional 378 B, incluindo PCs internos e excluindo backend/aplicação/libc.
- Passaram os 28 testes de alocação/cache. fat32_cache_mirror.c acrescenta nove cenários dirigidos nos três perfis: par sujo e bits altos/vizinhos, estouro do cacheTimer, espelho limpo envenenado, primária limpa, cada FAT em WRITING, candidato ocupado, reserva de outro escritor e dados independentes em WRITING. Confere três acessos lógicos, FSInfo, retainCount, dirtyEntries, writeTimestamp, cópia de quatro bytes versus reparo integral e ausência de I/O novo. Testes de três escritores, quase cheio, reserva única, fsync imediato e buffers imutáveis continuam passando.
- Três perfis de recuperação: lote grande 82 cortes de escrita +58 de recuperação +8 falhas/retry; coleção pequena 76 +58 +8. Doxygen 309 definições/308 nomes únicos e git diff --check limpo. lib/sdcard.h não foi editado. Alternativas aritmética/combinada ficaram somente nos experimentos: o ganho da aritmética foi pequeno e a combinação foi inferior em blocos grandes. Preparação e confirmação: small_write_cpu2_experiment.py, small_write_cpu2_confirm.py e run_small_write_cpu2_confirmation.ps1.

## 29. CPU de escrita: filtrar candidatas antes das políticas de flush

Implementado em afatfs_flushCacheFiltered(): páginas limpas, travadas ou com timestamp igual/mais novo que a candidata já escolhida são ignoradas antes de consultar as políticas puras de retenção. O padrão agora compartilha o laço antecipado já usado pelos perfis multiread/query, removendo um bloco condicional duplicado. A página escolhida e o desempate pelo primeiro índice permanecem iguais. As consultas removidas não mudam estado, cacheTimer ou I/O. Em afatfs_collectFatPage(), um filtro de intervalo FAT evita percorrer escritores para dados/diretórios: diferença unsigned relativa ao início e duas comparações/subtrações, sem multiplicar comprimentos que podem estourar. Conserva acertos das duas FATs mesmo com volta do LBA. Nenhum campo, buffer ou reserva adicional; barreiras, locks, timestamps, submissões e ACKs inalterados.

- Relatório [SMALL_WRITE_FLUSH_CPU.md](tests/SMALL_WRITE_FLUSH_CPU.md) e JSON; validador small_write_flush_cpu_report.py. Evidências tests/build/small-write-flush-cpu/final: before/lib congela a seção 28; normal/lib é a produção final; before-bench/after-bench têm somente relógio FAT normalizado. SHA-256 bruto asyncfatfs.c bec48f9daaf4c157c653311781a6c325eb57a1643d2eeb6bd805df6d4dd44538; texto LF 6baa43fb26f1c6f4690461851f5143fd90de4b72a51db88af0552d4fa940b7cd. Upstream local 60c5f3580152410cb08342942cbbe3a30a449c24, cache oito, mínimo multibloco original quatro; atual mínimo dois, duas FATs e reserva especulativa única. Upstream não grava FAT2 no workload e termina com espelhos diferentes.
- 21 repetições alternadas, MSVC x64 /O2, afinidade fixa, aquecimento e sem compilações concorrentes. Um/dois arquivos de 1 MiB, 257/4096 bytes, clusters 512/4096, serial/intercalado, padrão/multiread/query. CPU inclui biblioteca/backend/polling/aplicação, não é PIC24 ou cartão físico. Objetos da biblioteca idênticos entre CPU/auditoria/ritmos por variante. JSON mantém amostras/quartis; delta pareado é mediana das razões e excesso sobre upstream é razão das medianas.
- Escritas 257/cluster512 padrão: serial 1/257 4,778 →4,365 ciclos/B (−9,23% pareado), 2/257 5,269 →4,677 (−11,26%). Intercalado 1/257 5,044 →4,349 (−13,87%), 2/257 4,800 →4,340 (−9,52%). Excesso sobre upstream serial 17,45% →7,30% e 34,79% →19,64%; intercalado 24,23% →7,12% e 21,28% →9,65%. Diferença reduzida, ainda presente. Blocos 4096/cluster512 padrão também melhoraram: serial −7,31%/−8,16%, intercalado −8,29%/−4,96%.
- Perfis opcionais não têm ganho geral comprovado. Em 257/cluster512 multiread serial deltas −0,43%/+0,10%, intercalado −0,25%/+0,26%; query serial −2,59%/+0,22%, intercalado −1,32%/+0,30%. Vários são ruído de pequena magnitude; razão das medianas e pareado podem divergir com dispersão. Todos os demais resultados, inclusive regressões, estão no relatório; nenhuma regressão pareada observada superou 3% nesta rodada. Não extrapolar a melhora do padrão para todos os perfis.
- Todos os 192 pares de fases auditadas mantiveram exatamente leituras, escritas, metadados por FAT/diretório, polls, sessões e blocos multibloco nas duas latências. Throughput virtual igual. Padrão intercalado/latência0 continua 1/257 R19/W2730/meta682, 2/257 R70/W4947/meta722, 1/4096 R19/W3202/meta1154, 2/4096 R35/W5656/meta1560. Conteúdo, FATs iguais e ordem EOC/vínculo/diretório conferidos.
- RAM estática igual: x64 4872 B, x86 4760 B, PIC24 padrão 4614/4586 B. Código x64 45327 →46028 B (+701), x86 34944 →35010 B (+66). Matriz de 48 configurações com seções estáticas iguais: padrão 336/328 B de pilha, multiread/query 336/328 B, máximo opcional 378 B. Inclui PCs internos e exclui backend/aplicação/libc.
- Passaram 28 testes de alocação/cache. fat32_cache_mirror.c acrescenta verificação do intervalo com dois escritores e quatro geometrias: normal, endereços dando a volta, comprimento acima de 2³¹ e UINT32_MAX. Confere proprietários nas duas FATs e páginas sem proprietário, sem I/O novo. Os nove cenários de par DIRTY/cacheTimer/imutabilidade da seção 28 continuam passando nos três perfis. Também passaram quase cheio, três escritores, reserva única e fsync imediato.
- Três perfis de campanhas: lote grande 82 cortes de escrita +58 de recuperação +8 falhas/retry; coleção pequena 76 +58 +8. Doxygen 309 definições/308 nomes únicos; git diff --check limpo. lib/sdcard.h não foi editado. Scripts de preparação/medição: small_write_flush_cpu_experiment.py, small_write_flush_cpu_confirm.py e run_small_write_flush_cpu_confirmation.ps1. A rodada inicial separou filtragem, intervalo e combinação; a confirmação usa a combinação com intervalo sem overflow, não o protótipo com multiplicação. A ordem antecipada havia sido revertida no antigo padrão/cache quatro; foi reavaliada agora com cache oito e a coleção atual, com novos resultados.

## 30. CPU de escrita: despacho único de append e busca limitada do par FAT

Mantido em asyncfatfs.c: afatfs_collectNext() encerra a busca assim que encontra as duas páginas DIRTY, sem alterar acessos lógicos/timestamps ou o fallback. afatfs_fileOperationsPoll() ignora operações NONE, classifica append válido uma vez e usa sua continuação diretamente em todos os perfis nativos. Na fase COLLECT, backend ocupado não incrementa collectIdle; valores 0..62 incrementam uma vez por passe. Em 63, entra na continuação completa original, sela a coorte e aguarda dados/FATs/diretório. Sync/close, handles inválidos e demais operações continuam no despacho genérico. O contador original de seis bits foi preservado; não houve mudança de representação ou tamanho das estruturas na versão final. Cache oito setores, duas FATs, reserva especulativa única, janela de pedidos já recebidos e persistência inalterados.

- Relatório [SMALL_WRITE_POLL_CPU.md](tests/SMALL_WRITE_POLL_CPU.md) e JSON; publicador small_write_poll_cpu_report.py usa a validação compartilhada small_write_cpu_validation.py. Evidências finais tests/build/small-write-poll-cpu/final-dispatch: before/lib congela a seção 29; normal/lib é a produção; fontes bench têm só relógio FAT normalizado. SHA-256 bruto asyncfatfs.c 9c321484f79c190b372135916133b68d2fade647dc9ef8c6621ed4d910baf9a7; texto LF 9f66f82253f6024a0d28415a7bf17126df265dde6c780490cd243df3da2f16de. Upstream local permanece 60c5f3580152410cb08342942cbbe3a30a449c24. Upstream não grava FAT2 no workload; as garantias de espelhamento não são equivalentes.
- 21 repetições alternadas, MSVC x64 /O2, afinidade fixa, aquecimento, sem compilações concorrentes. Um/dois arquivos de 1 MiB, 257/4096 bytes, clusters 512/4096, serial/intercalado, padrão/multiread/query. CPU inclui biblioteca/backend/polling/aplicação; não é PIC24 ou cartão físico. Objetos de biblioteca idênticos entre CPU/auditoria/ritmos por variante. Medianas, quartis e amostras estão no JSON; delta pareado e razão das medianas são métricas diferentes e podem divergir no sinal com dispersão.
- Ganho mais consistente: DOIS escritores/257/cluster512. Padrão serial 4,662 →4,423 ciclos/B, −5,95% pareado; intercalado 4,302 →4,169, −3,06%. Excesso sobre upstream serial 18,95% →12,84%; intercalado 7,45% →4,14%. Multiread dois escritores: serial −7,20%, intercalado −6,27%; query −9,75%/−5,60%. Não afirmar ganho geral com um escritor: padrão serial 4,259 →4,267 (pareado −1,88%, mas razão das medianas +0,18%); intercalado 4,523 →4,290 (−1,25% pareado), com dispersão. Perfis opcionais de um escritor também variam; a redução de 21,43% pareada em query/serial tem baseline disperso e não deve ser generalizada.
- Os demais cenários, incluindo regressões, estão integralmente no relatório. Padrão/4096/cluster512: serial +2,25%/+0,81% pareados (um/dois escritores), intercalado +0,02%/−0,30%. A otimização é focada em 257 bytes; não anuncia melhora em todos os caminhos.
- Todos os 192 pares A/B auditados mantiveram exatamente I/O, metadados por FAT/diretório, polls e sessões/blocos multibloco, nas duas latências. Throughput virtual igual; conteúdo, FATs iguais e ordem EOC/vínculo/diretório conferidos. Padrão intercalado/latência0 continua 1/257 R19/W2730/meta682 e 2/257 R70/W4947/meta722; 1/4096 R19/W3202/meta1154, 2/4096 R35/W5656/meta1560.
- RAM estática igual: x64 4872 B, x86 4760 B, PIC24 padrão 4614/4586 B. Código x64 46028 →45923 B (−105); x86 35010 →34800 B (−210). Matriz final de 48 configurações: seções estáticas iguais e pilha padrão/multiread/query 336/328 B, máximo opcional 378 B. Inclui PCs internos; exclui backend/aplicação/libc. Probes de layout/flags x86/x64 executados e checks de alinhamento XC16 normal/short compilados. Estado/handle/append/extensão são iguais antes/depois nos quatro ABIs: x86 656/96/20/36, x64 760/120/20/48, PIC24 normal 512/76/16/26, short 484/74/14/24 bytes.
- Passaram novamente 28 testes de alocação/cache. checkCollectionIdlePoll() percorre incrementos 0..62, todos os valores 0..63 com backend ocupado, tipo NONE e operação NONE; verifica flags e cacheTimer. Dois passes seguidos dão 62 →63 e depois selam todos os escritores. Duas submissões de dados rejeitadas mantêm ambos em NEW_CLUSTER_DURABLE, sem avançar o segundo contador ou aceitar I/O. Os nove cenários de par FAT foram repetidos nos slots 0/1, 1/0 e 6/7 nos três perfis, cobrindo busca antecipada e ordem inversa. Os testes de limites LBA, quase cheio, três escritores, reserva única, fsync imediato e imutabilidade continuam passando.
- Campanhas finais nos três perfis: lote grande 82 cortes de escrita +58 de recuperação +8 falhas/retry; coleção pequena 76 +58 +8. Doxygen 309 definições/308 nomes únicos; git diff --check limpo. lib/sdcard.h não foi editado.
- Alternativas rejeitadas ficaram em tests/build/small-write-poll-cpu. A primeira combinação, com contador uint8_t e verificações duplicadas, confirmou ganho com dois escritores, mas regressão de 1,57% no padrão intercalado de um escritor. Dados congelados em final/rejected-report.md e JSON, fonte be1333401d8c3f7f9f2d8d9266ead3a3dab0ba2916208166e9ecd15123eb52d5. O byte coube na união sem aumentar RAM, mas a comparação com despacho unificado continuou favorecendo os seis bits com um escritor, então não foi mantido. Scripts: small_write_poll_cpu_experiment.py, small_write_idle_byte_experiment.py, small_write_dispatch_experiment.py, small_write_poll_cpu_confirm.py, run_small_write_poll_cpu_confirmation.ps1 e run_small_write_counter_layout.ps1. A falha inicial do comando XC16 short era splatting de uma string; corrigido para string[] e todos os probes foram repetidos com sucesso.
