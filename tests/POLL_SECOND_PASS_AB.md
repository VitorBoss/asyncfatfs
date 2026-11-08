# A/B: segunda passagem de operações no poll

## Resultado

**Manter a segunda passagem condicional (A).** B consumiu mais CPU em todos os casos medidos e não reduziu I/O ou o número de polls.

- Por bloco, todos os perfis: B usou **3.59% a 6.81% mais CPU** que A.
- Pedidos sobrepostos, todos os perfis: B usou **1.05% a 5.03% mais CPU** que A.

A checagem final de grupo continuou em B. Retirar o filtro anterior não eliminou todas as consultas de agrupamento; acrescentou a passagem de arquivos quando A a dispensava. Não foi observado benefício desse trabalho adicional nos workloads testados. A condição é útil como filtro de escalonamento, sem ser ela própria uma barreira de persistência.

Rodada de 18/11/2026. FAT32 8.3, clusters/setores de 512 bytes, oito setores de cache, um/dois arquivos de 1 MiB, blocos de 257/4096 bytes. Escrita multibloco habilitada nos dois lados.

**A** é o código recebido: segunda passagem condicionada a páginas sujas e grupo de alocações pendentes, incluindo o retorno antecipado quando falso. **B** substitui esse if/else por uma chamada incondicional a afatfs_fileOperationsPoll(). A consulta final de grupo antes do flush permanece. O caminho com gravação ativa no backend continua executando uma única passagem. Portanto B significa incondicional naquele ponto READY/CLOSING, não em todos os polls.

Esta comparação mede as duas políticas completas nesse ponto, incluindo seus efeitos no escalonamento; não isola apenas as instruções de uma comparação booleana. Nenhuma barreira, ACK, flush explícito, reserva ou regra de espelhamento foi removida.

MSVC x64 /O2, 21 repetições intercaladas, ordem invertida em rodadas alternadas e afinidade fixa; sem compilações concorrentes. Hora FAT fixada em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. Os ciclos do host incluem biblioteca, backend, polling e preparação dos dados. A auditoria completa do volume fica fora da janela de escrita; os arquivos também são relidos em 32 passagens. Não são medições em cartão físico ou PIC24.

Percentual positivo na coluna CPU B/A significa **B mais caro**. É a mediana das razões pareadas; não necessariamente a razão das medianas de ciclos/byte. Pequenas diferenças devem ser interpretadas considerando o ruído do host.

## Por bloco

O harness termina o bloco de um escritor antes de tentar o outro.

| Perfil | Bloco | Escritores | A ciclos/byte | B ciclos/byte | CPU B/A |
|---|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 7.2921 | 7.7907 | +6.16% |
| Padrão | 257 | 2 | 7.6477 | 8.2707 | +6.81% |
| Padrão | 4096 | 1 | 6.2667 | 6.5386 | +4.39% |
| Padrão | 4096 | 2 | 6.5476 | 6.8198 | +3.59% |
| Leitura multibloco | 257 | 1 | 6.4737 | 6.8821 | +6.35% |
| Leitura multibloco | 257 | 2 | 6.7678 | 7.1488 | +5.79% |
| Leitura multibloco | 4096 | 1 | 5.7184 | 5.9724 | +3.91% |
| Leitura multibloco | 4096 | 2 | 5.9006 | 6.1336 | +3.69% |
| Leitura multibloco + consulta | 257 | 1 | 6.6162 | 6.9011 | +4.23% |
| Leitura multibloco + consulta | 257 | 2 | 6.8811 | 7.3474 | +6.11% |
| Leitura multibloco + consulta | 4096 | 1 | 5.6921 | 6.0193 | +5.23% |
| Leitura multibloco + consulta | 4096 | 2 | 6.0190 | 6.2436 | +4.52% |

I/O de escrita em setores de 512 bytes, incluindo fechamento e drenagem. Valores nas células: **A / B**.

| Perfil | Bloco | Escritores | R | W | Metadados W | Polls |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Padrão | 257 | 2 | 34 / 34 | 12422 / 12422 | 8326 / 8326 | 20614 / 20614 |
| Padrão | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Padrão | 4096 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 12970 / 12970 |
| Leitura multibloco | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Leitura multibloco | 257 | 2 | 34 / 34 | 12422 / 12422 | 8326 / 8326 | 20614 / 20614 |
| Leitura multibloco | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Leitura multibloco | 4096 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 12970 / 12970 |
| Leitura multibloco + consulta | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Leitura multibloco + consulta | 257 | 2 | 34 / 34 | 12422 / 12422 | 8326 / 8326 | 20614 / 20614 |
| Leitura multibloco + consulta | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Leitura multibloco + consulta | 4096 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 12970 / 12970 |

## Pedidos sobrepostos

COMPARISON_INTERLEAVED_REQUESTS tenta atender ambos os escritores antes de aguardar progresso. Não atribuir diferenças entre os dois escalonamentos à alteração da biblioteca.

| Perfil | Bloco | Escritores | A ciclos/byte | B ciclos/byte | CPU B/A |
|---|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 8.5099 | 8.9232 | +5.03% |
| Padrão | 257 | 2 | 9.2680 | 9.7026 | +4.79% |
| Padrão | 4096 | 1 | 23.5321 | 23.7635 | +1.05% |
| Padrão | 4096 | 2 | 40.7177 | 41.1361 | +1.09% |
| Leitura multibloco | 257 | 1 | 7.6485 | 7.9221 | +3.89% |
| Leitura multibloco | 257 | 2 | 8.9357 | 9.3641 | +4.56% |
| Leitura multibloco | 4096 | 1 | 22.8171 | 23.1302 | +1.32% |
| Leitura multibloco | 4096 | 2 | 40.4495 | 40.9417 | +1.21% |
| Leitura multibloco + consulta | 257 | 1 | 7.8373 | 8.0957 | +3.15% |
| Leitura multibloco + consulta | 257 | 2 | 8.9723 | 9.4018 | +4.65% |
| Leitura multibloco + consulta | 4096 | 1 | 22.9667 | 23.2516 | +1.37% |
| Leitura multibloco + consulta | 4096 | 2 | 40.5166 | 40.9895 | +1.19% |

I/O de escrita em setores de 512 bytes, incluindo fechamento e drenagem. Valores nas células: **A / B**.

| Perfil | Bloco | Escritores | R | W | Metadados W | Polls |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Padrão | 257 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 16539 / 16539 |
| Padrão | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Padrão | 4096 | 2 | 34 / 34 | 12643 / 12643 | 8547 / 8547 | 12937 / 12937 |
| Leitura multibloco | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Leitura multibloco | 257 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 16539 / 16539 |
| Leitura multibloco | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Leitura multibloco | 4096 | 2 | 34 / 34 | 12643 / 12643 | 8547 / 8547 | 12937 / 12937 |
| Leitura multibloco + consulta | 257 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 10282 / 10282 |
| Leitura multibloco + consulta | 257 | 2 | 34 / 34 | 12420 / 12420 | 8324 / 8324 | 16539 / 16539 |
| Leitura multibloco + consulta | 4096 | 1 | 18 / 18 | 6181 / 6181 | 4133 / 4133 | 6457 / 6457 |
| Leitura multibloco + consulta | 4096 | 2 | 34 / 34 | 12643 / 12643 | 8547 / 8547 | 12937 / 12937 |

## RAM, pilha e persistência

RAM estática da biblioteca x64: **4.872 bytes em A e B**, nos três perfis e nos dois escalonamentos. Não houve acréscimo de buffer, campo ou heap. As 48 comparações de seções estáticas PIC24 entre A/B também foram iguais.

Pilha interna conservadora máxima: **A 378 B / B 378 B**, abaixo de 400 bytes. XC16 2.10, PIC24FJ256GA702, C99 -Os, enums normais/curtos. Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação e rotinas do compilador/libc conforme o escopo acordado.

Em cada variante passaram 11 cenários gerais e 10 verificações de alocação: ordenação dos vínculos antes de cada write, três arquivos, pedidos sobrepostos, volume quase cheio, espelho e reserva única, além de falhas de I/O/ACK, remoção/reinserção, remontagem e desligamento limpo. A auditoria de conteúdo e das duas FATs passou em todas as execuções do benchmark. Esses resultados validam os cenários exercitados; não são uma prova de todas as interrupções possíveis.

Os testes opcionais e a matriz usaram cópias de config_common com oito setores. As fontes e configurações do workspace não foram substituídas pelo experimento.

## Evidências

`tests/build/poll-second-pass-ab-20261118`: A/lib e B/lib são fontes normais; before/lib e after/lib são as cópias com hora controlada do benchmark. matrix/report.json contém os limites e hashes. A/build e B/build contêm as regressões. Trials por bloco estão na raiz e pedidos sobrepostos em interleaved/. O JSON ao lado deste relatório conserva todas as amostras, contagens e medições de RAM.

```powershell
python tests/poll_second_pass_ab_report.py --directory tests/build/poll-second-pass-ab-20261118 --repetitions 21 --output tests/POLL_SECOND_PASS_AB.md
```
