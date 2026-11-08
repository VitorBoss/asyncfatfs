# Agrupamento de alocações com as duas páginas FAT ainda sujas

Implementado em lib/asyncfatfs.c, no pipeline nativo FAT32 8.3. Cache **oito setores**, clusters 512 bytes, arquivos de 1 MiB por escritor. A fonte final incorpora a candidata, sua documentação e o fallback ao esgotar a busca alternativa; o I/O e a matriz foram novamente verificados nessa fonte final.

Quando uma reserva ainda suja bloqueia um escritor com pedido já pendente, a biblioteca preserva a reserva global e procura um cluster livre para esse pedido. Isso só ocorre com outro escritor pendente, sem flush em andamento, e com **ambas as cópias da página FAT em DIRTY**. O novo EOC compartilha o par ainda não enviado e continua passando por NEW_CLUSTER_DURABLE antes do vínculo; FAT_DURABLE continua protegendo a publicação do diretório.

Se uma cópia já está IN_SYNC, ausente, em leitura ou em trânsito, a biblioteca mantém a espera anterior. Não amplia a reserva especulativa: permanece um cluster global, e cada escritor conserva um único alvo de alocação pendente. Nenhum campo/array/buffer foi acrescentado à biblioteca.

Quando a busca alternativa não encontra espaço após o hint, ela volta a aguardar a reserva existente em vez de reportar ENOSPC. Falhas fatais de leitura mantêm o tratamento anterior. O teste específico cobre esse limite no fim da FAT, seguido de escrita e remontagem.

## Metadados: antes → depois

Resultados dos dois perfis de latência são idênticos para a biblioteca atual. Valores abaixo são setores de 512 bytes, na latência 0; as duas latências estão no JSON. Serial termina cada pedido antes de tentar o próximo; intercalado atende os escritores antes de avançar o backend.

| Ritmo | Escritores/bloco | FAT1 antes → depois | FAT2 antes → depois | Diretório | Metadados antes → depois | Redução |
|---|---|---:|---:|---:|---:|---:|
| serial | 1/257 | 2065 → 2065 | 2065 → 2065 | 3 | 4133 → 4133 | 0.000% |
| serial | 2/257 | 4161 → 4161 | 4161 → 4161 | 4 | 8326 → 8326 | 0.000% |
| serial | 1/4096 | 2065 → 2065 | 2065 → 2065 | 3 | 4133 → 4133 | 0.000% |
| serial | 2/4096 | 4160 → 4160 | 4160 → 4160 | 4 | 8324 → 8324 | 0.000% |
| interleaved | 1/257 | 2065 → 2065 | 2065 → 2065 | 3 | 4133 → 4133 | 0.000% |
| interleaved | 2/257 | 4160 → 4160 | 4160 → 4160 | 3 | 8323 → 8323 | 0.000% |
| interleaved | 1/4096 | 2065 → 2065 | 2065 → 2065 | 3 | 4133 → 4133 | 0.000% |
| interleaved | 2/4096 | 4128 → 4095 | 4128 → 4095 | 3 | 8259 → 8193 | 0.799% |
| serial | 3/257 | 6240 → 6240 | 6240 → 6240 | 5 | 12485 → 12485 | 0.000% |
| serial | 3/4096 | 6287 → 6287 | 6287 → 6287 | 5 | 12579 → 12579 | 0.000% |
| interleaved | 3/257 | 6176 → 6127 | 6176 → 6127 | 4 | 12356 → 12258 | 0.793% |
| interleaved | 3/4096 | 5920 → 5868 | 5920 → 5868 | 4 | 11844 → 11740 | 0.878% |

Os ganhos medidos ficam nos pedidos intercalados: **2/4096 −0,799%**, **3/257 −0,793%**, **3/4096 −0,878%** dos metadados. Os demais casos mantêm a contagem. A diferença grande para upstream ainda permanece; esta alteração elimina separações evitáveis de imagens, sem retirar os pontos de confirmação.

## RAM e pilha

| ABI | RAM antes → depois |
|---|---:|
| x86 (32 bits) | 4760 → 4760 B |
| x64 | 4872 → 4872 B |
| PIC24 normal | 4614 → 4614 B |
| PIC24 short | 4586 → 4586 B |

As **48 configurações/perfis/ABIs PIC24** passaram com RAM estática inalterada. Pilha interna padrão 270/264 bytes (normal/short), leitura multibloco 274/268; máximo entre opcionais **378 bytes**, abaixo de 400. Inclui chamadas internas e PCs de retorno; exclui backend, aplicação e libc conforme o orçamento estabelecido.

## Persistência e validação

- 48 cenários de escrita e 48 releituras antes/depois, com um, dois e três escritores, 257/4096 bytes, dois ritmos e duas latências. Auditoria por gravação de EOC/vínculo/diretório, conteúdo completo, igualdade das FATs e reconciliação das categorias/histogramas.
- Outros 48 cenários de escrita/releitura com o backend congelando a imagem na submissão. Mesmo I/O e **zero alterações dos buffers em trânsito** nas duas versões.
- 18 testes de alocação/cache na produção e sete suítes funcionais padrão/opcionais. Teste específico nos três perfis (padrão, multibloco, consulta): espera enquanto a página está em trânsito ou o par está parcialmente confirmado; reserva preservada; EOCs de pedidos reais agrupados; três ACKs falhos/retry em cada FAT; vínculos e diretório retidos até confirmação; busca alternativa esgotada sem falso ENOSPC; conteúdo após remontagem e reserva liberada.
- O limite original de propriedade foi reproduzido por fragmentação válida e chamadas públicas: dois escritores pendentes + uma reserva = três EOCs, três escritores + uma reserva = quatro. A candidata mantém um alvo por escritor e uma reserva global. Não confundir isso com lotes de vários alvos por escritor.
- A campanha não é uma prova de todos os pontos de corte físico. As barreiras não foram removidas; os testes incluem ACKs falhos com escrita parcial e reinicialização nos diagnósticos de propriedade. Não houve medição ou alegação de ganho de CPU.

## Alternativas rejeitadas na investigação

Priorizar passagens ou apenas adiar a drenagem não reduziu as contagens nos casos principais. Permitir novas alterações quando apenas a FAT2 ainda estava suja causou regravações adicionais da FAT1. Lotes de até oito clusters reduziram muito mais os metadados (2/4096 intercalado: 8259 → 1985), mas um corte após EOCs duráveis e antes dos vínculos deixou até 16 clusters sem ligação para dois escritores. Eles não foram incorporados: ampliar o lote sem registrar propriedade durável aumenta a alocação perdida após reiniciar.

## Evidências e reprodução

Evidências congeladas em tests/build/fat-batch. raw/base/lib é a versão anterior; raw/fresh_pair_dirty/lib é a candidata inicial; normal/lib é a produção final com documentação e fallback. I/O final em adopted/adopted-three, backend congelado final em adopted-snapshot/adopted-snapshot-three; controles anteriores em serial/interleaved, three e snapshot/snapshot-three. Matriz final em matrix-production; falhas e guardas em production-targeted; runner integrado em tests/run_fat32_allocation_checks.ps1. fat_batch_investigation.py reconstrói variantes a partir da base congelada, sem modificar produção. Este relatório valida os hashes, contadores, resultados e matriz.

```powershell
python tests/fat_dirty_grouping_report.py --prepare
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted -Variants fresh_pair_dirty
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-three -Variants fresh_pair_dirty
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-snapshot -Variants fresh_pair_dirty
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-batch/adopted-snapshot-three -Variants fresh_pair_dirty
python tests/fat_dirty_grouping_report.py
./tests/run_fat32_allocation_checks.ps1 -OutputDirectory tests/build/fat-dirty-grouping-checks
```
