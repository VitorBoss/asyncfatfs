# Cache FAT32 com dois escritores e blocos de 257 bytes

Data: 16/11/2026, America/Sao_Paulo.

No caso de dois arquivos de 1 MiB, blocos de 257 bytes e clusters FAT32 de 512 bytes, as leituras caem de 4.226 para 162 setores, redução de 96,2%. As gravações caem de 12.495 para 12.488 setores. A taxa no mesmo modelo de latência passa de 0.183 para 0.198 MiB/s, ganho de 8.0%. O cache permanece com quatro setores e as duas FATs continuam sendo atualizadas.

## Causa e correção

O rastreamento de setores mostrou que 4.217 das 4.226 leituras eram da FAT primária, e nove eram do diretório. Depois das gravações espelhadas, a página secundária pode permanecer limpa no cache enquanto a primária é substituída para atender aos dois escritores. A política anterior lia a primária novamente do backend, mesmo quando a imagem confirmada correspondente estava disponível na página secundária.

A rotina agora procura uma imagem secundária no estado IN_SYNC, desbloqueada e correspondente ao mesmo setor FAT. Se a primária estiver ausente, usa essa imagem para preencher sua página. Se o alocador reutilizou a própria página secundária, mantém os bytes e troca somente o descritor. Uma primária já presente, inclusive suja, mantém precedência. Cópias sujas, em trânsito, bloqueadas ou com ACK falho não são usadas.

A eliminação dessa leitura assíncrona expôs outro efeito de pressão de cache: adquirir a segunda página FAT podia forçar uma gravação antiga do diretório antes de uma página de dados desbloqueada. Para faltas de páginas FAT, a drenagem tenta primeiro a seleção de background já existente. Se isso não puder liberar uma página, volta à drenagem completa. Assim há progresso mesmo quando o diretório é a única página liberável; flush e barreiras explícitas mantêm o contrato anterior.

## Resultados

Todos os casos usam quatro setores de cache, três handles, API original 8.3, duas FATs, sem diário/LFN/exFAT/freefile. Sete repetições por cenário e perfil. R e W são setores de 512 bytes durante a escrita, incluindo criação, dados, fechamento e drenagem. A montagem e a auditoria ficam fora da fase medida.

| Cluster B | Bloco B | Arquivos | Antes R/W | Depois R/W | Antes MiB/s | Depois MiB/s |
| --- | --- | --- | --- | --- | --- | --- |
| 512 | 257 | 1 | 50 / 6231 | 50 / 6231 | 0.199 | 0.199 |
| 512 | 257 | 2 | 4226 / 12495 | 162 / 12488 | 0.183 | 0.198 |
| 512 | 4096 | 1 | 50 / 6231 | 50 / 6231 | 0.200 | 0.200 |
| 512 | 4096 | 2 | 160 / 12486 | 160 / 12486 | 0.199 | 0.199 |
| 4096 | 257 | 1 | 516 / 2830 | 516 / 2830 | 0.578 | 0.578 |
| 4096 | 257 | 2 | 781 / 5663 | 781 / 5663 | 0.426 | 0.426 |
| 4096 | 4096 | 1 | 516 / 2830 | 516 / 2830 | 0.579 | 0.579 |
| 4096 | 4096 | 2 | 1034 / 5663 | 1034 / 5663 | 0.578 | 0.578 |

Não houve aumento de leituras nem gravações em nenhum dos cenários dos dois perfis. As fases de leitura após remontagem mantiveram as contagens anteriores. No caso-alvo, as leituras da FAT primária caem de 4.217 para 161 e as de diretório de nove para uma. O custo de gravação das duas FATs permanece; portanto o ganho de throughput é menor que a redução percentual de reads.

### Tentativas descartadas

Dar preferência à expulsão da FAT secundária limpa reduziu leituras, mas elevou as gravações de diretório e piorou a taxa. Reconstruir a primária sem ajustar a drenagem sob pressão também tinha esse efeito. Essas versões foram descartadas; seus resultados foram mantidos na pasta de evidências para não confundir menos leituras com melhor desempenho global.

## Persistência e memória

A reconstrução não marca a página como suja e não representa um ACK novo. Ela usa uma cópia já confirmada. Na estratégia mínima existente, as mudanças FAT adquirem as duas páginas e as marcam juntas; uma primária suja ou em trânsito não pode ser expulsa, e uma secundária pendente não pode fornecer a imagem. Uma segunda FAT limpa só pode ser usada depois que sua gravação espelhada foi confirmada. A otimização não se aplica aos builds com diário, que continuam com seu fluxo de before-images e replay.

A aquisição para escrita continua exigindo páginas independentes para as duas FATs, e ambos os ACKs continuam necessários antes das publicações dependentes. As barreiras EOC antes do vínculo, encadeamento antes de tamanho, sync/close e desligamento não foram retiradas. FAT32 sem diário continua sem recuperação atômica geral para corrupção ou perda de energia arbitrária.

Os 24 layouts permanecem idênticos. Não foram adicionados campos, variáveis estáticas ou buffers à biblioteca. A RAM estática padrão continua 2.494 bytes no PIC24, 2.474 com enums curtos, 2.612 no x86 e 2.720 no x64. Os objetos x64 antes/depois também foram inspecionados e ambos emitem 2.720 bytes de seções graváveis.

A primeira implementação adicionava temporários ao frame PIC24 da rotina de cache; foi substituída. O ponteiro de saída existente serve de workspace até SUCCESS, e a busca fica em uma função rasa não inline. No assembly PIC24, tanto com enums normais quanto curtos, afatfs_cacheSector mantém lnk #14 antes/depois, sem novos pushes. A busca auxiliar usa lnk #12 e não aumenta o caminho de alocação já existente, que chama funções com frames de 18 e 8 bytes. Isso verifica os frames alterados e a profundidade desse caminho; não é medição do pico de pilha de uma aplicação completa ou de seus callbacks.

## Validação

- Matriz de regressão aprovada nas sete configurações, incluindo falhas de envio/ACK, fechamento, remoção de mídia e autorização de desligamento.
- 112 volumes antes e 112 depois no benchmark, com conteúdo, tamanho, término exato da cadeia, ausência de alocações extras e igualdade das duas FATs auditados independentemente. I/O e tempos virtuais idênticos nas sete repetições de cada cenário.
- Testes de pré-imagem antes de cada write FAT e conferência dos tamanhos já publicados, com um/dois/três escritores, travessia entre setores FAT e ociosidade.
- Testes de volume quase cheio e uso do último cluster livre por diretório.
- Testes diretos de imagem de cache: cópia para outra página; reaproveitamento da própria página secundária; precedência de primária suja; recusa de secundária suja, bloqueada, em trânsito e após ACK falho. Os padrões exercitam todos os bytes da página, sem alterar clusters usados por arquivos.
- Controle negativo isolado: permitir cópias secundárias não confirmadas faz o teste de recusa de leitura pelo backend falhar por assert.
- Hints de pre-erase de metadados continuam zerados nas duas FATs, sem retenções deixadas pendentes.
- Comparação dos 24 layouts e inspeção de frames PIC24 normal/enums curtos.

As taxas são do backend assíncrono em RAM com relógio virtual: cada poll custa 10 µs; leitura de setor 200 µs; escrita isolada 800 µs; escrita múltipla 400 µs; início/fim de sequência 100 µs. Não são medições de cartão SD físico. Os resultados anteriores e os atuais usam exatamente esse modelo.

## Reprodução e evidências

- `lib/asyncfatfs.c`: busca da FAT secundária confirmada e drenagem sob pressão para páginas FAT.
- `tests/fat32_cache_mirror.c`: testes positivos e de recusa das imagens de cache.
- `tests/upstream_comparison.c`: benchmark e contadores separados de reads por FAT e diretório.
- `tests/run_fat32_allocation_checks.ps1`: integra os novos testes com as verificações de persistência existentes.
- `tests/build/fat32-83-cache-20261116/`: fontes antes/depois, binários, CSVs, versões descartadas, controles negativos, logs, objetos e assembly. Essa pasta é ignorada pelo Git.
- `tests/FAT32_83_CACHE.json`: resultados consolidados e hashes das fontes medidas.

```powershell
./tests/run_fat32_allocation_checks.ps1
./tests/run_exfat_tests.ps1
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/nova-rodada/layout
```
