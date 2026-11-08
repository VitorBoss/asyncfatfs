# Prefetch entre clusters e leitura multibloco

Data: 16/11/2026. Investigação na configuração mínima FAT32 8.3, cache de quatro setores, clusters de 512/4096 bytes, com comparação a `upstream/master` local no commit `60c5f3580152410cb08342942cbbe3a30a449c24`.

Atualização posterior: o suporte opcional de sessão multibloco, sem ampliar a janela de prefetch, foi implementado e validado em [FAT32_MULTIREAD.md](FAT32_MULTIREAD.md). Este documento preserva o experimento anterior de janela ampliada.

A leitura de vários clusters antecipadamente é possível com as páginas existentes, mas isso não equivale a ler vários setores em um único comando do cartão. A interface atual só oferece `sdcard_readBlock()`, de 512 bytes; apenas a escrita possui início/fim de uma sessão multibloco. Uma janela experimental de dois/três setores atravessando clusters foi testada separadamente. Ela melhorou o modelo com um leitor e blocos de 257 bytes, mas causou forte regressão com dois leitores e aumentou o frame PIC24 da função. Por isso **nenhuma alteração desta investigação foi promovida à biblioteca**.

## O que a implementação atual faz

`afatfs_fprefetch()` solicita o setor do cursor quando alinhado, ou o setor seguinte quando o cursor está no meio de um setor. No segundo caso, retorna se o destino pertence a outro cluster. Não move o cursor e não mantém um plano de leitura futura.

Porém, ao consumir um setor inteiro, `afatfs_fread()` primeiro chama `afatfs_fseekInternal()` e só depois chama o prefetch. Quando a FAT está disponível, o seek já mudou `cursorCluster` para o próximo cluster. Portanto o próximo cluster **já é lido nesse caminho**, inclusive com clusters de 512 bytes. Quando falta a página FAT, a operação de seek precisa terminá-la antes de continuar. O limite explícito da função importa principalmente para antecipar a fronteira enquanto ainda se consome o setor atual.

O commit upstream consultado não contém a função de prefetch. Isso não impede que a leitura sob demanda alcance o mesmo número mínimo de setores lidos; o benefício potencial do prefetch está na antecipação e na sobreposição com o trabalho do consumidor, não na eliminação dos bytes obrigatórios do arquivo.

Com 1 MiB e clusters de 512 bytes, a versão atual lê 2.048 setores de dados e 18 de metadados: 17 da FAT e um do diretório. Cada setor de dados é lido uma vez. Dois arquivos somam 4.096 setores de dados e 34 de metadados. Nesse caso, reduzir leituras exige evitar metadados ou mudar a forma de transferência; não há releituras de dados a eliminar na carga de 257 bytes.

## Experimento executado

Foram preservadas cópias de `lib` em `tests/build/fat32-read-ahead-20261116/current`, `window` e `upstream`. O [JSON](FAT32_READ_AHEAD.json) inclui seus hashes e todos os resultados. A biblioteca de trabalho foi conferida contra a cópia `current` ao terminar.

O protótipo FAT32 normal percorre a cadeia real na FAT, sem presumir que o próximo cluster seja contíguo. A janela tem limite de dois ou três setores, para reservar parte das quatro páginas ao restante do trabalho. Cada chamada termina ao encontrar um setor ainda não pronto; o backend continua aceitando uma transferência de 512 bytes por vez. Também foi experimentada a chamada automática após leituras parciais, para antecipar a fronteira antes de liberar o setor atual.

Um teste separado chamou o prefetch repetidamente com o cursor parado e confirmou **três clusters de 512 bytes simultaneamente no cache**, cursor inalterado, nenhuma operação de arquivo enfileirada e conteúdo correto ao consumir o arquivo. Isso demonstra a possibilidade de preencher a janela por chamadas sucessivas; não demonstra uma transferência CMD18 nem três leituras em voo simultaneamente.

O benchmark usa 1 MiB por arquivo, um/dois leitores alternados, blocos de 257/4096 bytes, clusters de 512/4096 bytes e dois perfis de latência. Foram sete repetições em quatro variantes: 448 volumes e 896 fases de escrita/leitura. Cada volume foi auditado e remontado para conferir os dados. O upstream usa cache igualado em quatro setores; sua FAT secundária continua desatualizada no ensaio de escrita, como nas comparações anteriores. O foco desta tabela é a leitura posterior de dados válidos.

## Resultados para clusters de 512 bytes

Cada célula mostra **setores lidos / MiB/s modelados**. O perfil atribui 200 µs por leitura de setor e 10 µs por poll. As janelas de dois e três setores tiveram os mesmos resultados neste escalonamento: a primeira ausência no cache encerra a chamada e a frequência das chamadas não permite aproveitar uma janela maior. As contagens foram determinísticas nas sete repetições.

| Bloco | Leitores | Upstream, cache 4 | Atual, cache 4 | Janela experimental 2 ou 3 |
| --- | --- | --- | --- | --- |
| 257 B | 1 | 2.066 / 2,202595 | 2.066 / 2,202837 | 2.066 / 2,419843 |
| 257 B | 2 | 4.130 / 2,203565 | 4.130 / 2,203808 | 10.201 / 0,980248 |
| 4096 B | 1 | 2.066 / 2,405234 | 2.066 / 2,419960 | 2.066 / 2,419960 |
| 4096 B | 2 | 4.130 / 2,406391 | 4.640 / 2,155033 | 4.640 / 2,155033 |

Com um leitor e blocos de 257 bytes, antecipar a fronteira reduz os polls de 45.396 para 41.325 e eleva o throughput modelado em **9,85%** frente à versão atual. As leituras permanecem em 2.066; o ganho corresponde a esperas entre chamadas do harness, não a uma redução do custo de comando no dispositivo. As taxas são de um modelo, não de um cartão físico.

Com dois leitores, a janela disputa o cache com o outro arquivo e com a FAT. As leituras de metadados passam de 34 para 2.065; as de dados passam de 4.096 para 8.136. São 6.071 leituras adicionais no total e queda de aproximadamente **55,5%** no throughput modelado. Uma janela mais larga sem orçamento por consumidor não é uma otimização segura de desempenho para o cache atual.

O controle com dois leitores e blocos de 4.096 bytes continua com 510 releituras de dados a mais que upstream; o protótipo não corrige esse problema. Ele precisa ser tratado como disputa/substituição de páginas, separadamente do suporte a comandos multibloco.

Nos controles com clusters de 4.096 bytes e blocos de 257 bytes, um leitor passa de 2.305 leituras / 1,993024 MiB/s para as mesmas 2.305 / 2,169009 MiB/s. Dois leitores pioram de 4.357 leituras para 5.377. Os resultados completos estão no JSON; não houve melhora universal.

## Memória e motivo para não promover o protótipo

Não foram acrescentados campos ou buffers no protótipo; a estrutura global continua contendo as mesmas quatro páginas. Porém, na compilação PIC24 normal com XC16 2.10 e as opções disponíveis na licença, o frame de `afatfs_fprefetch()` passa de **14 para 34 bytes**. Além desses 20 bytes locais/salvos adicionais, a travessia acrescenta chamadas de consulta FAT ao caminho. Não foi demonstrado o mesmo orçamento de pilha. A biblioteca de produção permanece byte a byte igual à versão anterior a esta investigação.

O protótipo é intencionalmente restrito a arquivos normais FAT32; não foi qualificado para FAT16, diretórios, LFN ou exFAT. Os ensaios de conteúdo/remontagem e o teste de janela não substituem a validação completa necessária para uma alteração de produção.

## O que permitiria ler como a escrita sequencial

É tecnicamente possível usar uma sessão de leitura SD de múltiplos blocos, tipicamente CMD18 no backend apropriado, atravessando a fronteira lógica de clusters. A [especificação física simplificada da SD Association, versão 7.10](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver7.10.pdf) descreve CMD18 como transferência contínua de blocos até sua interrupção. O cartão só precisa de endereços consecutivos; quem precisa verificar o encadeamento FAT é a biblioteca. Clusters fisicamente fragmentados exigem interromper a sessão e abrir outra faixa.

Uma extensão compatível com a arquitetura atual seria uma capacidade opcional `sdcard_beginReadBlocks(start, count)` / `sdcard_endReadBlocks()`, continuando a entregar cada setor ao buffer e callback existentes. Isso evita exigir um novo buffer grande e contíguo: os slots de cache escolhidos para LBAs consecutivos podem estar separados na RAM. A interface atual não oferece essa capacidade. O repositório contém o simulador e os contratos de backend, mas não um driver físico SD que demonstre esse modo em hardware.

Para manter o orçamento e as garantias, o projeto precisa:

- Descobrir uma faixa **confirmada pela FAT**, limitando-a às páginas de dados disponíveis e sem expulsar páginas retidas de outro leitor ou metadados necessários para continuar a faixa.
- Restringir/desativar a antecipação quando há mais de um consumidor ou pressão de escrita. Somente aumentar a janela produz a regressão medida acima; uma política adaptativa ainda precisa ser validada.
- Garantir um callback por setor, preservar a geração completa de mídia e a posse de cada buffer até seu término. Falha no meio do lote não pode apresentar como válidos os setores ainda não entregues.
- Encerrar a sessão antes de leitura não consecutiva, escrita, seek incompatível, fechamento e reset/remoção de mídia, permitindo saída antecipada sem depender de consumir todo o lote anunciado.
- Reutilizar estado já reservado pelo backend/cache, medindo RAM estática e pilha nos ABIs relevantes. A disponibilidade de campos do modo de escrita depende do driver real e não foi presumida.
- Medir separadamente latência de abrir o comando, custo por bloco e término da sessão. O modelo atual cobra 200 µs independentes por setor; ele não pode comprovar o ganho de CMD18 atribuindo arbitrariamente uma latência menor.

Uma leitura multibloco economiza comandos, não elimina os setores necessários. Sem suporte novo no backend, ampliar `afatfs_fprefetch()` apenas antecipa comandos individuais. A investigação confirma a viabilidade funcional de atravessar vários clusters, mas rejeita a janela ampla testada como mudança padrão devido à disputa de cache e ao aumento de pilha.

## Reprodução

```powershell
tests/run_read_ahead_experiment.ps1
```

O script prepara as cópias isoladas, compila os quatro benchmarks, executa sete repetições, compara os frames PIC24, testa a janela de três clusters e valida o JSON com `tests/read_ahead_report.py`. Nenhum arquivo em `lib` é editado por esse procedimento. O gerador recusa contagens não determinísticas ou uma biblioteca de trabalho diferente do snapshot atual.
