# Escrita com oito setores de cache

Rodada de 18/11/2026. FAT32 8.3, clusters de 512 bytes, oito setores de cache nas duas bibliotecas, três handles e escrita multibloco habilitada. Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`.

API original, LFN/exFAT/journal desligados no benchmark. Imagem sintética MBR com 65.530 clusters, sem FSInfo ou boot de backup. Dois escritores são intercalados na mesma thread; o harness termina cada bloco antes de tentar o próximo escritor.

MSVC x64 /O2, 21 repetições intercaladas em ordem alternada, afinidade fixa, arquivos de 1 MiB por escritor, sem compilações concorrentes. São ciclos do host por byte de payload agregado, incluindo backend simulado, polling, abertura/fechamento e preparação dos dados. A auditoria completa do volume ocorre fora da janela de escrita. Não são ciclos isolados da biblioteca nem medições em cartão físico/PIC24.

A hora FAT foi fixada em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. As fontes normais da biblioteca não foram modificadas. O benchmark também faz 32 passagens de leitura para conferir conteúdo após a escrita.

## CPU de escrita

A coluna CPU adicional compara a razão das medianas com upstream. Valores positivos indicam maior consumo. Os perfis multibloco abaixo habilitam a leitura multibloco opcional; a escrita multibloco já está ligada em todos os perfis, inclusive no padrão e no upstream. As opções de leitura também selecionam especializações existentes no caminho de escrita.

| Perfil atual | Bloco | Escritores | Atual, ciclos/byte | Upstream, ciclos/byte | CPU adicional |
|---|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 8.2192 | 3.8639 | +112.72% |
| Padrão | 257 | 2 | 7.4280 | 3.8219 | +94.35% |
| Padrão | 4096 | 1 | 6.0880 | 3.4506 | +76.44% |
| Padrão | 4096 | 2 | 6.3649 | 3.3717 | +88.77% |
| Leitura multibloco | 257 | 1 | 6.1582 | 3.8639 | +59.38% |
| Leitura multibloco | 257 | 2 | 6.4772 | 3.8219 | +69.48% |
| Leitura multibloco | 4096 | 1 | 5.5000 | 3.4506 | +59.39% |
| Leitura multibloco | 4096 | 2 | 5.6357 | 3.3717 | +67.14% |
| Leitura multibloco + consulta | 257 | 1 | 6.3732 | 3.8639 | +64.94% |
| Leitura multibloco + consulta | 257 | 2 | 6.7007 | 3.8219 | +75.32% |
| Leitura multibloco + consulta | 4096 | 1 | 5.5129 | 3.4506 | +59.77% |
| Leitura multibloco + consulta | 4096 | 2 | 5.7573 | 3.3717 | +70.75% |

## I/O durante a escrita

R/W contam setores de 512 bytes e incluem metadados e drenagem do fechamento. As contagens são determinísticas nas repetições. Os três perfis atuais têm o mesmo I/O de escrita neste ensaio.

| Bloco | Escritores | R atual / upstream | W atual / upstream | Metadados W atual / upstream | Polls atual / upstream |
|---|---:|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 | 10282 / 4165 |
| 257 | 2 | 34 / 34 | 12422 / 8261 | 8326 / 4165 | 20614 / 8327 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 | 6457 / 2853 |
| 4096 | 2 | 34 / 50 | 12420 / 5557 | 8324 / 1461 | 12970 / 5653 |

A auditoria das duas FATs passou em todas as execuções atuais. O upstream deixa diferenças entre as cópias nesse workload. Portanto, o ensaio não compara implementações com garantias de espelhamento iguais. Os contadores não isolam quanto da diferença vem de espelhamento, ordenação de persistência ou política de cache; não atribuir toda a diferença de CPU apenas à segunda FAT.

## RAM estática da biblioteca

Soma das seções graváveis de asyncfatfs e fat_standard, incluindo cache e alinhamento; exclui backend, mídia em RAM e buffers do aplicativo/harness. Não é somente sizeof do estado global. Os três perfis atuais mediram a mesma RAM.

| ABI | Atual | Upstream | Diferença |
|---|---:|---:|---:|
| x86 | 4760 B | 4668 B | +92 B |
| x64 | 4872 B | 4784 B | +88 B |
| PIC24, enums normais | 4614 B | 4544 B | +70 B |
| PIC24, enums curtos | 4586 B | 4520 B | +66 B |

## Pilha interna PIC24

XC16 2.10, PIC24FJ256GA702, C99, -Os. Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação e rotinas do compilador/libc, conforme o escopo acordado. Limite conservador de compilação, não execução em hardware.

| Biblioteca / perfil | Enums normais | Enums curtos |
|---|---:|---:|
| Padrão | 270 B | 264 B |
| Leitura multibloco | 270 B | 264 B |
| Leitura multibloco + consulta | 270 B | 264 B |
| Upstream/master | 200 B | 202 B |

As **48 variantes com oito setores** passaram; pico entre as configurações opcionais: **378 bytes**, estritamente abaixo de 400. Os configs opcionais foram copiados para matrix-work/tests e seu config_common foi fixado em oito setores apenas nessa cópia; os configs originais do repositório não foram alterados.

## Evidências e reprodução do relatório

Snapshots, objetos, trials, hashes, matriz e medições upstream PIC24: `tests/build/cache8-write-20261118`. normal/lib preserva a fonte normal; after/lib é a cópia do benchmark com hora controlada. O JSON conserva todas as amostras e contagens. Não comparar diretamente estes tempos com rodadas históricas de quatro setores para atribuir um ganho ao cache sem um ensaio pareado.

```powershell
python tests/cache8_write_report.py --directory tests/build/cache8-write-20261118 --repetitions 21 --output tests/CACHE8_WRITE_COMPARISON.md
```
