# CPU: conversão de endereço do cache

A conversão de ponteiro para índice agora valida o deslocamento antes da divisão sem sinal. Com leitura multibloco habilitada, a função é incorporada ao chamador. A configuração padrão preserva o código gerado anterior nos caminhos verificados.

Referência: `upstream/master` local no commit `60c5f3580152410cb08342942cbbe3a30a449c24`. O código upstream foi conferido contra esse commit.

FAT32 8.3, clusters de 512 bytes, arquivos de 1 MiB, um ou dois leitores/escritores intercalados. MSVC x64 /O2, 21 repetições, 32 passagens de leitura, afinidade fixa e ordem alternada. Medição final sem compilações concorrentes. Ciclos por byte incluem biblioteca, backend simulado, polling e verificação dos dados. São medições no host, não tempos medidos no PIC24 ou em cartão físico.

O upstream equalizado usa quatro páginas de cache; o upstream nativo usa oito. A biblioteca atual usa quatro páginas e mantém as duas FATs. O upstream deixa diferenças entre as cópias da FAT nesse workload; seu custo de escrita não representa a mesma garantia de espelhamento.

## Leitura

Valores positivos nas colunas de redução indicam menos CPU. A mudança frente à versão anterior é a mediana das diferenças percentuais pareadas; frente ao upstream, é a razão das medianas. Diferenças próximas de zero devem ser tratadas como variação da medição.

| Configuração | Bloco | Leitores | Ciclos/byte | Redução anterior | Redução upstream 4 páginas | Redução upstream 8 páginas |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 3.7152 | Código idêntico | +17.85% | +20.53% |
| Padrão | 257 | 2 | 3.7116 | Código idêntico | +18.77% | +21.61% |
| Padrão | 4096 | 1 | 2.8828 | Código idêntico | +25.47% | +28.17% |
| Padrão | 4096 | 2 | 3.0764 | Código idêntico | +20.20% | +23.29% |
| Multibloco | 257 | 1 | 4.0309 | +1.16% | +10.87% | +13.78% |
| Multibloco | 257 | 2 | 3.6428 | +0.93% | +20.27% | +23.07% |
| Multibloco | 4096 | 1 | 3.3745 | +1.56% | +12.76% | +15.91% |
| Multibloco | 4096 | 2 | 3.0741 | +1.21% | +20.26% | +23.35% |
| Multibloco + consulta | 257 | 1 | 3.6932 | +2.11% | +18.33% | +21.00% |
| Multibloco + consulta | 257 | 2 | 3.6974 | +2.00% | +19.08% | +21.91% |
| Multibloco + consulta | 4096 | 1 | 3.0433 | +0.45% | +21.32% | +24.16% |
| Multibloco + consulta | 4096 | 2 | 3.1479 | +0.67% | +18.35% | +21.51% |

## Escrita com multibloco e consulta

| Bloco | Escritores | Ciclos/byte | Redução anterior | Redução upstream 4 páginas |
|---:|---:|---:|---:|---:|
| 257 | 1 | 6.1761 | +2.23% | -67.49% |
| 257 | 2 | 7.1898 | +0.85% | -75.73% |
| 4096 | 1 | 5.2979 | +0.35% | -54.30% |
| 4096 | 2 | 5.4545 | -0.48% | -59.17% |

## RAM, código e pilha

| ABI | Configuração | RAM antes/depois | Código antes → depois |
|---|---|---:|---:|
| x86 | Padrão | 2616 B | 25670 → 25670 B |
| x86 | Multibloco | 2616 B | 26765 → 26878 B |
| x86 | Multibloco + consulta | 2616 B | 26785 → 26898 B |
| x64 | Padrão | 2728 B | 33244 → 33244 B |
| x64 | Multibloco | 2728 B | 34488 → 34608 B |
| x64 | Multibloco + consulta | 2728 B | 34398 → 34518 B |

PIC24: as 48 variantes passaram, sem alteração de RAM estática ou do limite de pilha. Pico interno conservador: **398 bytes**, inferior a 400 bytes. Inclui chamadas internas e endereços de retorno; exclui backend, callbacks da aplicação e rotinas do compilador/libc, conforme o escopo acordado.

## I/O e persistência

Os contadores de leitura, escrita, metadados, polling, sessões multibloco e encerramentos permaneceram iguais aos da versão anterior em todos os casos medidos. Nenhuma ordenação, confirmação de gravação, reserva de cluster ou regra do journal foi alterada. A auditoria das duas FATs passou para todas as execuções da biblioteca atual.

Os 11 cenários funcionais passaram com multibloco e consulta, incluindo falhas de I/O, remoção de mídia, callbacks antigos, desmontagem/remontagem e diretório com crescimento adiado. As 295 definições de função passaram na verificação Doxygen.

## Reprodução e evidências

Evidências e snapshots: `tests/build/cpu-cache-index`. O JSON contém todas as amostras, contadores, hashes e cadeias de pilha.

```powershell
& tests/run_read_cpu_benchmark.ps1 -OutputDirectory tests/build/cpu-cache-index -RunOnly -Repetitions 21 -Variants before_default,default,before,before_query,after,after_no_query,upstream_equal,upstream_native
python tests/cache_index_cpu_report.py --directory tests/build/cpu-cache-index --repetitions 21 --output tests/build/cpu-cache-index/report.json --markdown tests/CACHE_INDEX_CPU.md
```

Os comandos pressupõem os snapshots e executáveis já preparados na pasta de evidências.
