# Flush filtrado na espera pela reserva

Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. FAT32 8.3, cache **8 setores**, clusters 512 bytes, arquivos de 1 MiB.

A espera da reserva usa flushAllocationCache(file), evitando enviar a imagem antiga do diretório. As verificações IN_SYNC das duas FATs permanecem antes de consumir a reserva. A reserva única, EOC confirmado antes do vínculo e vínculo confirmado antes do diretório permanecem.

MSVC x64 /O2, 21 repetições pareadas com ordem alternada, afinidade fixa e 32 passagens de leitura. Sem compilações concorrentes durante CPU. Hora FAT fixada em 12:00:00 somente nas cópias de benchmark. Ciclos/B incluem biblioteca, harness e backend simulado; não medem PIC24 ou cartão físico. O modo intercalado prepara o payload uma vez por pedido. Auditorias instrumentadas separadas, timings descartados. Redução positiva significa menos CPU; pequenas diferenças podem ser ruído.

PIC24: 48 configurações/perfis/ABIs, pilha interna máxima **378 bytes**, incluindo chamadas internas e PCs de retorno, excluindo driver/aplicação/libc. RAM estática igual antes/depois em todas. Recursos x64 abaixo contam somente os objetos da biblioteca.

## Resultado principal e custo observado

Padrão, dois escritores/4096 intercalados: CPU reduzida **2.71%**, ainda **28.53%** acima do upstream. Metadados 8547 → 8259 (−3,37%); escritas totais 12643 → 12355 (−2,28%); diretório 291 → 3; FAT1/FAT2 permanecem em 4128 cada. Sequências multibloco preservadas: 288 sessões/576 setores.

O ganho de CPU não é geral: nos casos seriais o I/O permanece igual e o maior aumento pareado de CPU observado foi **3.60%**, no perfil opcional com consulta. As tabelas preservam todos os resultados, incluindo aumentos. A alteração resolve o flush evitável do diretório; não resolve o custo dominante de publicação da FAT por cluster.

## serial-default

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.9033 → 6.8956 | +0.78% | 3.9213 | +75.85% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 7.1823 → 7.2593 | -0.40% | 3.8726 | +87.46% | 8326 → 8326 / 4165 | 34/12422 / 34/8261 |
| 4096/1 | 5.8975 → 5.9218 | -0.27% | 3.4751 | +70.41% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 6.1584 → 6.1649 | -0.67% | 3.3941 | +81.64% | 8324 → 8324 / 1461 | 34/12420 / 50/5557 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 36036}, "after": {"static_ram_bytes": 4872, "code_bytes": 36060}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4161 → 4161 | 4161 → 4161 | 4 → 4 | 0/0 → 0/0 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4160 → 4160 | 4160 → 4160 | 4 → 4 | 0/0 → 0/0 |

Controle de leitura (redução pareada de CPU): 257/1: +0.93%, 257/2: +0.92%, 4096/1: +0.48%, 4096/2: -0.68%.

## serial-multi

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.3017 → 6.3106 | -1.54% | 3.8998 | +61.82% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 6.4918 → 6.6267 | -1.64% | 3.8295 | +73.04% | 8326 → 8326 / 4165 | 34/12422 / 34/8261 |
| 4096/1 | 5.5455 → 5.5886 | -0.58% | 3.5030 | +59.54% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 5.6921 → 5.7701 | -1.29% | 3.3781 | +70.81% | 8324 → 8324 / 1461 | 34/12420 / 50/5557 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 37680}, "after": {"static_ram_bytes": 4872, "code_bytes": 37704}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4161 → 4161 | 4161 → 4161 | 4 → 4 | 0/0 → 0/0 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4160 → 4160 | 4160 → 4160 | 4 → 4 | 0/0 → 0/0 |

Controle de leitura (redução pareada de CPU): 257/1: -0.05%, 257/2: +0.06%, 4096/1: +0.12%, 4096/2: +0.14%.

## serial-query

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.2769 → 6.3379 | -2.33% | 3.9318 | +61.20% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 6.5756 → 6.6347 | -0.81% | 3.9109 | +69.64% | 8326 → 8326 / 4165 | 34/12422 / 34/8261 |
| 4096/1 | 5.4387 → 5.6308 | -3.60% | 3.4867 | +61.49% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 5.6856 → 5.7730 | -1.61% | 3.3984 | +69.88% | 8324 → 8324 / 1461 | 34/12420 / 50/5557 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 37441}, "after": {"static_ram_bytes": 4872, "code_bytes": 37465}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4161 → 4161 | 4161 → 4161 | 4 → 4 | 0/0 → 0/0 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4160 → 4160 | 4160 → 4160 | 4 → 4 | 0/0 → 0/0 |

Controle de leitura (redução pareada de CPU): 257/1: -0.20%, 257/2: -0.28%, 4096/1: +0.97%, 4096/2: +6.39%.

## prepared-default

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.9955 → 7.1598 | -0.84% | 4.0279 | +77.76% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 6.8455 → 6.7918 | +0.93% | 3.9420 | +72.29% | 8324 → 8323 / 1696 | 34/12419 / 35/5792 |
| 4096/1 | 5.9693 → 5.9964 | -0.44% | 3.6637 | +63.67% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 6.0248 → 5.8637 | +2.71% | 4.5621 | +28.53% | 8547 → 8259 / 1538 | 34/12355 / 453/5634 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 36036}, "after": {"static_ram_bytes": 4872, "code_bytes": 36060}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4160 → 4160 | 4160 → 4160 | 4 → 3 | 8/16 → 8/16 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4128 → 4128 | 4128 → 4128 | 291 → 3 | 288/576 → 288/576 |

Controle de leitura (redução pareada de CPU): 257/1: +0.80%, 257/2: -0.69%, 4096/1: -0.44%, 4096/2: -1.35%.

## prepared-multi

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.2885 → 6.2148 | +1.10% | 4.0408 | +53.80% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 6.4957 → 6.4710 | +0.03% | 3.9638 | +63.25% | 8324 → 8323 / 1696 | 34/12419 / 35/5792 |
| 4096/1 | 5.5742 → 5.5745 | +1.23% | 3.6303 | +53.56% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 5.9109 → 5.8176 | +1.58% | 4.5896 | +26.76% | 8547 → 8259 / 1538 | 34/12355 / 453/5634 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 37680}, "after": {"static_ram_bytes": 4872, "code_bytes": 37704}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4160 → 4160 | 4160 → 4160 | 4 → 3 | 8/16 → 8/16 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4128 → 4128 | 4128 → 4128 | 291 → 3 | 288/576 → 288/576 |

Controle de leitura (redução pareada de CPU): 257/1: -1.23%, 257/2: -0.35%, 4096/1: -0.88%, 4096/2: +0.39%.

## prepared-query

| Bloco/escritores | CPU antes → depois (c/B) | Redução pareada | Upstream c/B | CPU extra upstream | Metadados antes → depois / upstream | R/W depois / upstream |
|---|---:|---:|---:|---:|---:|---:|
| 257/1 | 6.5386 → 6.4138 | +2.06% | 4.0461 | +58.52% | 4133 → 4133 / 2083 | 18/6181 / 18/4131 |
| 257/2 | 6.6178 → 6.4815 | +2.40% | 3.9381 | +64.58% | 8324 → 8323 / 1696 | 34/12419 / 35/5792 |
| 4096/1 | 5.6582 → 5.6648 | +0.07% | 3.6433 | +55.49% | 4133 → 4133 / 741 | 18/6181 / 34/2789 |
| 4096/2 | 6.1259 → 5.8897 | +4.02% | 4.5766 | +28.69% | 8547 → 8259 / 1538 | 34/12355 / 453/5634 |

Recursos: `{"before": {"static_ram_bytes": 4872, "code_bytes": 37441}, "after": {"static_ram_bytes": 4872, "code_bytes": 37465}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}}`.

| Bloco/escritores | FAT1 antes → depois | FAT2 antes → depois | Diretório antes → depois | Multibloco sessões/setores antes → depois |
|---|---:|---:|---:|---:|
| 257/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 257/2 | 4160 → 4160 | 4160 → 4160 | 4 → 3 | 8/16 → 8/16 |
| 4096/1 | 2065 → 2065 | 2065 → 2065 | 3 → 3 | 0/0 → 0/0 |
| 4096/2 | 4128 → 4128 | 4128 → 4128 | 291 → 3 | 288/576 → 288/576 |

Controle de leitura (redução pareada de CPU): 257/1: -0.15%, 257/2: -0.81%, 4096/1: +0.52%, 4096/2: +2.04%.

## Recursos adicionais

Recursos x86: `{"before": {"static_ram_bytes": 4760, "code_bytes": 26244}, "after": {"static_ram_bytes": 4760, "code_bytes": 26244}, "upstream": {"static_ram_bytes": 4668, "code_bytes": 13375}}`.

| PIC24 padrão | Atual antes → depois (RAM/pilha B) | Upstream (RAM/pilha B) |
|---|---:|---:|
| normal | 4614/270 → 4614/270 | 4544/200 |
| short | 4586/264 → 4586/264 | 4520/202 |

## Persistência e limites

Passaram 15 testes de alocação/cache, sete suítes funcionais padrão/opcionais e 11 suítes em cada perfil de leitura multibloco (com/sem consulta). Cobrem reserva/agrupamento com ACKs falhos e retries, espelho, quase cheio, três escritores e ordenação por gravação. O teste específico reserve_wait mantém um diretório sujo enquanto falham submissões/ACKs da primária ou do espelho, confirmando que a reserva não é consumida e o diretório não é enviado. O controle negativo com a biblioteca anterior falha na asserção que exige manter o diretório sujo. Não equivale a uma campanha física de corte de energia.

O upstream deste workload não atualiza a segunda FAT. As contagens são setores transferidos, não programas NAND. A mudança reduz flushes evitáveis de diretório; a amplificação dominante da FAT permanece. Evidências: tests/build/reserve-flush.
