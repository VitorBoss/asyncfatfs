# Escritas pequenas: janela de alocações já solicitadas

Validação final em FAT32 8.3, clusters de 512 bytes e cache de **oito setores**. Arquivos de 1 MiB por escritor, um/dois escritores, pedidos de 257/4096 bytes e duas latências simuladas. Base: versão imediatamente anterior, após os pequenos ajustes de fechamento (4132/8320 metadados nos casos intercalados 1/257 e 2/257).

## Resultado principal

Contagens de setores de 512 bytes, perfil de latência 0. Redução é calculada sobre **todas as escritas**, incluindo dados e metadados.

| Ritmo | Escritores/bloco | Escritas antes → depois | Redução total | Metadados antes → depois | Leituras antes → depois |
|---|---|---:|---:|---:|---:|
| serial | 1/257 | 6180 → 2730 | 55.83% | 4132 → 682 | 17 → 19 |
| serial | 2/257 | 12421 → 4946 | 60.18% | 8325 → 721 | 33 → 71 |
| serial | 1/4096 | 3202 → 3202 | 0.00% | 1154 → 1154 | 19 → 19 |
| serial | 2/4096 | 6465 → 6465 | 0.00% | 2369 → 2369 | 35 → 35 |
| interleaved | 1/257 | 6180 → 2730 | 55.83% | 4132 → 682 | 17 → 19 |
| interleaved | 2/257 | 12416 → 4947 | 60.16% | 8320 → 722 | 33 → 70 |
| interleaved | 1/4096 | 3202 → 3202 | 0.00% | 1154 → 1154 | 19 → 19 |
| interleaved | 2/4096 | 5656 → 5656 | 0.00% | 1560 → 1560 | 35 → 35 |

## Como o ganho foi obtido

Pedidos menores que um setor podem acrescentar EOCs a uma janela de até **16 clusters já solicitados**. O limite é `AFATFS_SMALL_WRITE_WINDOW_CLUSTERS=16`; zero desativa a coleção, preservando o caminho de alocação anterior. O lote original para pedidos grandes continua limitado por `AFATFS_REQUESTED_BATCH_CLUSTERS=8`.

Cada novo cluster só é alocado quando um pedido realmente o exige. Com vários escritores, uma passada fixa dentro da página FAT permite intercalar suas alocações sem reservar os espaços intermediários. A busca mantém os espaços livres descobertos acessíveis e recorre a qualquer cluster livre da página quando a posição preferida está ocupada. A reserva especulativa global continua sendo, no máximo, um cluster.

A janela usa as mesmas páginas FAT do cache; seu estado e callbacks de sync/close cabem na união de operações existente. Flags novos ocupam bits dos bytes já existentes. Não há heap ou buffer de setores novo. Dados podem sair do cache enquanto os EOCs são agrupados. A janela termina no limite, em sync/close ou após até 64 passagens internas ociosas, sem contar períodos em que o backend ainda grava.

## Persistência

- O marcador FAT32 de volume sujo é confirmado nas duas FATs antes de preparar a janela.
- Todos os EOCs são confirmados nas duas cópias antes dos vínculos; os vínculos são confirmados antes da publicação do diretório. A imagem exata publicada pela coleção corresponde a dados já confirmados.
- fsync/fclose podem ser enfileirados atrás da alocação usando o prefixo comum da união existente. Seus callbacks aguardam dados, ambas as FATs, diretório e término do backend.
- fsync continua confirmando dados e tamanho exato. Uma sessão que usou coleção permanece conservadoramente marcada suja até o fechamento; isso evita regravar os cabeçalhos a cada fsync. Um reinício pode executar recuperação mesmo após fsync, preservando os dados confirmados. O desligamento limpo continua concluindo os ACKs antes de permitir power-off.

A reserva especulativa não foi ampliada. Os até 16 clusters por escritor são propriedade de pedidos já recebidos. Não se antecipa dado futuro. A recuperação anterior continua removendo cabeças órfãs com memória limitada e protegendo referências vivas.

## Memória e testes

RAM/código dos objetos MSVC /O2: `{"32": {"before": {"static_ram_bytes": 4760, "code_bytes": 32519}, "after": {"static_ram_bytes": 4760, "code_bytes": 34602}}, "64": {"before": {"static_ram_bytes": 4872, "code_bytes": 42446}, "after": {"static_ram_bytes": 4872, "code_bytes": 44912}}}`. RAM estática igual em x86/x64 e em todas as 48 configurações/perfis/ABIs PIC24. Máximo interno **378 bytes**; inclui PCs internos e exclui backend/aplicação/libc, conforme o orçamento estabelecido.

As 40 execuções de modos/backends passaram em **320 cenários de escrita e 320 releituras**. Incluem w/a/w+/x/a+/r+, fputc, backends padrão, leitura multibloco, multibloco com consulta e escrita monobloco. Cada gravação foi auditada quanto à ordem EOC/vínculo/diretório; conteúdo e ambas as FATs foram conferidos. Os 80 pares de 257 bytes superaram 50% de redução nas escritas totais, incluindo as duas latências. As contagens completas, incluindo as duas latências, estão no JSON.

Passaram as sete suítes funcionais e os testes de alocação/cache, incluindo disco quase cheio com outro escritor aberto e inativo. Esse caso consome espaços de ambas as posições e verifica a cadeia do arquivo que ocupa o restante do volume. Testes de falha agora deixam explicitamente um byte sujo quando precisam bloquear uma transferência: a coleção pode já ter confirmado os dados após uma espera.

Campanhas por perfil: `{"mirror_cache": {"requested_recovery": {"write_cuts": 82, "recovery_cuts": 58, "retry_cases": 8}, "small_collection": {"write_cuts": 76, "recovery_cuts": 58, "retry_cases": 8}}, "mirror_cache_multi": {"requested_recovery": {"write_cuts": 82, "recovery_cuts": 58, "retry_cases": 8}, "small_collection": {"write_cuts": 76, "recovery_cuts": 58, "retry_cases": 8}}, "mirror_cache_query": {"requested_recovery": {"write_cuts": 82, "recovery_cuts": 58, "retry_cases": 8}, "small_collection": {"write_cuts": 76, "recovery_cuts": 58, "retry_cases": 8}}}`. Incluem meio setor, falhas de submissão/ACK, recuperação interrompida, criação e extensão, preservação de prefixo confirmado, raiz/subdiretório e imutabilidade dos buffers em trânsito.

## fsync a cada pedido

Este padrão encerra a janela em cada sincronização e não recebe a mesma redução da escrita contínua. O teste usa 16 pedidos de 257 bytes por escritor e solicita fsync imediatamente após aceitar os bytes, sem aguardar a coleção ficar ociosa. Confere a mídia após cada callback, injeta três falhas de ACK e reinicia sem fclose para validar os dados confirmados.

| Escritores | FAT antes → depois | Diretório antes → depois | Escritas duráveis antes → depois |
|---|---:|---:|---:|
| 1 | 52 → 36 | 16 → 16 | 92 → 76 |
| 2 | 66 → 36 | 32 → 23 | 146 → 107 |

As três tentativas de ACK que falharam são adicionais em ambas as versões e não entram nessa tabela de conclusões duráveis.

## Custos e limites

Nos pedidos de 257 bytes, o número de leituras pode aumentar por causa de cabeçalhos e releituras de setores parciais. O relatório mostra esse custo; a meta de mais de 50% se refere às escritas totais contínuas. O caso 2/4096 preserva 35 leituras e 1560 metadados na latência 0. Não foram medidos CPU nem throughput em cartão físico.

Permanecem os limites anteriores da recuperação: montagem de volume sujo pode ser demorada em volumes grandes/fragmentados e não substitui fsck para ciclos órfãos ou corrupção arbitrária. A campanha sintética não prova todos os padrões possíveis de falha física.

Evidências congeladas: tests/build/small-write-collection. O harness e seus snapshots não pertencem ao orçamento de RAM/pilha da biblioteca.
