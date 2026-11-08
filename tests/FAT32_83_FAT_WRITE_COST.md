# Custo de gravação das duas FATs em FAT32

Data: 16/11/2026, America/Sao_Paulo.

A investigação encontrou um pequeno ganho seguro adicional: preparar a reserva única junto com o EOC do cluster atual, quando ambos pertencem ao mesmo setor FAT. Isso elimina uma gravação exclusiva de EOC no início do arquivo e após fronteiras de setor FAT. O ganho incremental no modelo de latência ficou entre 0,5% e 0,7% com clusters de 512 bytes. As duas FATs, todos os ACKs e o limite de uma reserva antecipada permanecem.

A medição também isolou o custo dominante restante. Mantendo exatamente as mesmas leituras, gravações, conteúdo e callbacks, um diagnóstico que cobra apenas 10 µs pela latência de cada write da FAT secundária chega a aproximadamente 0,296–0,300 MiB/s, perto dos 0,298–0,302 do upstream com cache igualado. Esse diagnóstico não é uma otimização de produção nem representa um cartão físico. O código normal continua cobrando a latência real/modelada das duas cópias.

## Mudança implementada

Antes, a reserva do cluster seguinte só aproveitava a escrita do vínculo anterior. No primeiro cluster e no início de um novo setor FAT, essa oportunidade faltava: a primeira entrada do setor recebia seu EOC e, logo depois, a alocação seguinte exigia outra gravação exclusiva de EOC.

Agora a mesma reserva única também pode aproveitar a escrita do EOC atual. A reserva continua sem vínculo com qualquer arquivo e só é consumida depois que as duas FATs confirmam sua imagem. A fase de EOC durável, a fase de vínculo durável e a publicação posterior do tamanho continuam presentes. Não há novo estado, buffer ou ampliação da janela de reservas.

## Comparação com clusters de 512 bytes

Cada arquivo tem 1 MiB. Dois arquivos são alternados na mesma thread. Cache de quatro setores, três handles e limiar de múltiplos blocos 2, com API original 8.3, sem LFN/exFAT/freefile/diário. Sete repetições antes e depois; números abaixo usam o perfil de latência. R/W são setores de 512 bytes.

| Bloco B | Arquivos | Antes R/W | Depois R/W | Antes MiB/s | Depois MiB/s | Ganho |
| --- | --- | --- | --- | --- | --- | --- |
| 257 | 1 | 50 / 6231 | 50 / 6197 | 0.198585 | 0.199663 | 0.54% |
| 257 | 2 | 162 / 12488 | 98 / 12422 | 0.197934 | 0.199227 | 0.65% |
| 4096 | 1 | 50 / 6231 | 50 / 6197 | 0.200105 | 0.201200 | 0.55% |
| 4096 | 2 | 160 / 12486 | 160 / 12420 | 0.199482 | 0.200538 | 0.53% |

Nenhum cenário dos dois perfis aumentou leituras ou gravações. O caso de dois escritores e blocos de 257 bytes ainda reduz leituras de 162 para 98. As fases de leitura após remontagem mantêm os resultados anteriores.

### Gravações separadas por cópia

| Bloco B | Arquivos | FATs antes primária/secundária | FATs depois primária/secundária | Diretório antes/depois |
| --- | --- | --- | --- | --- |
| 257 | 1 | 2082 / 2082 | 2065 / 2065 | 19 / 19 |
| 257 | 2 | 4194 / 4194 | 4161 / 4161 | 4 / 4 |
| 4096 | 1 | 2082 / 2082 | 2065 / 2065 | 19 / 19 |
| 4096 | 2 | 4193 / 4193 | 4160 / 4160 | 4 / 4 |

Com um escritor, cada cópia passa de 2.082 para 2.065 gravações para 2.048 clusters de dados. São 17 gravações removidas por FAT, 34 no conjunto. O custo restante por cópia está próximo de uma gravação de vínculo por cluster, mais as fronteiras e o fechamento nessa estratégia de reserva única. Isso não é uma prova de limite universal do formato FAT32.

## Writes sem alteração de conteúdo

O backend compara a imagem de cada FAT com os bytes presentes no meio imediatamente antes da cópia. Nenhum write de FAT era idêntico à imagem anterior nos cenários medidos, antes ou depois. Os contadores são somente do harness de teste e não acrescentam RAM à biblioteca. Portanto, nesta carga, suprimir writes por conteúdo idêntico não oferece ganho adicional.

## Diagnóstico da latência da segunda FAT

O controle COMPARISON_ZERO_MIRROR_LATENCY continua escrevendo todos os setores secundários e esperando seus callbacks. Somente a latência atribuída a esse tipo de write muda de 800 para 10 µs, o mínimo de um poll. As contagens de I/O, blocos múltiplos e metadados são idênticas às da versão normal em todos os cenários; os volumes permanecem íntegros e com as duas FATs iguais. O controle não altera o código da biblioteca.

| Bloco B | Arquivos | Normal MiB/s | Diagnóstico MiB/s | Upstream cache 4 MiB/s | Upstream padrão cache 8 MiB/s |
| --- | --- | --- | --- | --- | --- |
| 257 | 1 | 0.199663 | 0.296114 | 0.301577 | 0.444506 |
| 257 | 2 | 0.199227 | 0.296227 | 0.297818 | 0.448077 |
| 4096 | 1 | 0.201200 | 0.299506 | 0.302132 | 0.450213 |
| 4096 | 2 | 0.200538 | 0.299099 | 0.299638 | 0.450559 |

O upstream é a referência local upstream/master no commit 60c5f3580152410cb08342942cbbe3a30a449c24. A comparação de mesmo orçamento usa quatro páginas e limiar 2; o upstream padrão tem oito páginas e limiar 4. Os valores upstream são da rodada anterior preservada. O upstream deixou a FAT secundária desatualizada naquele ensaio. As duas configurações não têm o mesmo comportamento de persistência.

No caso de um arquivo e blocos de 257 bytes, a versão atual escreve 6.197 setores e o upstream igualado 4.128. Dos 2.069 setores adicionais, 2.065 são da cópia secundária. Ao cobrar apenas um poll por essa cópia, a taxa fica em 0,296 MiB/s contra 0,302 do upstream igualado. Isso localiza a maior parte do tempo adicional no custo do espelho no modelo, sem inferir a mesma proporção para um cartão real ou para o upstream com cache maior.

## O que continua limitado

As atualizações atuais aguardam o EOC do destino nas duas cópias antes do vínculo e aguardam o encadeamento antes de publicar o tamanho. Agrupar várias alocações antes desses pontos demandaria outra estratégia para as reservas, as caudas não usadas, a sincronização e a interrupção de mídia. Os resultados presentes não demonstram que essa mudança manteria o mesmo contrato e orçamento de memória, por isso essa janela não foi ampliada.

As duas cópias ficam em setores físicos distintos, separados pelo comprimento da FAT. A API de múltiplos blocos cobre endereços consecutivos e pode pré-apagar a faixa anunciada; ela não permite saltar o intervalo entre as cópias. Anunciar toda essa faixa incluiria metadados não preparados e não preservaria o contrato do backend.

A redução segura confirmada nesta etapa é pequena. O diagnóstico mostra que retirar a latência do segundo write no modelo é muito diferente de eliminar esse custo em produção. Não foi introduzida aprovação antecipada de ACK, espelho adiado para depois do sync ou aumento de cache.

## Persistência e memória

- Reserva antecipada continua limitada a um cluster, utilizando o mesmo hint de 32 bits. Não há novos membros, buffers, heap ou variáveis estáticas na biblioteca.
- Consumo da reserva continua dependendo de ambas as cópias confirmadas. O vínculo e a publicação de tamanho mantêm as barreiras existentes.
- Sync, close, flush, truncamento, uso por diretório e inatividade continuam liberando reservas não consumidas.
- O teste novo cobre primeiro cluster e travessia da fronteira 4.991 para 4.992: verifica o EOC reservado nas duas FATs, injeta três ACKs falhos durante sua liberação, confirma que o callback de sync não termina antes da recuperação e confere tamanho exato e conteúdo após remontar.
- FAT32 sem diário continua sem recuperação atômica geral após queda. O novo agendamento não amplia o limite de reserva antecipada; trabalho interrompido antes do ponto durável continua sujeito à semântica documentada do modo sem diário.

Os 24 layouts ficaram idênticos. RAM estática padrão: PIC24 2.494 bytes, PIC24 com enums curtos 2.474, x86 2.612 e x64 2.720. Os objetos x64 antes/depois também emitiram 2.720 bytes de seções graváveis. No PIC24, enums normais e curtos mantêm os frames das duas funções alteradas: 14 bytes em afatfs_reserveNextCluster e 20 em afatfs_appendRegularFreeClusterContinue. A chamada adicional não cria uma profundidade maior que o caminho já existente da mesma função auxiliar. Isso não é medição do pico de uma aplicação inteira ou de seus callbacks.

## Verificações e reprodução

- Sete configurações de regressão aprovadas, incluindo falhas de envio/ACK, remontagem, fechamento, remoção e autorização de desligamento.
- 112 volumes antes e 112 depois no benchmark normal. Conteúdo, tamanho, término da cadeia, ausência de alocações extras e igualdade das duas FATs auditados; contagens e tempo virtual idênticos nas sete repetições.
- Mais 16 volumes no diagnóstico de latência, com exatamente os mesmos reads/writes da versão normal.
- Auditoria de pré-imagem e imagem de cada write FAT, encadeamento compatível com o tamanho publicado, ociosidade, três escritores e volume quase cheio.
- Casos de fonte de cache limpa/suja/bloqueada/em trânsito/ACK falho, retenções e hints de pre-erase de metadados.
- Novo teste de reserva e sync com falhas; controle negativo com a versão anterior falha na exigência da reserva do primeiro cluster, demonstrando cobertura do caminho novo.
- Compilação e asserts dos 24 layouts e comparação de frames PIC24 normal/enums curtos.

O modelo mantém polls de 10 µs, leitura de setor a 200 µs, escrita isolada a 800 µs, escrita múltipla a 400 µs e 100 µs de início/fim de sequência. As taxas são do modelo de latência, não de SD físico. O diagnóstico fictício é identificado separadamente.

Arquivos: lib/asyncfatfs.c, tests/fat32_reservation_commit.c, tests/upstream_comparison.c e tests/run_fat32_allocation_checks.ps1. Evidências locais completas em tests/build/fat32-83-fat-writes-20261116/, com snapshots, CSVs, logs, binários, objetos, assembly e controle negativo. Dados consolidados e hashes em tests/FAT32_83_FAT_WRITE_COST.json.

```powershell
./tests/run_fat32_allocation_checks.ps1
./tests/run_exfat_tests.ps1
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/nova-rodada/layout
```
