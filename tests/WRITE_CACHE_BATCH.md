# Cache de escrita: FAT preservada sob pressão de dados

FAT32 8.3, clusters de 512 bytes, cache de oito setores. A reserva especulativa continua sendo um único cluster. A política compartilhada atende fwrite/fputc e os modos w, a, w+, x, a+ e r+.

## Alteração

Uma página liberada pelo escritor recebe a preferência de descarte já existente. Ela só pode ser reutilizada depois do ACK, quando estiver IN_SYNC, sem lock nem retenção. Durante uma sessão de lotes marcada suja, a FAT limpa recebe preferência temporária sobre dados sujos que ainda podem ser gravados. Sob pressão, grava-se primeiro o dado liberado mais antigo, em vez de expulsar a FAT ou publicar um diretório antigo para abrir espaço. Sem página de dados liberável, mantém-se o fallback normal.

As barreiras explícitas continuam drenando dados e ambas as FATs antes da publicação do diretório. EOCs, vínculos, marcador de volume sujo/limpo e recuperação mantêm seus ACKs. Nenhuma nova reserva, buffer ou membro de estrutura foi acrescentado.

## Comparação principal

Arquivos de 1 MiB, latência simulada 0. Números são setores de 512 bytes, sem medição de CPU. A base é a versão anterior de 1799 metadados.

| Ritmo | Escritores/bloco | Leituras antes → depois | Metadados antes → depois | Escritas antes → depois |
|---|---|---:|---:|---:|
| serial | 1/257 | 17 → 17 | 4133 → 4133 | 6181 → 6181 |
| serial | 2/257 | 33 → 33 | 8326 → 8326 | 12422 → 12422 |
| serial | 1/4096 | 499 → 19 | 1367 → 1154 | 3415 → 3202 |
| serial | 2/4096 | 516 → 35 | 2758 → 2369 | 6854 → 6465 |
| interleaved | 1/257 | 17 → 17 | 4133 → 4133 | 6181 → 6181 |
| interleaved | 2/257 | 33 → 33 | 8323 → 8323 | 12419 → 12419 |
| interleaved | 1/4096 | 499 → 19 | 1367 → 1154 | 3415 → 3202 |
| interleaved | 2/4096 | 771 → 35 | 1799 → 1560 | 5895 → 5656 |

Caso intercalado 2/4096: **771 →35 leituras (−95,46%)**, **1799 →1560 metadados (−13,29%)**, **5895 →5656 escritas (−4,05%)**. FAT1/FAT2: 643 gravações cada; diretório: 274. As 35 leituras são 33 da FAT primária, uma da secundária e uma de diretório. O diretório sobe de 259 para 274 gravações; a redução de FAT compensa esse custo.

Upstream/master local 60c5f3580152410cb08342942cbbe3a30a449c24, conforme comparação anterior: 453 leituras, 5634 escritas, 1538 metadados nesse caso. Restam 22 escritas a mais (+0,39%), com duas FATs atualizadas; o upstream não grava a segunda FAT nesse workload.

## Modos e backends

Caso intercalado 2/4096; cada linha também foi validada com um escritor, pedidos de 257 bytes, ritmo serial e duas latências. r+ inclui a criação e o fechamento inicial dos arquivos vazios antes da reabertura; esse custo é idêntico nas duas versões.

| Modo/perfil | Leituras antes → depois | Metadados antes → depois |
|---|---:|---:|
| buffered_default | 771 → 35 | 1799 → 1560 |
| append_default | 771 → 35 | 1799 → 1560 |
| update_default | 771 → 35 | 1799 → 1560 |
| exclusive_default | 771 → 35 | 1799 → 1560 |
| byte_default | 33 → 33 | 8193 → 8193 |
| buffered_multi | 771 → 35 | 1799 → 1560 |
| buffered_query | 771 → 35 | 1799 → 1560 |
| buffered_single | 771 → 35 | 1799 → 1560 |
| append_read_default | 771 → 35 | 1799 → 1560 |
| reopen_default | 771 → 35 | 1803 → 1564 |

Os pedidos pequenos e fputc preservam as contagens anteriores. Não passam a reservar oito clusters: somente pedidos já cobrindo vários setores permitem lotes. Isso respeita a escolha do usuário de manter uma única reserva especulativa, mesmo quando o ganho varia.

A quantidade de dados enviada em sessões multibloco também muda: no caso principal, 3585 →253 setores. A pressão é resolvida mais cedo e o cache conserva metadados. O suporte multibloco continua ativo; não se afirma ganho de throughput em cartão físico a partir dessas contagens.

## Memória e validação

RAM estática idêntica antes/depois: `{"32": {"before": {"static_ram_bytes": 4760, "code_bytes": 31611}, "after": {"static_ram_bytes": 4760, "code_bytes": 32354}}, "64": {"before": {"static_ram_bytes": 4872, "code_bytes": 42660}, "after": {"static_ram_bytes": 4872, "code_bytes": 42465}}}`. Matriz PIC24 de 48 configurações/perfis/ABIs aprovada, máximo interno **378 bytes** (inclui PCs internos; exclui backend/aplicação/libc conforme o orçamento estabelecido).

As 40 execuções de modos/backends validaram **320 cenários de escrita e 320 releituras**, com conteúdo completo, FATs iguais e auditoria EOC/vínculo/diretório a cada gravação. Das 160 comparações pareadas de escrita, 72 melhoraram leituras ou metadados; as demais mantiveram as contagens, sem regressões de I/O medidas. Passaram 21 testes de alocação/cache, incluindo três campanhas de 82 cortes de escrita, 58 cortes durante recuperação e oito falhas/retry. Incluem buffers imutáveis, dados confirmados na raiz/subdiretório, escritores mistos e novo lote durante a publicação do marcador limpo. Sete suítes funcionais padrão/opcionais também passaram.

Permanecem os limites da recuperação da versão anterior: varredura potencialmente lenta em volumes sujos grandes e ausência de reparo geral de ciclos/corrupção arbitrária. Evidências: tests/build/write-cache-batch; medições no simulador, sem cartão físico.
