# Leitura multibloco opcional em FAT32

Data: 16/11/2026. Implementação e comparação com o `upstream/master` local, commit `60c5f3580152410cb08342942cbbe3a30a449c24`.

Foi implementado o contrato opcional de sessão de leitura no backend, sua implementação no simulador de arquivos e a integração na biblioteca. Não há aumento de RAM estática da biblioteca nem do estado do simulador. O cache mantém quatro páginas e o prefetch mantém seu comportamento anterior. O recurso fica **desabilitado por padrão**, para preservar a compatibilidade com backends existentes.

Para habilitar na compilação da biblioteca:

```c
#define AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT 2
```

O backend precisa implementar `sdcard_beginReadBlocks()` e `sdcard_endReadBlocks()`, além das funções existentes. O driver físico SD não está neste repositório; o ganho em hardware depende da sua implementação de comando multibloco e término de sessão. As taxas deste relatório são de um modelo, não de um cartão físico.

## Funcionamento

A integração anuncia trechos para um único handle normal, exclusivamente de leitura, em FAT32 com clusters de 512 bytes. Um primeiro passo verifica o handle e limita a quantidade ao tamanho lógico. Um segundo consulta somente a FAT já presente no cache, verifica links consecutivos, limites do volume e fronteira da página FAT. Nenhum desses passos inicia leitura de FAT ou reserva páginas futuras. São chamadas sequenciais, sem estado persistente adicional.

O backend mantém uma sessão limitada à faixa anunciada. A biblioteca continua solicitando **um setor por vez**, com seu buffer e callback próprios. Repetir o início no próximo LBA esperado reutiliza a sessão em vez de emitir outro comando. A entrega do último setor encerra a sessão. Assim, não é necessário manter simultaneamente no cache todos os setores da faixa, nem alocar um buffer contíguo grande.

O máximo anunciado nesta integração é uma página FAT32: 128 clusters. O limite pode ser menor pelo EOF, fim do volume, fragmentação ou posição inicial na página FAT. A primeira leitura antes de a FAT estar no cache usa o caminho simples; não se lê metadado extra só para descobrir uma sessão possível.

Com dois handles legíveis, inclusive dois leitores do mesmo arquivo, a política volta a leituras simples. Isso evita abrir e interromper uma sessão a cada alternância entre arquivos, sem acrescentar estado por consumidor. Também há fallback para handles de leitura/escrita, outras geometrias e outros sistemas de arquivos. A política é conservadora: até um segundo leitor aberto mas inativo desabilita novos anúncios.

O simulador reutiliza os dois contadores que já serviam às escritas multibloco. Uma posição de seu enum de estado distingue a sessão de leitura; o enum não cresce em nenhum dos ABIs medidos. Não foram acrescentados buffers, estruturas, campos Booleanos ou contadores de produção. Os contadores adicionais do benchmark são instrumentação de teste, fora da biblioteca e do backend distribuído.

## Persistência e posse dos buffers

- Iniciar ou parar uma leitura não apaga nem grava setores. O teste do simulador verifica que até as partes anunciadas mas não consumidas permanecem intactas.
- Leitura não consecutiva ou escrita termina a sessão anterior antes de executar a nova operação. Escrita ainda em andamento não pode ser interrompida sem completar sua posse/ACK; o backend responde ocupado.
- A sessão não possui buffers futuros: somente uma solicitação de setor aceita pode acessar o buffer entregue, até seu callback. Permanecem os estados READING existentes do cache.
- Falha de leitura encerra a sessão; um callback com NULL invalida a página, que deve ser lida novamente. A geração completa de mídia continua em cada callback, sem bits reaproveitados.
- Flush, sync/close, destruição limpa e uma resposta positiva de idle confirmam a parada da leitura. Falha/ocupado na parada mantém a operação pendente. Reset cancela sincronamente DMA e sessão; callbacks antigos não podem validar páginas de uma nova montagem.
- Um seek atendido inteiramente pelo cache não precisa emitir outro comando: uma solicitação posterior fora da sequência para a sessão antes de ler. Fechamento, flush e idle também permitem abandoná-la antes do fim anunciado.
- As duas FATs, a ordem de alocação, os ACKs de metadados e as barreiras de publicação não foram alterados. Os ensaios com o recurso ligado e desligado têm o mesmo I/O de escrita; a leitura não acrescenta writes.

## Comparação de leitura: clusters de 512 bytes

Cada arquivo tem 1 MiB. Os dois leitores são alternados pelo harness comum. Foram sete repetições por caso, nos perfis de latência e de baixo atraso, com blocos de 257/4096 bytes e controles com clusters de 4096 bytes. As quatro variantes somam 448 volumes e 896 fases de escrita/leitura. Cada volume é auditado diretamente e remontado para conferir tamanho/conteúdo. Os resultados completos, fontes identificadas por hash e recursos medidos estão em [FAT32_MULTIREAD.json](FAT32_MULTIREAD.json).

As taxas abaixo são **MiB/s agregados no modelo de latência**. A configuração atual usa cache 4; foram comparados upstream com cache igualado em 4 e seu padrão de 8 páginas. O upstream continua deixando a FAT secundária desatualizada no ensaio de escrita; não é uma referência equivalente de persistência para writes. Os dados lidos após remontagem são conferidos em todas as variantes.

| Bloco | Leitores | Upstream cache 4 | Upstream cache 8 | Atual, recurso desligado | Atual, recurso ligado |
| --- | --- | --- | --- | --- | --- |
| 257 B | 1 | 2,202595 | 2,202595 | 2,202837 | **3,932982** |
| 257 B | 2 | 2,203565 | 2,203565 | 2,203808 | **2,203808** |
| 4096 B | 1 | 2,405234 | 2,405234 | 2,419960 | **4,683183** |
| 4096 B | 2 | 2,406391 | 2,406391 | 2,155033 | **2,155033** |

Com um leitor, são **78,56%** de ganho modelado sobre upstream com blocos de 257 bytes e **94,71%** com blocos de 4096 bytes. Frente à versão atual desligada, os ganhos são 78,54% e 93,52%, respectivamente. Com dois leitores, o fallback mantém o comportamento anterior: não elimina a diferença de aproximadamente 10,45% para upstream com blocos de 4096 bytes. Esse caso continua lendo 510 setores de dados adicionais por disputa do cache.

| Bloco/leitores | Setores lidos antes/depois | Sessões | Setores dentro de sessões | Comandos de leitura antes/depois |
| --- | --- | --- | --- | --- |
| 257 B / 1 | 2.066 / 2.066 | 17 | 2.031 | 2.066 / **52** |
| 4096 B / 1 | 2.066 / 2.066 | 17 | 2.031 | 2.066 / **52** |
| 257 B / 2 | 4.130 / 4.130 | 0 | 0 | 4.130 / 4.130 |
| 4096 B / 2 | 4.640 / 4.640 | 0 | 0 | 4.640 / 4.640 |

Com um leitor, os 52 comandos correspondem a 35 leituras individuais e 17 inícios multibloco; há também 17 terminações. Continuam existindo 2.066 callbacks de setor, para 2.048 setores de dados e 18 de metadados. O ganho resulta da amortização do comando, não da omissão de dados ou metadados.

### Premissas de tempo e custo de CPU

O perfil 1 decompõe a leitura simples de 200 µs em 100 µs de comando e 100 µs de transferência. Uma sessão custa 100 µs de início, 100 µs por setor e 100 µs de término. O poll continua em 10 µs. Esses valores são hipóteses explícitas do backend de teste; o modelo anterior sem capacidade de sessão não demonstraria ganho somente pela troca da API. Um driver/cartão com comando mais barato, parada mais cara ou transferências mais lentas terá outro resultado.

O perfil 0 usa um poll por transferência e não cobra início/fim, expondo mais o custo de CPU. Na mediana de sete repetições, a leitura de um arquivo com blocos de 257 bytes passa de 540,161 para 480,400 MiB/s no computador, cerca de 11,1% de redução. Com dois leitores passa de 514,284 para 513,532 MiB/s, cerca de 0,15% de redução. Essas taxas incluem harness, geração/verificação e callbacks; não são taxas físicas do cartão. A implementação troca custo de consulta por menos comandos e permanece opcional.

Nos controles com clusters de 4096 bytes, não são iniciadas sessões pela biblioteca. Todas as contagens e tempos virtuais permanecem iguais antes/depois. Todos os casos de escrita também preservam suas contagens e tempos virtuais.

## RAM e pilha

| ABI | RAM estática da biblioteca: antes / padrão / habilitado | Estado do simulador: antes/depois |
| --- | --- | --- |
| PIC24, enum normal | 2.494 / 2.494 / 2.494 B | 38 / 38 B |
| PIC24, enum curto | 2.474 / 2.474 / 2.474 B | 38 / 38 B |
| x86 | 2.612 / 2.612 / 2.612 B | 56 / 56 B |
| x64 | 2.720 / 2.720 / 2.720 B | 72 / 72 B |

Os handles e descritores também não mudam. Os buffers do backend/harness, o ponteiro FILE do simulador e a instrumentação não estão incluídos na coluna da biblioteca; o ponteiro e os contadores opcionais de diagnóstico do simulador não foram ampliados.

A inspeção PIC24 cobre enum normal e curto, incluindo funções clonadas pelo compilador. Nenhum prólogo de função existente cresceu na biblioteca ou no simulador. As consultas novas de limite e FAT usam 12 e 8 bytes e não chamam outras funções. Seus picos, incluindo o retorno de chamada, ficam abaixo do caminho já existente de `allocateCacheSector` (18 bytes) chamando `cacheSectorInit` (frame zero). As consultas são sequenciais, não somadas na pilha. `cacheSector` mantém 22 bytes com enum normal e 20 com enum curto. Os wrappers de início/fim têm frame zero.

No simulador, `beginReadBlocks` usa 34 bytes; `endReadBlocks` tem frame zero. O início cabe no caminho de chamada já utilizado pela leitura/escrita, sem buffers temporários adicionais; a função de leitura existente e as demais funções comuns mantêm seus frames. Isso não mede a pilha de libc, interrupções ou callbacks da aplicação. Um driver físico novo precisa preservar o contrato e verificar seu próprio orçamento de memória.

Como custo de código, a soma das seções executáveis dos objetos da biblioteca aumenta 1.156 bytes no x86 e 1.318 bytes no x64 com a opção habilitada. Com a opção desligada, a mesma soma permanece igual à anterior. Não é tamanho final de firmware linkado.

Compiladores: MSVC `/O2` e XC16 2.10 para PIC24FJ256GA702, `-Os` sujeito às limitações da licença disponível, com as mesmas opções antes/depois. O recurso não adiciona heap nem altera registros de disco.

## Testes e reprodução

Passaram as sete configurações de regressão com a opção habilitada: padrão, API original explícita, API de compatibilidade FAT, FAT/LFN, exFAT, exFAT/freefile e LFN/exFAT. Também passaram as regressões padrão com a opção desligada e os testes existentes de batching/pre-erase de escrita.

O teste `fat32_multiread.c` verifica limite de EOF, fim de página FAT, EOC, fragmentação, bits altos reservados, falhas de início, leitura parcial inválida seguida de retry, falha/ocupado de parada antes de close, idle, ausência de writes durante a leitura, fallback de dois leitores, remoção com leitura em voo, callbacks de geração antiga e remontagem com payload correto.

O teste `multiread_backend.c` exercita diretamente o simulador: limites da mídia, repetição do início sem ampliar a faixa, parada antecipada, requisição não consecutiva, escrita ocupada durante leitura, transições leitura/escrita, cancelamento da posse do buffer em reset e ausência de pre-erase nas faixas de leitura.

O teste `multiread_integration.c` liga a biblioteca ao simulador distribuído, sem backend falso. Cria uma imagem FAT32 com clusters de 512 bytes, grava dois arquivos, fecha e reabre a imagem, confirma sessões reais do simulador com um leitor e fallback com dois leitores, confere o payload em blocos de 257 bytes e a autorização de desligamento limpo. Ao final, compara diretamente todas as páginas das duas FATs na imagem. O teste de backend também confirma que destruir/reabrir a mídia não reaproveita uma sessão antiga.

```powershell
# before/lib e before/tests preservam as fontes anteriores à implementação.
tests/run_multiread_comparison.ps1
python tests/multiread_report.py --directory tests/build/fat32-multiread-20261116 --output tests/FAT32_MULTIREAD.json
tests/run_multiread_tests.ps1
tests/run_exfat_tests.ps1
tests/run_io_batching_tests.ps1
```

O validador exige sete repetições determinísticas, igualdade de RAM e de layout do backend, ausência de crescimento nos frames comuns e limite de pilha das novas consultas. Confere que os fontes de produção correspondem ao snapshot medido e que o recurso não altera I/O de escrita nem acrescenta leituras. As evidências brutas estão em `tests/build/fat32-multiread-20261116`.
