# Origem das gravações de metadados em afatfs_poll()

Diagnóstico isolado: FAT32 8.3, cache oito setores, clusters 512 bytes, arquivos de 1 MiB. Dois ritmos de pedidos, duas latências, um/dois escritores e blocos de 257/4096 bytes. Timings instrumentados descartados; produção não modificada.

## Conclusões

Upstream/master local verificado: `60c5f3580152410cb08342942cbbe3a30a449c24`. Passaram **160 cenários de escrita e 160 releituras**, com a base reproduzindo o I/O da seção RESERVE_FLUSH. Nenhum timing instrumentado foi usado para afirmar ganho de CPU.

- No padrão, o ramo AFATFS_PIPELINE_ALLOCATION chama flushCacheFiltered(0, true). As chamadas literais a flushCache() no poll estão nos ramos de journal ou sem pipeline, inativos nesta configuração.
- O flush periódico inicia muitas gravações, mas retirá-lo não elimina nenhuma gravação FAT nos 16 casos da base: as barreiras em fileOperationsPoll assumem o envio. A economia fica em zero/um setor de diretório. Omitir flush periódico em produção prejudicaria o escoamento em idle.
- A segunda passagem condicional reduz I/O. Em 2/4096 intercalado, retirá-la leva FAT1 de 4128 a 4160 e metadados de 8259 a 8291; FAT2 fica em 4128. O histograma mostra 32 imagens de duas entradas substituídas por 64 imagens de uma entrada, sem no-ops. Em 2/257, a remoção adiciona um setor de metadados. Torná-la incondicional ou executar passagens extras não reduz I/O nos 16 casos.
- Em 2/4096 intercalado, a base envia FAT1/FAT2: 256/288 no background anterior às operações, 3839/3840 na primeira passagem e 33/0 na segunda; o diretório tem duas submissões na primeira passagem e uma fora do poll. A chamada final de background não submeteu metadados nesse caso. Isso é atribuição de envio; o A/B mostra que suprimir o background transfere esse trabalho para as barreiras.
- O novo desperdício localizado é a imagem antiga do diretório no início de fsync/close. Filtrar apenas a fase 1 ajuda alguns casos; também deferir o background nas fases 0/1 elimina um setor de diretório em cada um dos 16 casos, mantendo as contagens das FATs. É uma redução de apenas 0,012–0,024% dos metadados, ainda sem qualificação completa para produção.
- Transferir a reserva ainda pendente ao escritor, preservando a barreira NEW_CLUSTER_DURABLE, não melhorou I/O: em 2/4096 intercalado também resultou em 8291 metadados. Não adotar essa variante com base nestes resultados.
- O alvo dominante continua sendo a granularidade de publicação das alocações: reunir alterações já solicitadas dentro da mesma fase EOC/vínculo. Aumentar o número de chamadas do escalonador, por si só, não reúne mais alterações nos casos medidos.

As variantes no_pre_background/no_background omitem flushes oportunistas do poll, mantendo flushes de barreiras, cache cheio, fsync e fechamento. São controles causais, não políticas aprovadas de persistência/flush em idle. single_pass retira apenas a segunda passagem condicional; second_unconditional a executa sem a condição; extra_passes executa três passagens naquele ponto.

sync_filter deixa o diretório antigo fora da drenagem de dados/FAT da fase 1 de fsync/close. sync_filter_defer também permite a deferral de background para esse diretório nas fases 0/1 de fsync/close, mantendo o limite de idade 64. A fase 2 escreve a entrada final e a fase 3 mantém o flush explícito e o ACK antes do callback. São candidatas diagnósticas, sem promoção à produção ou qualificação completa de CPU/pilha/falhas.

adopt_pending_reservation transfere a reserva única com EOC ainda pendente ao escritor que já pediu alocação. Usa NEW_CLUSTER_DURABLE para confirmar as duas FATs antes do vínculo; a reserva global é consumida, sem criar uma segunda reserva. Diagnóstico de agrupamento, não implementação aprovada.

## serial: contagens de metadados (latência 0 / 1)

| Variante | 1/257 | 2/257 | 1/4096 | 2/4096 |
|---|---:|---:|---:|---:|
| base | 4133 / 4133 | 8326 / 8326 | 4133 / 4133 | 8324 / 8324 |
| no_pre_background | 4132 / 4132 | 8325 / 8325 | 4132 / 4132 | 8323 / 8323 |
| no_background | 4132 / 4132 | 8325 / 8325 | 4132 / 4132 | 8323 / 8323 |
| single_pass | 4133 / 4133 | 8326 / 8326 | 4133 / 4133 | 8324 / 8324 |
| second_unconditional | 4133 / 4133 | 8326 / 8326 | 4133 / 4133 | 8324 / 8324 |
| extra_passes | 4133 / 4133 | 8326 / 8326 | 4133 / 4133 | 8324 / 8324 |
| sync_filter | 4133 / 4133 | 8325 / 8325 | 4133 / 4133 | 8323 / 8323 |
| sync_filter_defer | 4132 / 4132 | 8325 / 8325 | 4132 / 4132 | 8323 / 8323 |
| adopt_pending_reservation | 4133 / 4133 | 8326 / 8326 | 4133 / 4133 | 8324 / 8324 |
| upstream | 2083 / 755 | 4165 / 1474 | 741 / 723 | 1461 / 1443 |

### Origem direta das submissões na base (latência 0)

| Arquivos/bloco | Etapa | FAT1 | FAT2 | Diretório | Chamadas |
|---|---|---:|---:|---:|---:|
| 1/257 | release_idle_reservation | 0 | 0 | 0 | 10285 |
| 1/257 | pre_background | 2049 | 2065 | 2 | 10276 |
| 1/257 | first_operations | 16 | 0 | 1 | 10280 |
| 2/257 | outside_poll | 7 | 0 | 1 | 0 |
| 2/257 | release_idle_reservation | 0 | 0 | 0 | 20618 |
| 2/257 | pre_background | 4089 | 4161 | 1 | 20605 |
| 2/257 | first_operations | 65 | 0 | 2 | 20613 |
| 1/4096 | release_idle_reservation | 0 | 0 | 0 | 6463 |
| 1/4096 | pre_background | 2049 | 2065 | 2 | 6451 |
| 1/4096 | first_operations | 16 | 0 | 1 | 6458 |
| 2/4096 | outside_poll | 0 | 0 | 1 | 0 |
| 2/4096 | release_idle_reservation | 0 | 0 | 0 | 12974 |
| 2/4096 | pre_background | 4096 | 4160 | 1 | 12961 |
| 2/4096 | first_operations | 64 | 0 | 2 | 12969 |

## interleaved: contagens de metadados (latência 0 / 1)

| Variante | 1/257 | 2/257 | 1/4096 | 2/4096 |
|---|---:|---:|---:|---:|
| base | 4133 / 4133 | 8323 / 8323 | 4133 / 4133 | 8259 / 8259 |
| no_pre_background | 4132 / 4132 | 8323 / 8323 | 4132 / 4132 | 8259 / 8259 |
| no_background | 4132 / 4132 | 8323 / 8323 | 4132 / 4132 | 8259 / 8259 |
| single_pass | 4133 / 4133 | 8324 / 8324 | 4133 / 4133 | 8291 / 8291 |
| second_unconditional | 4133 / 4133 | 8323 / 8323 | 4133 / 4133 | 8259 / 8259 |
| extra_passes | 4133 / 4133 | 8323 / 8323 | 4133 / 4133 | 8259 / 8259 |
| sync_filter | 4133 / 4133 | 8322 / 8322 | 4133 / 4133 | 8258 / 8258 |
| sync_filter_defer | 4132 / 4132 | 8322 / 8322 | 4132 / 4132 | 8258 / 8258 |
| adopt_pending_reservation | 4133 / 4133 | 8323 / 8323 | 4133 / 4133 | 8291 / 8291 |
| upstream | 2083 / 755 | 1696 / 1696 | 741 / 723 | 1538 / 1507 |

### Origem direta das submissões na base (latência 0)

| Arquivos/bloco | Etapa | FAT1 | FAT2 | Diretório | Chamadas |
|---|---|---:|---:|---:|---:|
| 1/257 | release_idle_reservation | 0 | 0 | 0 | 10285 |
| 1/257 | pre_background | 2049 | 2065 | 2 | 10276 |
| 1/257 | first_operations | 16 | 0 | 1 | 10280 |
| 2/257 | outside_poll | 0 | 0 | 1 | 0 |
| 2/257 | release_idle_reservation | 0 | 0 | 0 | 16542 |
| 2/257 | pre_background | 1 | 2079 | 0 | 8273 |
| 2/257 | first_operations | 4158 | 2081 | 2 | 16537 |
| 2/257 | second_operations | 1 | 0 | 0 | 1 |
| 1/4096 | release_idle_reservation | 0 | 0 | 0 | 6463 |
| 1/4096 | pre_background | 2049 | 2065 | 2 | 6451 |
| 1/4096 | first_operations | 16 | 0 | 1 | 6458 |
| 2/4096 | outside_poll | 0 | 0 | 1 | 0 |
| 2/4096 | release_idle_reservation | 0 | 0 | 0 | 12653 |
| 2/4096 | pre_background | 256 | 288 | 0 | 1343 |
| 2/4096 | first_operations | 3839 | 3840 | 2 | 12648 |
| 2/4096 | second_operations | 33 | 0 | 0 | 33 |

## Limites e reprodução

Nas variantes atuais, as checagens EOC/vínculo/diretório são executadas a cada gravação, com releitura integral e igualdade das duas FATs. Upstream recebe verificação de conteúdo, sem exigir o espelhamento que ele não mantém neste workload. Não houve injeção de falhas nesta rodada. A matriz anterior de RAM/pilha pertence à produção inalterada; os contadores e arrays novos existem somente nas cópias diagnósticas.

Os caminhos inflight_operations e post_background não foram exercitados como emissores de metadados na base destes workloads; isso não prova que sejam dispensáveis em outros backends, latências ou números de escritores.

JSON inclui histogramas de entradas alteradas, fases de alocação por etapa e contadores de I/O completos.

```powershell
python tests/poll_metadata_audit.py --prepare
$variants = (Get-Content tests/build/poll-metadata/manifest.json | ConvertFrom-Json).variants
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/poll-metadata -Variants $variants
python tests/poll_metadata_audit.py
```
