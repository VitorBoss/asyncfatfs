# Relação entre I/O e CPU: versão atual e upstream/master

FAT32 8.3, clusters de 512 bytes, API original, **oito setores de cache** nos dois códigos. Atual = fonte de WRITE_DISPATCH_CPU, sem alterações na biblioteca nesta investigação. Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`.

O I/O adicional é inteiramente de metadados: os dados são escritos exatamente uma vez em ambos. Mais transferências e etapas de confirmação produzem mais trabalho de CPU, mas os totais não crescem na mesma proporção. Há também uma amplificação no harness intercalado: ele regenera dados pendentes a cada retry. O experimento abaixo separa essa preparação de dados da sequência de operações da biblioteca.

## Método e limites

MSVC x64 /O2, 21 repetições pareadas, ordem alternada, afinidade fixa, 1 MiB por arquivo e 32 passagens de leitura para validar o conteúdo. Todas as compilações terminaram antes da medição. Ciclos incluem biblioteca, backend simulado e harness; não são tempos exclusivos da biblioteca nem medições em PIC24 ou cartão físico.

A CPU foi medida sem os novos contadores. Executáveis separados COMPARISON_METADATA_TRACE + COMPARISON_CPU_WORK_AUDIT contam FAT primária, espelho, diretório, bytes gerados para escrita, tentativas fwrite e chamadas pump. O timing desses executáveis foi descartado. Seus contadores de I/O e polling coincidem com os dos executáveis de CPU.

O perfil CPU usa latência virtual mínima: cada transferência termina no próximo sdcard_poll. Não mede espera física do cartão. Portanto `polls − (leituras + escritas)` conta polls sem conclusão de setor nesse perfil; não representa espera medida de hardware.

## I/O e CPU por bloco

Valores nas células: atual / upstream. CPU é a mediana de ciclos por byte aceito. I/Os são leituras + escritas de setores de 512 bytes no backend simulado, não comandos físicos do cartão nem programações NAND. As razões são descritivas, não coeficientes causais de custo por operação.

| Bloco | Escritores | I/Os | Razão I/O | CPU ciclos/B | Razão CPU | Polls | Polls sem conclusão de setor |
|---|---:|---:|---:|---:|---:|---:|---:|
| 257 | 1 | 6199 / 4149 | 1.494× | 6.8967 / 3.8868 | 1.774× | 10282 / 4165 | 4083 / 16 |
| 257 | 2 | 12456 / 8295 | 1.502× | 7.1747 / 3.8611 | 1.858× | 20614 / 8327 | 8158 / 32 |
| 4096 | 1 | 6199 / 2823 | 2.196× | 5.8970 / 3.4704 | 1.699× | 6457 / 2853 | 258 / 30 |
| 4096 | 2 | 12454 / 5607 | 2.221× | 6.1613 / 3.4040 | 1.810× | 12970 / 5653 | 516 / 46 |

Com 257 bytes, muitos polls adicionais não concluem setor; o custo inclui retomadas da máquina de estados e novas tentativas de escrita. Com 4096 bytes, o I/O aumenta proporcionalmente mais que a CPU. Isso impede explicar o tempo apenas multiplicando ciclos por número de setores gravados.

| Bloco | Escritores | Pedidos lógicos mínimos | Tentativas fwrite atual/up | Tentativas além do mínimo atual/up |
|---|---:|---:|---:|---:|
| 257 | 1 | 4081 | 10274 / 4161 | 6193 / 80 |
| 257 | 2 | 8162 | 20603 / 8323 | 12441 / 161 |
| 4096 | 1 | 256 | 6449 / 2846 | 6193 / 2590 |
| 4096 | 2 | 512 | 12959 / 5646 | 12447 / 5134 |

As tentativas excedentes incluem retornos parciais e tentativas sem aceitar bytes; não são uma contagem isolada de BUSY. No serial, os dois códigos geram exatamente 1× o payload, portanto essa tabela não tem a regeneração do modo intercalado histórico.

## De onde vêm as gravações adicionais

Contagem de transferências por destino, por bloco. Não confundir UPDATE_FAT1/UPDATE_FAT2 (EOC/vínculo) com as duas cópias da FAT.

| Bloco | Escritores | Dados atual/up | FAT primária atual/up | Espelho atual/up | Diretório atual/up | W adicional total |
|---|---:|---:|---:|---:|---:|---:|
| 257 | 1 | 2048 / 2048 | 2065 / 1056 | 2065 / 0 | 3 / 1027 | 2050 = 1009 + 2065 + (-1024) |
| 257 | 2 | 4096 / 4096 | 4161 / 2129 | 4161 / 0 | 4 / 2036 | 4161 = 2032 + 4161 + (-2032) |
| 4096 | 1 | 2048 / 2048 | 2065 / 378 | 2065 / 0 | 3 / 363 | 3392 = 1687 + 2065 + (-360) |
| 4096 | 2 | 4096 / 4096 | 4160 / 746 | 4160 / 0 | 4 / 715 | 6863 = 3414 + 4160 + (-711) |

O espelho explica parte da diferença. A primária também pode ser gravada mais vezes: a versão atual confirma EOC nas duas FATs antes de publicar o vínculo e confirma o vínculo antes do diretório. O upstream pode coalescer alterações ainda não confirmadas e não atualiza a segunda FAT nesse workload. Em contrapartida, a política atual pode reduzir publicações intermediárias do diretório. A decomposição acima separa esses efeitos em vez de atribuir todo o excesso à segunda cópia.

## Amplificação de trabalho no harness intercalado

O modo histórico gera `amount − written[index]` bytes antes de cada tentativa, inclusive quando fwrite não aceita nada. O modo preparado mantém os padrões distintos por arquivo, mas os gera uma única vez por pedido em buffers separados. Não altera a biblioteca, a ordem de tentativas ou o loop de polling. I/O, polling, tentativas fwrite e chamadas pump são **exatamente iguais** nos dois modos, por versão.

| Bloco | Escritores | Bytes gerados/payload histórico atual/up | Tentativas fwrite atual/up | CPU histórico atual/up | CPU preparado atual/up | CPU extra ao upstream histórico → preparado |
|---|---:|---:|---:|---:|---:|---:|
| 257 | 1 | 1.76× / 1.02× | 10274 / 4161 | 8.4077 / 4.0281 | 6.9848 / 4.0533 | +108.73% → +72.32% |
| 257 | 2 | 2.27× / 1.11× | 28866 / 9954 | 9.2588 / 4.1035 | 6.9029 / 3.9662 | +125.63% → +74.05% |
| 4096 | 1 | 14.57× / 6.08× | 6449 / 2846 | 23.4993 / 10.1424 | 5.9699 / 3.6123 | +131.69% → +65.26% |
| 4096 | 2 | 27.93× / 11.74× | 24768 / 10397 | 40.8446 / 18.3634 | 6.0508 / 4.5621 | +122.42% → +32.63% |

Nos buffers preparados, bytes gerados/payload = **1× nos dois códigos**, assim como no teste serial. Para dois escritores/4096, a diferença absoluta de CPU cai de 22.4812 para 1.4887 ciclos/B (93.38% menor), sem reduzir I/O. Essa é a diferença observada ao remover a regeneração e mudar a preparação/cache dos buffers; não é um percentual de CPU exclusiva da biblioteca nem uma melhoria no código FAT.

Não misturar os destinos da tabela serial com os do teste intercalado. Para dois escritores/4096 nos modos intercalado histórico e preparado:

| Versão | R | W dados | W primária | W espelho | W diretório | Polls | Sequências/setores multibloco de escrita |
|---|---:|---:|---:|---:|---:|---:|---:|
| current | 34 | 4096 | 4128 | 4128 | 291 | 12937 | 288 / 576 |
| upstream | 453 | 4096 | 831 | 0 | 707 | 6410 | 0 / 0 |

A auditoria encontrou **0 gravações FAT atuais com imagem inteira idêntica à já armazenada**, nos casos serial e intercalado. Isso não prova que o número de gravações seja mínimo: imagens diferentes ainda podem ser coalescidas quando a ordenação permitir. O teste não contou imagens idênticas do diretório.

## Relação no percurso do código

| Trabalho | Relação com I/O/CPU |
|---|---|
| Atualizar FAT | A versão atual adquire/mantém duas páginas e, no padrão, copia a imagem para o espelho; upstream altera a primária. Isso custa CPU antes da submissão. |
| Submeter e concluir setor | Cada transferência executa busca/estado de cache, backend, cópia de 512 bytes no simulador e callback. Mais setores implicam mais eventos. |
| Esperar barreiras e retomar | Pode exigir polls e novas entradas em fwrite sem concluir outro setor. É trabalho adicional além da transferência. |
| Coalescer metadados | Muda o número de escritas por byte útil; depende de cache, tamanho do pedido e garantias de ordenação. |
| Preparar dados da aplicação | No harness histórico intercalado, retries induzidos por I/O repetem a geração do restante do bloco, amplificando a CPU medida. |

Há controles anteriores com I/O constante e CPU diferente: o atalho de despacho de WRITE_DISPATCH_CPU reduziu CPU sem alterar os contadores; os perfis multibloco atuais também têm o mesmo I/O de escrita do padrão, com custos de CPU diferentes. Logo a correlação é real, mas não determina sozinha o custo de execução.

Não é possível atribuir com estes totais uma fração exata da CPU ao driver, aos callbacks ou à segunda FAT. Dividir ciclos por transferências/polls dá médias do workload inteiro, não o custo marginal de cada evento. Também não se deve comparar as duas implementações como se oferecessem a mesma persistência.

## Integridade, RAM e reprodução

Todos os executáveis verificaram o conteúdo dos arquivos e a imagem do volume. A atual manteve as duas FATs iguais. Os objetos da biblioteca têm instruções idênticas entre os três modos e entre CPU/auditoria, por versão. A fonte da biblioteca tem o mesmo hash da última matriz validada em WRITE_DISPATCH_CPU; não houve mudança de RAM, cache, pilha ou persistência. A matriz PIC24 anterior continua aplicável à mesma fonte (máximo 378 bytes internos); não foi recompilada nesta análise.

Os contadores extras existem somente no harness de auditoria. O modo preparado usa três buffers locais de 4096 bytes na aplicação de teste Windows; eles não entram na RAM estática ou pilha interna da biblioteca e não fazem parte da produção. As macros são opcionais; o modo histórico permanece disponível para reproduzir relatórios antigos.

`tests/build/io-cpu-relation` conserva fontes congeladas das bibliotecas, harness original/modificado, executáveis de CPU/auditoria, builds e trials. O JSON ao lado conserva todas as amostras não instrumentadas e somente os contadores das auditorias (seus timings não foram usados).

```powershell
./tests/run_io_cpu_relation.ps1 -OutputDirectory tests/build/io-cpu-relation -Repetitions 21
python tests/io_cpu_relation_report.py --directory tests/build/io-cpu-relation --repetitions 21 --output tests/IO_CPU_RELATION.md
```
