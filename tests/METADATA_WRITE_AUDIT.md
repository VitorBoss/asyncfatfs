# Auditoria do excesso de gravação de metadados

FAT32 8.3, clusters de 512 bytes, cache de oito setores, API original, LFN/exFAT/journal desligados. Base = versão atual de WRITE_DISPATCH_CPU. Upstream/master local: `60c5f3580152410cb08342942cbbe3a30a449c24`. Esta investigação não modifica a biblioteca de produção.

## Quantificação conferida

O excesso é real em todos os cenários abaixo. Metadados contam transferências de 512 bytes para FAT primária, espelho e diretório. Perfil 0 usa uma conclusão por próximo poll; perfil 1 usa latências simuladas diferenciadas de leitura/escrita. São contagens, não timings de CPU ou medições de comandos físicos do cartão.

Aumento = `(atual / upstream − 1) × 100`. Por exemplo, 557,76% **do** upstream significa 457,76% **a mais**, não 557,76% a mais.

| Ritmo | Perfil | Bloco | Escritores | Metadados atual/upstream | Aumento |
|---|---:|---:|---:|---:|---:|
| serial | 0 | 257 | 1 | 4133 / 2083 | +98.42% |
| serial | 0 | 257 | 2 | 8326 / 4165 | +99.90% |
| serial | 0 | 4096 | 1 | 4133 / 741 | +457.76% |
| serial | 0 | 4096 | 2 | 8324 / 1461 | +469.75% |
| serial | 1 | 257 | 1 | 4133 / 755 | +447.42% |
| serial | 1 | 257 | 2 | 8326 / 1474 | +464.86% |
| serial | 1 | 4096 | 1 | 4133 / 723 | +471.65% |
| serial | 1 | 4096 | 2 | 8324 / 1443 | +476.85% |
| interleaved | 0 | 257 | 1 | 4133 / 2083 | +98.42% |
| interleaved | 0 | 257 | 2 | 8324 / 1696 | +390.80% |
| interleaved | 0 | 4096 | 1 | 4133 / 741 | +457.76% |
| interleaved | 0 | 4096 | 2 | 8547 / 1538 | +455.72% |
| interleaved | 1 | 257 | 1 | 4133 / 755 | +447.42% |
| interleaved | 1 | 257 | 2 | 8324 / 1696 | +390.80% |
| interleaved | 1 | 4096 | 1 | 4133 / 723 | +471.65% |
| interleaved | 1 | 4096 | 2 | 8547 / 1507 | +467.15% |

Nesta fonte/cache, a faixa conferida é **+98.42% a +476.85%**. Faixas de relatórios anteriores podem usar outro cache/perfil; não misturar percentual do baseline com aumento.

## Causa dominante: granularidade da confirmação FAT

No serial, perfil 0:

| Bloco | Escritores | FAT primária atual/up | Espelho atual/up | Diretório atual/up | Entradas diferentes por imagem primária atual/up |
|---|---:|---:|---:|---:|---:|
| 257 | 1 | 2065 / 1056 | 2065 / 0 | 3 / 1027 | 1.984 / 2.909 |
| 257 | 2 | 4161 / 2129 | 4161 / 0 | 4 / 2036 | 1.969 / 3.819 |
| 4096 | 1 | 2065 / 378 | 2065 / 0 | 3 / 363 | 1.984 / 6.373 |
| 4096 | 2 | 4160 / 746 | 4160 / 0 | 4 / 715 | 1.969 / 6.902 |

A auditoria compara cada imagem FAT com a armazenada no instante da conclusão. A atual muda aproximadamente duas entradas por imagem, e grava cada cópia aproximadamente uma vez por cluster. O upstream acumula mais entradas alteradas por imagem, especialmente com pedidos de 4096 bytes. Ele também não mantém o espelho nesse workload.

Na atual, a sequência é: preparar EOC → confirmar ambas as FATs → ligar a cadeia → confirmar ambas → atualizar diretório. A reserva única adianta um EOC, compartilhando sua imagem com o vínculo anterior. Isso economiza uma rodada, mas ainda mantém a granularidade de um cluster na sequência serial. Há também imagens intermediárias EOC que ficam duráveis antes de serem substituídas pelo vínculo; o upstream pode coalescer essas alterações sem a mesma barreira.

Isso explica por que a diferença não é apenas 2× por haver duas FATs. O grande excesso de 4096 bytes está sobretudo na primária gravada mais frequentemente, somada ao espelho. O diretório serial já está muito reduzido; otimizá-lo sozinho não elimina a diferença.

## Gravações evitáveis encontradas na espera da reserva

Para dois escritores/4096 intercalados, a base faz 291 gravações do diretório. O rastreamento encontrou 288 envios genéricos enquanto um escritor estava em FIND_FREESPACE, aguardando a FAT da reserva. As imagens eram recentes, com idade inferior a 64; não foi o timeout de adiamento do background nem a pressão de cache que provocou esses envios.

O ponto é o for que espera as páginas FAT da reserva ficarem IN_SYNC em appendRegularFreeClusterContinue. Ele chama flushCache sem exclusão e pode selecionar o diretório antigo antes da FAT. As demais barreiras de alocação já usam flushAllocationCache para drenar FAT/dados deixando essa entrada antiga fora da submissão.

A cópia diagnóstica reserve_wait troca somente essa chamada pelo filtro de alocação existente. Não altera a checagem de confirmação das duas FATs antes de consumir a reserva. Não foi aplicada à produção nesta auditoria.

| Ritmo | Bloco | Escritores | Metadados base → diagnóstico | Diretório base → diagnóstico | FAT1/FAT2 base → diagnóstico |
|---|---:|---:|---:|---:|---:|
| serial | 257 | 1 | 4133 → 4133 | 3 → 3 | 2065/2065 → 2065/2065 |
| serial | 257 | 2 | 8326 → 8326 | 4 → 4 | 4161/4161 → 4161/4161 |
| serial | 4096 | 1 | 4133 → 4133 | 3 → 3 | 2065/2065 → 2065/2065 |
| serial | 4096 | 2 | 8324 → 8324 | 4 → 4 | 4160/4160 → 4160/4160 |
| interleaved | 257 | 1 | 4133 → 4133 | 3 → 3 | 2065/2065 → 2065/2065 |
| interleaved | 257 | 2 | 8324 → 8323 | 4 → 3 | 4160/4160 → 4160/4160 |
| interleaved | 4096 | 1 | 4133 → 4133 | 3 → 3 | 2065/2065 → 2065/2065 |
| interleaved | 4096 | 2 | 8547 → 8259 | 291 → 3 | 4128/4128 → 4128/4128 |

O perfil 1 reproduziu as mesmas contagens de metadados da base e desse diagnóstico. No intercalado 2/4096: **8547 → 8259 metadados (−3,37%)**, diretório **291 → 3**, FAT1/FAT2 inalteradas em 4128/4128. O excesso dominante da FAT continua existindo após eliminar esses 288 envios; não é uma solução para todo o intervalo.

## Controles isolados

| Variante | Serial 1/4096 | Intercalado 2/257 | Intercalado 2/4096 |
|---|---:|---:|---:|
| base | 4133 | 8324 | 8547 |
| no_reservation | 8193 | 8321 | 8321 |
| no_group | 4133 | 8326 | 8294 |
| data_pressure | 4133 | 8324 | 8547 |
| reserve_wait | 4133 | 8323 | 8259 |

- Sem reserva: a escrita serial passa de 4133 para 8193 metadados. A reserva já evita quase metade desse custo; removê-la globalmente é contraproducente.
- Sem agrupamento: melhora um caso de 4096 ao evitar o envio intermediário do diretório, mas piora 2/257. Corrigir a espera da reserva preserva o agrupamento e supera esse controle.
- Flush de background também para pressão de dados: não mudou as contagens. O excesso de diretório rastreado vem da espera da reserva, não desse fallback.

## Interpretação e próximo alvo técnico

Há uma ineficiência concreta de flush do diretório e um custo maior, estrutural ao protocolo atual: confirmação FAT em granularidade de cluster. Nenhuma imagem FAT atual já idêntica foi regravada nos workloads base auditados anteriormente. Portanto o alvo principal é compartilhar imagens de alocações já solicitadas dentro da mesma fase de durabilidade, preservando EOC confirmado antes do vínculo e vínculo confirmado antes da publicação. Isso exige investigar a granularidade da máquina de estados; não se resolve apenas aumentando o cache ou reduzindo memcpy.

O diagnóstico reserve_wait tem integridade/ordenação verificadas nos workloads abaixo, mas ainda não recebeu avaliação completa de CPU, pilha, fragmentação, pressão extrema e falhas necessária para promovê-lo à produção. Não usar os timings desses binários instrumentados para afirmar ganho de desempenho.

## Validação e reprodução

Foram executados os dois ritmos, dois perfis de latência, um/dois escritores e 257/4096, nas seis variantes: **96 cenários de escrita**, com releitura integral, auditoria de conteúdo e, nas variantes atuais, igualdade das FATs e checagens por gravação de EOC/vínculo/diretório. As bases de perfil 0 reproduziram os contadores de IO_CPU_RELATION. Instrumentação e alterações experimentais ficam em cópias congeladas em tests/build; produção, RAM, cache e pilha da biblioteca não mudaram.

MSUB registra a origem/contexto da submissão; FAT_HIST registra quantas entradas de quatro bytes diferiam na conclusão. As contagens de submissões conferem com as de conclusão. Não houve injeção de falhas nesses diagnósticos; os checks são do workload bem-sucedido e não substituem uma campanha de corte de energia.

Evidências: `tests/build/metadata-write-audit`. JSON ao lado inclui contadores, origens agregadas, exemplos de diretório e histogramas completos. manifest.json registra a fonte base, inalterada em produção.

```powershell
python tests/metadata_write_audit.py --directory tests/build/metadata-write-audit
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/metadata-write-audit
python tests/metadata_write_report.py --directory tests/build/metadata-write-audit --output tests/METADATA_WRITE_AUDIT.md
```
