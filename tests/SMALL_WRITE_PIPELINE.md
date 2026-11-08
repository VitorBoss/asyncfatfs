# Pedidos de 257 bytes: reserva e sincronização

FAT32 8.3, clusters de 512 bytes, cache oito setores. Duas FATs, reserva especulativa única, RAM estática e ordem de persistência mantidas. Não há lote especulativo de oito clusters para pedidos pequenos.

## Mudanças

- Uma alocação já solicitada por outro escritor tem prioridade sobre criar uma reserva opcional. A reserva existente permanece válida; não são criados novos estados ou buffers.
- fsync/fclose drenam dados e ambas as FATs mantendo a imagem antiga do próprio diretório em cache. Depois publicam o tamanho lógico final e aguardam seu ACK. O background também reconhece os estágios iniciais de sync/close dentro do limite de idade já existente. Isso evita gravar a imagem antiga imediatamente antes de substituí-la.

## Escrita contínua

Arquivos de 1 MiB. Setores de 512 bytes; latência 0. Não houve medição de CPU.

| Ritmo | Escritores/bloco | Metadados antes → depois | Leituras antes → depois | Escritas antes → depois |
|---|---|---:|---:|---:|
| serial | 1/257 | 4133 → 4132 | 17 → 17 | 6181 → 6180 |
| serial | 2/257 | 8326 → 8325 | 33 → 33 | 12422 → 12421 |
| serial | 1/4096 | 1154 → 1154 | 19 → 19 | 3202 → 3202 |
| serial | 2/4096 | 2369 → 2369 | 35 → 35 | 6465 → 6465 |
| interleaved | 1/257 | 4133 → 4132 | 17 → 17 | 6181 → 6180 |
| interleaved | 2/257 | 8323 → 8320 | 33 → 33 | 12419 → 12416 |
| interleaved | 1/4096 | 1154 → 1154 | 19 → 19 | 3202 → 3202 |
| interleaved | 2/4096 | 1560 → 1560 | 35 → 35 | 5656 → 5656 |

O ganho contínuo de 257 bytes é pequeno: um setor com um escritor e três com dois intercalados. Com um escritor, as FATs já somam 4130 setores: um par por cluster, mais as transições entre páginas FAT e a limpeza da reserva final. O diretório cai de três para dois setores. Não foi encontrada redução da mesma escala dos pedidos de 4096 preservando o caminho atual de confirmação por cluster. Acumular pedidos futuros, ampliar reservas ou remover barreiras não foi adotado nesta mudança.

## Sincronização a cada pedido de 257 bytes

Dezesseis pedidos por escritor. Dados e tamanho foram conferidos diretamente na mídia após cada fsync; houve três falhas de ACK do diretório com retry, bloqueio de conclusão/desligamento e verificação do buffer em trânsito. Releitura após remontagem passou. Escritas abaixo são conclusões duráveis, sem contar tentativas que falharam.

| Escritores | FAT antes → depois | Diretório antes → depois | Dados antes → depois | Metadados antes → depois |
|---|---:|---:|---:|---:|
| 1 | 52 → 52 | 25 → 16 | 24 → 24 | 77 → 68 |
| 2 | 66 → 66 | 41 → 32 | 48 → 48 | 107 → 98 |

## Validação e memória

Quatro perfis de backend (padrão, multibloco de leitura, multibloco com consulta e escrita monobloco), duas latências e dois ritmos; um/dois escritores e 257/4096. Três escritores são controles adicionais. Conteúdo, ambas as FATs e auditoria EOC/vínculo/diretório por gravação foram conferidos. As contagens completas estão no JSON.

RAM e código dos objetos: `{"32": {"before": {"static_ram_bytes": 4760, "code_bytes": 32354}, "after": {"static_ram_bytes": 4760, "code_bytes": 32519}}, "64": {"before": {"static_ram_bytes": 4872, "code_bytes": 42465}, "after": {"static_ram_bytes": 4872, "code_bytes": 42446}}}`. Matriz PIC24 de 48 configurações aprovada, máximo de pilha interna **378 bytes**, incluindo PCs internos e excluindo backend/aplicação/libc.

Passaram 24 testes de alocação/cache e sete suítes funcionais. As campanhas nos três perfis incluem 82 cortes de escrita, 58 cortes durante recuperação e oito cenários de falha/retry por perfil. O teste de páginas FAT sujas agora monta explicitamente a reserva preexistente, porque a nova política evita criá-la quando há pedidos de pares já pendentes; a cobertura de ambos os ACKs e do fallback de disco quase cheio permanece.

As variantes experimentais staggered/durable não deram ganho relevante, e a remoção isolada da espera chegou a acrescentar duas gravações. Não foram incorporadas. Evidências: tests/build/small-write-pipeline. Persistem os limites anteriores da recuperação de volumes sujos.
