# Cache de dois leitores em FAT32 com clusters de 512 bytes

Data: 16/11/2026, America/Sao_Paulo. Base do trabalho: commit `786a27f140f04c57723d00cd6a1c90e81a7e9bfe` (`DMA support`). Referência: `upstream/master` local no commit `60c5f3580152410cb08342942cbbe3a30a449c24`.

A otimização eliminou as **510 releituras adicionais** no caso de dois leitores alternados e blocos de 4096 bytes. O cache continua com quatro páginas, os handles/descritores não mudaram e o limite conservador das cascatas internas PIC24 permanece abaixo dos 400 bytes solicitados. As duas FATs e as regras de ACK/persistência foram preservadas.

## Mudança e causa das releituras

Após terminar um bloco de um arquivo, seu prefetch já havia trazido o primeiro setor do bloco seguinte. Enquanto o outro leitor consumia oito setores, a política LRU podia expulsar esse prefetch, embora ainda mantivesse páginas já consumidas. Ao voltar ao primeiro arquivo, era necessário ler aquele setor novamente. Nos 256 blocos de cada arquivo de 1 MiB, excluindo o fim de arquivo, isso correspondia a 510 leituras extras.

Agora, liberar uma página retida de um arquivo normal, exclusivamente de leitura, em FAT32 com clusters de 512 bytes marca a página como candidata preferencial à substituição. Reter a página novamente remove essa preferência. O bit `discardable` já existia; não foi adicionado estado por leitor. O prefetch continua oportunista e não ganha retenção permanente.

A escolha de vítima continua exigindo página IN_SYNC, sem lock e sem retenção. A preferência não permite descartar dados sujos, writes/reads em voo ou buffers ainda usados por outro leitor. Metadata, cursores, estado de publicação, gerações de mídia e callbacks não mudaram. Rewind/seek pode exigir uma leitura normal caso uma página antiga já tenha sido reutilizada; o conteúdo é conferido nos testes.

Uma primeira versão marcava o consumo em `fread()` e usava quatro bytes adicionais de pilha no PIC24. A marcação foi movida para a rotina de liberação existente. As duas versões produziram o mesmo I/O e tempo modelado. Não há ganho de CPU demonstrado por economizar esses quatro bytes: o ganho principal é evitar comandos/transferências repetidos.

## Correção necessária para medir a base DMA

O commit inicial habilitava o cache DMA por padrão, mas `afatfs_init()` atribuía seu ponteiro **antes** de chamar a rotina que zerava todo o estado. O ponteiro ficava NULL e o benchmark terminava com acesso inválido antes de gerar resultados. Isso era independente dos discos/VHDs ausentes.

A atribuição foi movida para depois do memset, dentro de `afatfs_resetMediaSession()`. O ponteiro é restaurado em todos os resets, após o backend cancelar acessos antigos, sem criar buffers ou alterar a geração de mídia. Essa mesma correção foi aplicada à base anterior para comparar somente a política de cache. `original/lib` preserva a versão recebida, `before/lib` contém apenas a correção DMA e `after/lib` contém também a otimização.

## Comparação com upstream

Cada arquivo tem 1 MiB. Foram sete repetições por caso, com um/dois leitores, blocos de 257/4096 bytes e controles com clusters de 4096 bytes. As variantes atuais usam cache de quatro setores; upstream foi medido com quatro setores e com seu padrão de oito. A tabela mostra o perfil de latência, em **MiB/s agregados modelados**.

| Bloco | Leitores | Antes, multileitura ligada | Depois, multileitura desligada | Depois, multileitura ligada | Upstream cache 4 / 8 |
| --- | --- | --- | --- | --- | --- |
| 257 B | 1 | 3,932982 | 2,202837 | 3,932982 | 2,202595 / 2,202595 |
| 257 B | 2 | 2,203808 | 2,203808 | 2,203808 | 2,203565 / 2,203565 |
| 4096 B | 1 | 4,683183 | 2,419960 | 4,683183 | 2,405234 / 2,405234 |
| 4096 B | 2 | **2,155033** | **2,421132** | **2,421132** | **2,406391 / 2,406391** |

No cenário alvo, o ganho modelado é de **12,35%** sobre a base anterior e **0,61%** sobre upstream. A política de sessão multibloco continua usando fallback com dois leitores; portanto esse ganho vem do cache, com a sessão ligada ou desligada.

| Dois leitores, blocos de 4096 B | Antes | Depois | Upstream |
| --- | --- | --- | --- |
| Setores de dados distintos | 4.096 | 4.096 | payload validado |
| Releituras de dados por endereço físico | **510** | **0** | — |
| Leituras de metadados | 34 | 34 | 34 |
| Total de setores lidos | **4.640** | **4.130** | **4.130** |
| Gravações durante a leitura | 0 | 0 | 0 |

A auditoria adicional usa um mapa de setores no harness, não na biblioteca, para distinguir primeiro acesso e releitura. Os 510 setores desapareceram integralmente; não foram substituídos por mais leituras de FAT. O perfil de latência passa de 928.060 para 826.060 µs: exatamente 102 ms a menos para 510 leituras de 200 µs.

Todos os outros cenários preservaram suas contagens e tempos virtuais, inclusive escrita, blocos de 257 bytes, um leitor e controles com clusters de 4096 bytes. As quatro variantes principais abrangem 448 volumes/896 fases; as auditorias antes/depois acrescentam 224 volumes/448 fases. Cada volume é auditado e remontado para verificar tamanho, payload e, nas versões atuais, igualdade das FATs e ausência de alocações excedentes.

O upstream continua deixando a FAT secundária desatualizada no ensaio de escrita, como nas comparações anteriores. Seus writes não são uma referência equivalente de persistência; o objeto desta tabela é a leitura após remontagem.

### Limites das taxas

O modelo usa 200 µs por leitura simples e polls de 10 µs. A sessão opcional mantém as premissas anteriores: 100 µs de abertura, 100 µs por setor e 100 µs de parada. Não são medidas em cartão físico.

No perfil de um poll por transferência, com a mesma auditoria de endereços ligada nas duas versões, a mediana de execução no computador foi 611,808 MiB/s antes e 607,773 MiB/s depois no caso alvo (aproximadamente −0,66%). Portanto, a redução de I/O não deve ser apresentada como aceleração de CPU; sua utilidade está no dispositivo com latência. A redução de quatro bytes de frame também não foi isolada como ganho de velocidade.

## RAM e limite de pilha em cascata

| ABI | RAM estática antes/depois | Handle antes/depois | Descritor antes/depois |
| --- | --- | --- | --- |
| PIC24, enums normais | 2.494 / 2.494 B | 76 / 76 B | 18 / 18 B |
| PIC24, enums curtos | 2.474 / 2.474 B | 74 / 74 B | 16 / 16 B |
| x86 | 2.616 / 2.616 B | 96 / 96 B | 24 / 24 B |
| x64 | 2.728 / 2.728 B | 120 / 120 B | 24 / 24 B |

A configuração DMA mantém o buffer externo alinhado e o ponteiro no estado. Os números x86/x64 incluem esse ponteiro já presente no commit recebido; não devem ser confundidos com as medições anteriores à mudança de branch. As colunas são iguais com multileitura ligada ou desligada. Não há heap novo na biblioteca; o mapa de releituras pertence exclusivamente ao teste.

Nenhum frame existente cresceu no código PIC24 gerado, incluindo clones do compilador: `fread` mantém 24 bytes e a liberação mantém frame zero. Além disso, foi analisada a cascata interna, e não apenas cada função isoladamente:

| Configuração FAT32 8.3 / PIC24 | Limite conservador acumulado | Margem até 400 B |
| --- | --- | --- |
| Antes, enums normais | 316 B | 84 B |
| Depois, multileitura desligada, enums normais | 316 B | 84 B |
| Depois, multileitura ligada, enums normais | 314 B | 86 B |
| Antes/depois, enums curtos | 302 B | 98 B |

O escopo de 400 bytes é **somente as chamadas internas da biblioteca**, conforme definido pelo usuário. O analisador cobre `asyncfatfs.c` e `fat_standard.c`, adiciona os PCs de retorno de quatro bytes, registradores salvos, frame pointer e ajustes positivos de pilha, inclusive os de argumentos. Soma inclusive ajustes de caminhos mutuamente exclusivos, produzindo uma cota conservadora; reserva também o scratch aritmético usado pelo compilador. Recursão direta e ajustes dinâmicos/desconhecidos de SP são rejeitados.

Chamadas ao backend, libc, rotinas aritméticas do compilador e callbacks da aplicação são fronteiras externas listadas no JSON; seus frames e reentrância pela aplicação não entram neste orçamento. Interrupções também não entram. Isso é uma análise estática das compilações PIC24 8.3 avaliadas, não um watermark de firmware/hardware nem uma garantia para configurações opcionais não analisadas.

A maior cadeia conservadora começa em `fopen`, passa pela criação/extensão de diretório e alocação, e termina no caminho de cache/flush. Não é o caminho de leitura. O limite é verificado automaticamente por `pic24_stack_budget.py` e pelo gerador de resultados. As compilações usam XC16 2.10/PIC24FJ256GA702 e as mesmas opções antes/depois, sujeitas às limitações da licença disponível.

## Testes disponíveis e arquivos ausentes

Foram usados testes sintéticos que geram seus próprios dados/imagens. Não foi necessário recuperar VHDs ou discos perdidos na troca de branch. Testes com VHD/driver nativo e cartão físico não foram executados e não são contados como falhas do código.

Passaram as dez configurações/casos da suíte de multileitura disponível, incluindo FAT16/FAT32, LFN/exFAT opcionais, falhas de sessão, remoção de mídia, callbacks antigos e integração com o simulador distribuído. Também passaram os testes sintéticos de ordenação de alocação, três escritores, volume quase cheio, espelhamento, ACKs com falha e reserva única.

O novo `fat32_reader_cache.c` cria seu próprio arquivo e verifica a preferência de páginas liberadas, remoção da preferência ao reter novamente, proteção de retenção/lock/dirty/READING/WRITING, exclusão de escritores, payload após rewind e restauração do ponteiro DMA após reset. As fixtures de descritores são descartadas antes de voltar a executar I/O real.

## Evidências e reprodução

Fontes e saídas estão preservadas em `tests/build/fat32-two-readers-cache-20261116`. O [JSON](FAT32_TWO_READER_CACHE.json) contém hashes, métricas, recursos e cadeias de pilha completas. A base `before/lib` deve preservar a política anterior com a mesma correção do ponteiro DMA; `original/lib` mantém a fonte recebida.

```powershell
tests/run_reader_cache_comparison.ps1
tests/run_fat32_allocation_checks.ps1 -OutputDirectory tests/build/fat32-two-readers-cache-20261116/allocation
tests/run_multiread_tests.ps1 -OutputDirectory tests/build/fat32-two-readers-cache-20261116/final-regressions
python tests/two_reader_cache_report.py --directory tests/build/fat32-two-readers-cache-20261116 --output tests/FAT32_TWO_READER_CACHE.json
```

O validador exige sete repetições determinísticas, as 510 releituras eliminadas, nenhum aumento de I/O nos controles, igualdade de RAM, ausência de crescimento dos frames comuns, cascatas internas de até 400 bytes e correspondência entre os fontes medidos e a biblioteca de trabalho.
