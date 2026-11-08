# CPU de escrita: continuação direta da alocação no escalonador

A análise posterior [IO_CPU_RELATION.md](IO_CPU_RELATION.md) separa o custo de
metadados e mostra a regeneração de payload no harness intercalado histórico.
Os ciclos deste relatório incluem esse trabalho da aplicação de teste; não são
tempos exclusivos da biblioteca.

FAT32 8.3, clusters de 512 bytes, **oito setores de cache**, API original. Referência local upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24` (blob asyncfatfs.c `81a8b854a345e329e0c289798ff6d82ef31dcf4f`, conferido).

No pipeline nativo com cache de pelo menos oito setores e sem leitura multibloco, fileOperationsPoll chama appendRegularFreeClusterContinue diretamente para APPEND_FREE_CLUSTER com tipo válido. Evita o despacho genérico de opcode nessa operação frequente. Mantém a checagem inicial de operação ativa, a validação de tipo, o dispatcher para os demais casos, a ordem dos arquivos e a quantidade de passagens por poll.

A continuação é exatamente a mesma; não foram alteradas fases, ACKs, reserva única, duas FATs, publicação do diretório, fechamento ou backend. As builds x64 multibloco, com/sem consulta, preservam **todas as instruções dos objetos da biblioteca/harness e dos executáveis ligados**, verificadas por hashes. Variações de timing nesses pares idênticos não são ganhos novos.

MSVC x64 /O2, 21 repetições pareadas, ordem alternada, afinidade fixa e arquivos de 1 MiB por escritor. 32 passagens de leitura conferem os dados. Medições sem compilações concorrentes. Hora FAT fixada em 12:00:00 somente nos snapshots COMPARISON_CPU_ONLY. Ciclos incluem biblioteca, backend simulado, polling e harness; não são medições em PIC24 ou cartão físico. A redução é a mediana das razões pareadas; CPU adicional ao upstream usa a razão das medianas. Redução positiva significa menos CPU; valores pequenos podem refletir ruído do host.

## Por bloco

Cada pedido de um escritor termina antes de tentar o próximo.

| Perfil | Bloco | Escritores | Antes ciclos/B | Depois ciclos/B | Redução pareada | Upstream ciclos/B | CPU adicional upstream antes → depois |
|---|---:|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 7.0552 | 6.8638 | +3.20% | 3.8871 | +81.50% → +76.58% |
| Padrão | 257 | 2 | 7.4530 | 7.1720 | +3.59% | 3.8387 | +94.15% → +86.83% |
| Padrão | 4096 | 1 | 6.1057 | 5.9008 | +3.45% | 3.5061 | +74.15% → +68.30% |
| Padrão | 4096 | 2 | 6.3402 | 6.1332 | +3.09% | 3.4083 | +86.02% → +79.95% |
| Multibloco | 257 | 1 | 6.2197 | 6.2374 | -0.50% | 3.8871 | +60.01% → +60.47% |
| Multibloco | 257 | 2 | 6.5229 | 6.5320 | -0.22% | 3.8387 | +69.92% → +70.16% |
| Multibloco | 4096 | 1 | 5.5444 | 5.5927 | +0.49% | 3.5061 | +58.14% → +59.52% |
| Multibloco | 4096 | 2 | 5.7217 | 5.7026 | +0.59% | 3.4083 | +67.87% → +67.31% |
| Multibloco + consulta | 257 | 1 | 6.1789 | 6.1891 | -0.72% | 3.8871 | +58.96% → +59.22% |
| Multibloco + consulta | 257 | 2 | 6.5537 | 6.5597 | -0.47% | 3.8387 | +70.72% → +70.88% |
| Multibloco + consulta | 4096 | 1 | 5.5345 | 5.4613 | +0.81% | 3.5061 | +57.85% → +55.77% |
| Multibloco + consulta | 4096 | 2 | 5.7161 | 5.6826 | -0.23% | 3.4083 | +67.71% → +66.73% |

I/O em setores de 512 bytes, incluindo fechamento e drenagem. **Antes/depois idênticos** em leituras, gravações, metadados, polling, sessões e tempo virtual para cada perfil/caso. Comparação padrão/upstream:

| Bloco | Escritores | R atual/upstream | W atual/upstream | Metadados W atual/upstream |
|---|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 |
| 257 | 2 | 34 / 34 | 12422 / 8261 | 8326 / 4165 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 |
| 4096 | 2 | 34 / 50 | 12420 / 5557 | 8324 / 1461 |

Controles de leitura padrão, redução pareada frente ao baseline:

| Bloco | Leitores | Redução de CPU |
|---|---:|---:|
| 257 | 1 | +1.70% |
| 257 | 2 | +0.59% |
| 4096 | 1 | +0.99% |
| 4096 | 2 | -0.01% |
## Pedidos intercalados

COMPARISON_INTERLEAVED_REQUESTS tenta atender ambos os escritores antes de aguardar progresso.

| Perfil | Bloco | Escritores | Antes ciclos/B | Depois ciclos/B | Redução pareada | Upstream ciclos/B | CPU adicional upstream antes → depois |
|---|---:|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 9.4003 | 8.5665 | +5.48% | 4.0787 | +130.47% → +110.03% |
| Padrão | 257 | 2 | 10.1917 | 9.1739 | +9.86% | 4.1551 | +145.28% → +120.78% |
| Padrão | 4096 | 1 | 24.6205 | 23.4927 | +4.57% | 10.1579 | +142.38% → +131.28% |
| Padrão | 4096 | 2 | 41.7946 | 40.7775 | +2.46% | 18.3360 | +127.94% → +122.39% |

I/O em setores de 512 bytes, incluindo fechamento e drenagem. **Antes/depois idênticos** em leituras, gravações, metadados, polling, sessões e tempo virtual para cada perfil/caso. Comparação padrão/upstream:

| Bloco | Escritores | R atual/upstream | W atual/upstream | Metadados W atual/upstream |
|---|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 |
| 257 | 2 | 34 / 35 | 12420 / 5792 | 8324 / 1696 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 |
| 4096 | 2 | 34 / 453 | 12643 / 5634 | 8547 / 1538 |

Controles de leitura padrão, redução pareada frente ao baseline:

| Bloco | Leitores | Redução de CPU |
|---|---:|---:|
| 257 | 1 | +1.00% |
| 257 | 2 | +2.11% |
| 4096 | 1 | +0.67% |
| 4096 | 2 | +1.67% |

Upstream não atualiza a segunda FAT nesse workload. A diferença restante inclui as garantias adicionais de espelhamento e ordenação; a otimização do dispatcher não reduz essa quantidade de trabalho.

## RAM, código e pilha

RAM estática da biblioteca, incluindo buffers e alinhamento; exclui backend e aplicação. Código contado nos objetos da biblioteca.

| ABI/perfil | RAM antes → depois / upstream | Código antes → depois / upstream |
|---|---:|---:|
| x64/Padrão | 4872 → 4872 / 4784 B | 36190 → 36036 / 17355 B |
| x64/Multibloco | 4872 → 4872 / 4784 B | 37680 → 37680 / 17355 B |
| x64/Multibloco + consulta | 4872 → 4872 / 4784 B | 37441 → 37441 / 17355 B |
| x86/Padrão | 4760 → 4760 / 4668 B | 26412 → 26244 / 13375 B |

| PIC24 padrão | RAM antes → depois / upstream | Pilha antes → depois / upstream |
|---|---:|---:|
| Enums normal | 4614 → 4614 / 4544 B | 270 → 270 / 200 B |
| Enums short | 4586 → 4586 / 4520 B | 264 → 264 / 202 B |

As **48 variantes PIC24** passaram com seções estáticas iguais e pilha interna máxima de **378 bytes**, abaixo de 400. XC16 2.10, PIC24FJ256GA702, C99 -Os, enums normais/curtos. Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação e rotinas do compilador/libc.

## Testes e tentativas descartadas

Passaram 11 cenários funcionais e 12 checks de alocação, cobrindo ordenação, pressão, três escritores, pedidos sobrepostos, espaço esgotado, reserva, espelhamento nos três perfis e falhas de ACK/retry. As auditorias do benchmark confirmam ambas as FATs atuais. Doxygen continua com 295 definições; nenhuma fragmentação em .inc foi introduzida. Os testes usam mídia sintética, sem depender dos VHDs perdidos.

Dois filtros de seleção de páginas foram descartados. Reduziam CPU de escrita, mas regrediam a leitura padrão 1/4096. Não fazem parte da fonte mantida e suas amostras não foram combinadas às finais.

- `cache8-flush-filter`: leitura 1/4096 -12.45% de redução (negativo = mais CPU).
- `cache8-flush-eligibility`: leitura 1/4096 -13.53% de redução (negativo = mais CPU).

## Evidências

`tests/build/cache8-append-dispatch` conserva before/lib, normal/lib (fonte final sem hora fixada), after/lib (benchmark), upstream canônico, objetos, executáveis, trials, scripts derivados, testes e matrizes. O JSON ao lado registra amostras completas, contadores, hashes e tentativas descartadas. lib/sdcard.h não foi editado.

```powershell
python tests/write_dispatch_cpu_report.py --directory tests/build/cache8-append-dispatch --repetitions 21 --output tests/WRITE_DISPATCH_CPU.md
```
