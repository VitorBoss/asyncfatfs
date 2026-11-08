# FAT32 8.3 com reserva antecipada e agrupamento de metadados

Data: 16/11/2026, America/Sao_Paulo.

A nova implementação reduz novamente as gravações de metadados sem acrescentar campos, buffers ou variáveis estáticas à biblioteca. Para um escritor e clusters de 512 bytes, passa de 6.178 para 4.183 setores de metadados e de 8.226 para 6.231 setores escritos no total. A taxa no mesmo modelo de latência sobe aproximadamente 32%, de 0,151 para 0,200 MiB/s. O resultado com dois escritores depende do tamanho dos blocos e da pressão sobre o cache.

A técnica anterior de atualizar EOC e vínculo na mesma imagem foi removida. Um teste novo confirmou que ela podia publicar um vínculo antes de seu EOC em uma gravação parcial. A substituta usa o EOC já persistido nas duas FATs e conserva a barreira do vínculo antes da publicação do tamanho.

## Alterações implementadas

1. Uma reserva antecipada, no máximo um cluster, é marcada como EOC junto com a gravação de um vínculo cujo destino já é durável. A próxima alocação só consome essa reserva após os ACKs das duas FATs. Isso evita repetir uma gravação exclusiva para o novo EOC. A reserva não utilizada é liberada em flush, sync, fechamento, truncamento, necessidade de alocação de diretório ou ociosidade.
2. Durante a alocação de arquivos com clusters de 512 bytes, as barreiras drenam FAT e dados, mas podem manter em cache a página de diretório que ainda anuncia a cadeia anterior, já durável. Depois, o tamanho dessa página é atualizado para a nova cadeia. O background pode agrupar atualizações recentes dessa página; na ausência de atividade, o adiamento é limitado por 64 ticks do relógio de cache. Pressão de cache continua podendo forçar sua saída.
3. A FAT primária é adquirida e retida sem marcá-la como suja até a aquisição da página secundária. Uma tentativa sem cache suficiente não cria uma gravação inútil da primária. A retenção termina antes de retornar, inclusive em tentativas incompletas.

A reserva usa o bit alto do campo de hint `lastClusterAllocated`, que já existia. Endereços FAT32 ocupam 28 bits; o marcador é somente de runtime e nunca é escrito em campos de disco. O tamanho de `afatfs_t` e dos handles não muda.

## Garantias e limites de persistência

- Nenhum novo vínculo é enviado para o backend antes de seu destino já possuir EOC durável na mesma cópia da FAT. A prova usa a imagem anterior ao write, não somente o setor depois de gravado.
- A publicação de tamanho continua depois da confirmação do encadeamento. A página adiada contém somente uma publicação anterior segura até ser substituída; o adiamento não autoriza publicar uma cadeia pendente.
- Sync e fechamento liberam o buffer de dados e aguardam as gravações e o encerramento do backend como antes. Flush explícito não usa a política de atraso, libera reservas e conserva seu contrato de drenar páginas desbloqueadas, sem transformar a cauda bloqueada de um arquivo em dado sincronizado.
- Um diretório pode usar o último cluster livre que estava reservado; o teste de volume quase cheio cobre esse caso. Ociosidade sem novas chamadas de escrita também libera a reserva e termina; não depende de um fechamento futuro.
- Falhas de envio ou ACK, cancelamento de mídia e autorização de desligamento continuam seguindo as máquinas de estado existentes. Não foi alterado o significado de `afatfs_canPowerOff`.

FAT32 sem diário continua sem recuperação atômica geral após queda. Uma interrupção antes do ponto de sincronização pode deixar um EOC não ligado, como em uma alocação interrompida. A reserva limita esse estado antecipado a um cluster e o elimina nos pontos de sincronização e no encerramento limpo. Os testes novos não prometem recuperação de corrupção arbitrária, de quaisquer combinações de bytes parcialmente gravados ou de firmware/backend que reconheça dados ainda não duráveis.

## Comparação medida com clusters de 512 bytes

Cada arquivo tem 1 MiB. Um ou dois arquivos são intercalados na mesma thread, com quatro setores de cache e três handles. Sete repetições por caso, dois perfis de backend. O controle estrito desativa o pipeline e conserva EOC durável antes do vínculo; a versão anterior é a implementação de mesmo setor que foi substituída. O upstream igualado usa o commit `60c5f3580152410cb08342942cbbe3a30a449c24`, com o mesmo cache e limiar de múltiplos blocos.

### Setores de metadados e total escrito

Valores em setores de 512 bytes, perfil 1. As comparações com upstream são históricas e mantêm o comportamento daquele commit, que não atualizou a segunda FAT neste ensaio.

| Bloco B | Arquivos | Controle meta/total | Anterior meta/total | Nova meta/total | Upstream igualado meta/total |
| --- | --- | --- | --- | --- | --- |
| 257 | 1 | 10240 / 12288 | 6178 / 8226 | 4183 / 6231 | 2080 / 4128 |
| 257 | 2 | 20479 / 24575 | 12419 / 16515 | 8399 / 12495 | 4257 / 8353 |
| 4096 | 1 | 10240 / 12288 | 6178 / 8226 | 4183 / 6231 | 2080 / 4128 |
| 4096 | 2 | 20479 / 24575 | 12417 / 16513 | 8390 / 12486 | 4222 / 8318 |

### Throughput no modelo de latência

| Bloco B | Arquivos | Controle MiB/s | Anterior MiB/s | Nova MiB/s | Upstream igualado MiB/s | Ganho vs anterior |
| --- | --- | --- | --- | --- | --- | --- |
| 257 | 1 | 0.101 | 0.151 | 0.199 | 0.302 | 31.3% |
| 257 | 2 | 0.097 | 0.142 | 0.183 | 0.298 | 29.1% |
| 4096 | 1 | 0.102 | 0.152 | 0.200 | 0.302 | 32.0% |
| 4096 | 2 | 0.101 | 0.151 | 0.199 | 0.300 | 32.3% |

Com um escritor, o ganho fica perto de 97% sobre o controle estrito e 32% sobre a versão anterior. Com dois escritores, a nova taxa fica em aproximadamente 0,183 MiB/s para blocos de 257 bytes e 0,199 MiB/s para blocos de 4 KiB. A diferença para o upstream não foi inteiramente eliminada.

### Separação das gravações de metadados

| Versão | Bloco B | Arquivos | FAT primária | FAT secundária | Diretório |
| --- | --- | --- | --- | --- | --- |
| strict_control | 257 | 1 | 4095 | 4095 | 2050 |
| strict_control | 257 | 2 | 8190 | 8190 | 4099 |
| strict_control | 4096 | 1 | 4095 | 4095 | 2050 |
| strict_control | 4096 | 2 | 8190 | 8190 | 4099 |
| final | 257 | 1 | 2082 | 2082 | 19 |
| final | 257 | 2 | 4194 | 4194 | 11 |
| final | 4096 | 1 | 2082 | 2082 | 19 |
| final | 4096 | 2 | 4193 | 4193 | 4 |
| upstream_equal | 257 | 1 | 1055 | 0 | 1025 |
| upstream_equal | 257 | 2 | 2176 | 0 | 2081 |
| upstream_equal | 4096 | 1 | 1055 | 0 | 1025 |
| upstream_equal | 4096 | 2 | 2142 | 0 | 2080 |

No controle de um escritor, o diretório era gravado 2.050 vezes; na versão nova são 19. As escritas de cada FAT caem de 4.095 para 2.082. Na investigação intermediária com dois escritores e blocos de 257 bytes, a primária tinha 8.210 gravações e a secundária 4.194; adiar a marcação de dirty reduziu a primária para as mesmas 4.194 gravações da secundária.

### Leituras e clusters de 4 KiB

As fases de leitura mantiveram as contagens de setores do controle nos dois perfis. Durante a escrita com dois arquivos e blocos de 257 bytes, as leituras passam de 4.218 para 4.226, aumento de oito setores, abaixo de 0,2%. Com blocos de 4 KiB, caem de 223 para 160. Não se deve interpretar a redução de writes como uma redução uniforme de todo tipo de I/O.

O adiamento de páginas de diretório foi restrito a clusters de 512 bytes. Com clusters de 4 KiB, a reserva antecipada preserva aproximadamente o ganho da versão anterior: um escritor fica perto de 0,578 MiB/s no modelo, contra 0,470 no controle. Não houve ampliação do cache.

## Memória e escopo

A otimização é automática somente no build mínimo: API de compatibilidade, LFN, exFAT, freefile e logging introspectivo desabilitados. FAT16 mantém seu fluxo; builds com diário mantêm a estratégia existente de captura/replay. `AFATFS_DISABLE_ALLOCATION_PIPELINE` permite medir o caminho sem a otimização.

Os 24 layouts foram comparados com a matriz anterior: PIC24 normal/enums curtos, x86 e x64, em seis configurações. Todos são idênticos. A RAM estática padrão da biblioteca permanece 2.494 bytes em PIC24, 2.474 com enums curtos, 2.612 em x86 e 2.720 em x64. Não há novo heap, buffer, membro ou variável estática na biblioteca. Esses valores excluem a pilha e a instrumentação do backend. O pico de pilha não foi medido; o XC16 disponível não suporta `-fstack-usage`.

## Validações

- Controle estrito e versão final: 112 volumes cada, 448 fases medidas no conjunto. Tamanho, conteúdo, cadeias exatas, ausência de clusters extras e igualdade final das duas FATs foram auditados independentemente. I/O e tempo virtual se repetiram exatamente nas sete execuções de cada cenário.
- Auditoria de pré-imagem e imagem depois de cada write FAT, mais verificação de que o tamanho publicado cabe no encadeamento já persistido. Inclui travessias de setor, dois escritores alternados, inatividade e três escritores.
- Controle negativo: a biblioteca anterior foi rejeitada pelo novo teste de pré-imagem, pois o destino ainda estava livre no meio ao enviar o vínculo. Esse teste mostra por que manter apenas a auditoria pós-write era insuficiente.
- Teste de volume quase cheio: dois clusters de dados e o último cluster livre para um diretório, com remontagem e conferência de conteúdo.
- Matriz de regressão: sete configurações, incluindo falhas de envio/ACK, acesso, fechamento, remoção de mídia e desligamento.
- Cache e múltiplos blocos: testes de pre-erase destrutivo, BUSY/FAILURE, locks e interrupção de sequências. O teste adicional `fat32_metadata_hint.c` confirma páginas distintas, retenção balanceada e ausência de hints de pre-erase futuro nas duas FATs.
- Compilação e asserts de layout nas 24 combinações; não é execução PIC24 em hardware.

As taxas são de um backend assíncrono em RAM com tempo virtual: poll de 10 µs, leitura de setor de 200 µs, escrita isolada de 800 µs, escrita múltipla de 400 µs e 100 µs de início/fim de sequência. Não são velocidades de cartão SD físico. A comparação de throughput usa esse mesmo modelo e a mesma carga do relatório upstream.

## Evidências e reprodução

- `lib/asyncfatfs.c`: aquisição das páginas FAT, reserva antecipada, consumo depois do ACK, limpeza e drenagem de diretório.
- `tests/upstream_comparison.c`: carga, contadores de setores, auditoria de pré-imagem, cadeias publicadas, inatividade, três escritores e volume quase cheio.
- `tests/run_fat32_allocation_checks.ps1`: executa os testes adicionais de persistência e borda.
- `tests/build/fat32-83-pipeline-20261116/`: snapshots das fontes, binários, CSVs, traces por FAT/diretório, controles negativos, layouts e logs.
- `tests/FAT32_83_PIPELINE.json`: dados consolidados e hashes das fontes.

```powershell
./tests/run_fat32_allocation_checks.ps1
./tests/run_exfat_tests.ps1
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/nova-rodada/layout
```
