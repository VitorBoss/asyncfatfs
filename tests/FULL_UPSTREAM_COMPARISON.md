# Comparação completa com upstream/master

Referência local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Fonte atual: `08f51da7ffb388f40e48295a450e7e1a14c74dab4eb60e0d05cab76918c7c5b4`.

## Principais resultados

Com clusters de 512 bytes, as escritas intercaladas de 257 bytes reduzem I/O, mas custam 28,45%/33,48% mais CPU por byte com um/dois escritores. Dois escritores/4096 bytes reduzem CPU em 4,38%. Com clusters de 4096 bytes, o custo de CPU da escrita aumenta 30,66% a 73,49%. Menos I/O não significa automaticamente menos CPU.

A leitura multibloco melhora o throughput simulado de um leitor com clusters de 512 bytes, com custo adicional de CPU. Dois leitores não obtêm esse ganho no modelo medido. O controle do limiar multibloco do upstream não explica a diferença principal nas escritas.

## Método

FAT32 8.3, cache de oito setores em ambas as versões, arquivos de 1 MiB por arquivo. Leitura/escrita, um/dois arquivos, blocos de 257/4096 bytes, clusters de 512/4096 bytes. Escritas seriais e intercaladas; no intercalado os buffers da aplicação são preparados uma vez por pedido, evitando geração adicional de dados a cada retry.

MSVC x64 /O2, 21 repetições alternadas, afinidade em um processador e um aquecimento por variante, sem compilações concorrentes durante a medição. CPU usa QueryThreadCycleTime; throughput do host usa QueryPerformanceCounter. Leitura de CPU tem 16 passagens, incluindo rewind; I/O e backend simulado usam uma passagem de dados após remontagem; metadados acessados na montagem podem permanecer no cache. Formatação/montagem ficam fora da medição. Escrita inclui open, close e drain. CPU usa somente o perfil de baixa latência 0; o throughput simulado das tabelas usa o perfil 1. Os dois não são somados.

Ciclos/byte incluem biblioteca, backend em RAM, polling e trabalho da aplicação/harness. A leitura inclui verificação byte a byte; a escrita inclui preparação dos buffers. Não são ciclos exclusivos da biblioteca, medidas no PIC24 ou throughput em cartão físico. Medianas e quartis por cenário estão no JSON; deltas são razões das medianas. Diferenças pequenas perante a dispersão não demonstram ganho.

Os objetos da biblioteca são idênticos entre executáveis de CPU e auditoria e entre ritmos. A auditoria de I/O tem contadores adicionais somente no harness. Data/hora FAT foram fixadas igualmente nas cópias de benchmark; a produção não foi modificada.

Atual padrão: escrita multibloco mínima 2; upstream original: mínima 4. O controle upstream_min2 altera somente esse limiar. Perfis atuais multiread e query habilitam leitura multibloco opcional.

## Diferença de persistência

A versão atual mantém as duas FATs e suas barreiras, reserva especulativa única e recuperação de alocações pendentes. O upstream deixa diferenças entre as FATs neste workload. Seu menor custo em alguns cenários não representa a mesma garantia de espelhamento. Todas as versões passaram na conferência do conteúdo e da cadeia primária; a atual também passou na igualdade das FATs e na auditoria de EOC/vínculo/diretório por gravação.

## CPU e throughput no host — padrão, intercalado

CPU menor é melhor; throughput maior é melhor. Δ CPU positivo indica custo maior da atual.

| Operação | Cluster B | Arquivos/bloco | CPU upstream → atual (ciclos/B) | Δ CPU | Host upstream → atual (MiB/s) |
|---|---:|---|---:|---:|---:|
| write | 512 | 1/257 | 4.223 → 5.424 | +28.45% | 626.80 → 487.88 |
| write | 512 | 2/257 | 4.001 → 5.341 | +33.48% | 670.96 → 500.90 |
| write | 512 | 1/4096 | 3.616 → 4.525 | +25.15% | 755.34 → 585.75 |
| write | 512 | 2/4096 | 4.570 → 4.370 | -4.38% | 590.68 → 616.37 |
| write | 4096 | 1/257 | 3.177 → 4.151 | +30.66% | 840.62 → 651.72 |
| write | 4096 | 2/257 | 3.038 → 4.892 | +61.04% | 894.77 → 550.63 |
| write | 4096 | 1/4096 | 2.941 → 4.464 | +51.77% | 932.66 → 613.20 |
| write | 4096 | 2/4096 | 3.135 → 5.439 | +73.49% | 855.83 → 492.74 |
| read | 512 | 1/257 | 3.834 → 3.706 | -3.34% | 698.42 → 715.81 |
| read | 512 | 2/257 | 3.902 → 3.895 | -0.20% | 689.41 → 685.81 |
| read | 512 | 1/4096 | 3.125 → 2.998 | -4.07% | 857.37 → 891.32 |
| read | 512 | 2/4096 | 3.125 → 3.180 | +1.77% | 862.21 → 844.64 |
| read | 4096 | 1/257 | 3.685 → 3.811 | +3.41% | 729.11 → 701.73 |
| read | 4096 | 2/257 | 3.829 → 3.895 | +1.73% | 702.71 → 688.40 |
| read | 4096 | 1/4096 | 2.955 → 3.050 | +3.20% | 905.37 → 877.04 |
| read | 4096 | 2/4096 | 2.929 → 3.047 | +4.00% | 917.53 → 873.13 |

## CPU e throughput no host — padrão, serial

| Operação | Cluster B | Arquivos/bloco | CPU upstream → atual (ciclos/B) | Δ CPU | Host upstream → atual (MiB/s) |
|---|---:|---|---:|---:|---:|
| write | 512 | 1/257 | 4.002 → 5.566 | +39.08% | 653.89 → 485.51 |
| write | 512 | 2/257 | 3.928 → 5.880 | +49.70% | 679.53 → 444.99 |
| write | 512 | 1/4096 | 3.553 → 4.562 | +28.40% | 769.17 → 595.70 |
| write | 512 | 2/4096 | 3.482 → 4.590 | +31.81% | 778.63 → 594.53 |
| write | 4096 | 1/257 | 3.104 → 4.234 | +36.39% | 888.26 → 633.87 |
| write | 4096 | 2/257 | 3.428 → 5.018 | +46.37% | 777.42 → 534.44 |
| write | 4096 | 1/4096 | 2.929 → 4.448 | +51.83% | 934.32 → 600.06 |
| write | 4096 | 2/4096 | 3.088 → 5.713 | +85.02% | 869.53 → 471.15 |
| read | 512 | 1/257 | 3.772 → 4.715 | +25.01% | 711.25 → 567.73 |
| read | 512 | 2/257 | 3.866 → 4.880 | +26.24% | 688.41 → 547.79 |
| read | 512 | 1/4096 | 3.035 → 4.016 | +32.33% | 880.24 → 668.47 |
| read | 512 | 2/4096 | 3.030 → 4.204 | +38.72% | 886.54 → 637.18 |
| read | 4096 | 1/257 | 3.672 → 4.791 | +30.46% | 728.74 → 554.67 |
| read | 4096 | 2/257 | 3.779 → 4.859 | +28.58% | 710.60 → 551.80 |
| read | 4096 | 1/4096 | 2.927 → 4.037 | +37.93% | 917.25 → 660.15 |
| read | 4096 | 2/4096 | 2.920 → 4.046 | +38.59% | 917.92 → 662.66 |

## I/O e throughput simulado — padrão, serial

R/W/metadados são setores de 512 bytes; números de metadados fazem parte de W. Perfil realista do simulador: leitura simples 200 µs, leitura multibloco 100 µs, escrita simples 800 µs, multibloco 400 µs; sessões podem custar 100 µs e cada poll 10 µs. O throughput virtual é derivado desse modelo, sem somar o tempo de CPU do host.

| Operação | Cluster B | Arquivos/bloco | R upstream → atual | W upstream → atual | Metadados upstream → atual | Backend upstream → atual (MiB/s) |
|---|---:|---|---:|---:|---:|---:|
| write | 512 | 1/257 | 35 → 27 | 2803 → 2730 | 755 → 682 | 0.445 → 0.471 |
| write | 512 | 2/257 | 36 → 70 | 5570 → 4946 | 1474 → 721 | 0.448 → 0.503 |
| write | 512 | 1/4096 | 21 → 19 | 2771 → 3189 | 723 → 1141 | 0.450 → 0.429 |
| write | 512 | 2/4096 | 37 → 35 | 5539 → 6436 | 1443 → 2340 | 0.451 → 0.421 |
| write | 4096 | 1/257 | 516 → 516 | 2564 → 2824 | 516 → 776 | 0.464 → 0.580 |
| write | 4096 | 2/257 | 522 → 522 | 4877 → 5653 | 781 → 1557 | 0.499 → 0.432 |
| write | 4096 | 1/4096 | 516 → 516 | 2564 → 2824 | 516 → 776 | 0.464 → 0.581 |
| write | 4096 | 2/4096 | 1034 → 1034 | 5131 → 5653 | 1035 → 1557 | 0.464 → 0.580 |
| read | 512 | 1/257 | 2066 → 2065 | 0 → 0 | 0 → 0 | 2.203 → 2.204 |
| read | 512 | 2/257 | 4130 → 4129 | 0 → 0 | 0 → 0 | 2.204 → 2.204 |
| read | 512 | 1/4096 | 2066 → 2065 | 0 → 0 | 0 → 0 | 2.405 → 2.421 |
| read | 512 | 2/4096 | 4130 → 4129 | 0 → 0 | 0 → 0 | 2.406 → 2.422 |
| read | 4096 | 1/257 | 2305 → 2305 | 0 → 0 | 0 → 0 | 1.993 → 1.993 |
| read | 4096 | 2/257 | 4357 → 4357 | 0 → 0 | 0 → 0 | 2.099 → 2.099 |
| read | 4096 | 1/4096 | 2305 → 2305 | 0 → 0 | 0 → 0 | 2.169 → 2.169 |
| read | 4096 | 2/4096 | 4609 → 4609 | 0 → 0 | 0 → 0 | 2.170 → 2.170 |

## I/O e throughput simulado — padrão, interleaved

R/W/metadados são setores de 512 bytes; números de metadados fazem parte de W. Perfil realista do simulador: leitura simples 200 µs, leitura multibloco 100 µs, escrita simples 800 µs, multibloco 400 µs; sessões podem custar 100 µs e cada poll 10 µs. O throughput virtual é derivado desse modelo, sem somar o tempo de CPU do host.

| Operação | Cluster B | Arquivos/bloco | R upstream → atual | W upstream → atual | Metadados upstream → atual | Backend upstream → atual (MiB/s) |
|---|---:|---|---:|---:|---:|---:|
| write | 512 | 1/257 | 35 → 27 | 2803 → 2730 | 755 → 682 | 0.445 → 0.471 |
| write | 512 | 2/257 | 451 → 39 | 5792 → 4826 | 1696 → 722 | 0.423 → 0.516 |
| write | 512 | 1/4096 | 21 → 19 | 2771 → 3189 | 723 → 1141 | 0.450 → 0.429 |
| write | 512 | 2/4096 | 453 → 35 | 5603 → 5453 | 1507 → 1357 | 0.437 → 0.484 |
| write | 4096 | 1/257 | 516 → 516 | 2564 → 2824 | 516 → 776 | 0.464 → 0.580 |
| write | 4096 | 2/257 | 522 → 522 | 4626 → 6151 | 530 → 2055 | 0.526 → 0.398 |
| write | 4096 | 1/4096 | 516 → 516 | 2564 → 2824 | 516 → 776 | 0.464 → 0.581 |
| write | 4096 | 2/4096 | 530 → 1038 | 4626 → 6147 | 530 → 2051 | 0.525 → 0.520 |
| read | 512 | 1/257 | 2066 → 2065 | 0 → 0 | 0 → 0 | 2.203 → 2.204 |
| read | 512 | 2/257 | 4130 → 4129 | 0 → 0 | 0 → 0 | 2.204 → 2.204 |
| read | 512 | 1/4096 | 2066 → 2065 | 0 → 0 | 0 → 0 | 2.405 → 2.421 |
| read | 512 | 2/4096 | 4130 → 4129 | 0 → 0 | 0 → 0 | 2.406 → 2.422 |
| read | 4096 | 1/257 | 2305 → 2305 | 0 → 0 | 0 → 0 | 1.993 → 1.993 |
| read | 4096 | 2/257 | 4357 → 4357 | 0 → 0 | 0 → 0 | 2.099 → 2.099 |
| read | 4096 | 1/4096 | 2305 → 2305 | 0 → 0 | 0 → 0 | 2.169 → 2.169 |
| read | 4096 | 2/4096 | 4609 → 4609 | 0 → 0 | 0 → 0 | 2.170 → 2.170 |

## Composição das escritas — padrão, intercalado, latência 1

Cada célula mostra upstream → atual; contagens são setores, incluindo regravações.

| Cluster B | Escritores/bloco | Dados | FAT1 | FAT2 | Diretório |
|---:|---|---:|---:|---:|---:|
| 512 | 1/257 | 2048 → 2048 | 393 → 276 | 0 → 276 | 362 → 130 |
| 512 | 2/257 | 4096 → 4104 | 864 → 296 | 0 → 296 | 832 → 130 |
| 512 | 1/4096 | 2048 → 2048 | 369 → 562 | 0 → 562 | 354 → 17 |
| 512 | 2/4096 | 4096 → 4096 | 800 → 643 | 0 → 643 | 707 → 71 |
| 4096 | 1/257 | 2048 → 2048 | 258 → 259 | 0 → 259 | 258 → 258 |
| 4096 | 2/257 | 4096 → 4096 | 268 → 770 | 0 → 770 | 262 → 515 |
| 4096 | 1/4096 | 2048 → 2048 | 258 → 259 | 0 → 259 | 258 → 258 |
| 4096 | 2/4096 | 4096 → 4096 | 268 → 768 | 0 → 768 | 262 → 515 |

## Perfis opcionais de leitura — intercalado

CPU é mediana no host; backend é o perfil de latência realista. Cache continua oito setores.

| Cluster B | Leitores/bloco | CPU padrão / multiread / query / upstream | Backend padrão / multiread / query / upstream (MiB/s) |
|---:|---|---:|---:|
| 512 | 1/257 | 3.706 / 4.252 / 3.941 / 3.834 | 2.204 / 3.938 / 3.938 / 2.203 |
| 512 | 2/257 | 3.895 / 3.975 / 4.089 / 3.902 | 2.204 / 2.204 / 2.204 / 2.204 |
| 512 | 1/4096 | 2.998 / 3.511 / 3.153 / 3.125 | 2.421 / 4.690 / 4.690 / 2.405 |
| 512 | 2/4096 | 3.180 / 3.181 / 3.300 / 3.125 | 2.422 / 2.422 / 2.422 / 2.406 |
| 4096 | 1/257 | 3.811 / 3.839 / 3.930 / 3.685 | 1.993 / 1.993 / 1.993 / 1.993 |
| 4096 | 2/257 | 3.895 / 3.935 / 3.996 / 3.829 | 2.099 / 2.099 / 2.099 / 2.099 |
| 4096 | 1/4096 | 3.050 / 3.068 / 3.144 / 2.955 | 2.169 / 2.169 / 2.169 / 2.169 |
| 4096 | 2/4096 | 3.047 / 3.076 / 3.159 / 2.929 | 2.170 / 2.170 / 2.170 / 2.170 |

## Controle de escrita multibloco mínima 2 — intercalado

| Cluster B | Escritores/bloco | CPU upstream 4 / upstream 2 / atual 2 | Backend upstream 4 / upstream 2 / atual 2 (MiB/s) |
|---:|---|---:|---:|
| 512 | 1/257 | 4.223 / 4.066 / 5.424 | 0.445 / 0.445 / 0.471 |
| 512 | 2/257 | 4.001 / 3.987 / 5.341 | 0.423 / 0.423 / 0.516 |
| 512 | 1/4096 | 3.616 / 3.601 / 4.525 | 0.450 / 0.450 / 0.429 |
| 512 | 2/4096 | 4.570 / 4.567 / 4.370 | 0.437 / 0.437 / 0.484 |
| 4096 | 1/257 | 3.177 / 3.206 / 4.151 | 0.464 / 0.464 / 0.580 |
| 4096 | 2/257 | 3.038 / 3.030 / 4.892 | 0.526 / 0.526 / 0.398 |
| 4096 | 1/4096 | 2.941 / 2.966 / 4.464 | 0.464 / 0.464 / 0.581 |
| 4096 | 2/4096 | 3.135 / 3.089 / 5.439 | 0.525 / 0.525 / 0.520 |

## Memória e evidências

Recursos dos objetos x64 (biblioteca sem harness): `{"default": {"static_ram_bytes": 4872, "code_bytes": 44912}, "multiread": {"static_ram_bytes": 4872, "code_bytes": 47311}, "query": {"static_ram_bytes": 4872, "code_bytes": 46998}, "upstream": {"static_ram_bytes": 4784, "code_bytes": 17355}, "upstream_min2": {"static_ram_bytes": 4784, "code_bytes": 17355}}`. A matriz de persistência/memória da versão atual é a validada em SMALL_WRITE_COLLECTION: 48 configurações, pilha interna máxima 378 B. O orçamento não inclui backend/aplicação/libc.

JSON contém todos os cenários, amostras, quartis, CPU/byte, throughput do host, contagens de metadados por FAT/diretório, sessões multibloco e diferenças de espelhamento, inclusive latência 0. Evidências congeladas: `tests/build/full-upstream-20261119`. Nenhuma conclusão de velocidade física de SD é inferida.
