# Amplificação de metadados: transições FAT realmente gravadas

FAT32 8.3, cache oito setores, clusters 512 bytes. Upstream/master local `60c5f3580152410cb08342942cbbe3a30a449c24`. Somente contagem de metadados; nenhum resultado de CPU nesta rodada. Produção inalterada.

São transições entre a imagem anterior no dispositivo simulado e cada imagem concluída, não contagens de chamadas FATSetNextCluster. FREE→LINK significa que um EOC intermediário não chegou a ser persistido naquela entrada antes do vínculo. Cada registro de FAT1/FAT2 representa um setor de 512 bytes.

## Razão da diferença

A FAT2 explica só parte do excesso. No serial 2/4096, o excesso de 6863 setores de metadados se decompõe exatamente em **+3414 FAT1 +4160 FAT2 −711 diretório**. A contagem maior de FAT1 vem do avanço limitado por confirmações a cada cluster e da persistência separada de estados EOC intermediários, enquanto o upstream acumula mais alterações antes de enviar uma imagem.

Exemplo serial 1/4096: a atual persiste 2049 transições FREE→EOC e 2047 EOC→LINK, além de uma liberação da reserva. O upstream persiste 362 FREE→EOC, **1686 FREE→LINK diretamente** e 361 EOC→LINK. Assim, muitas entradas do upstream chegam ao dispositivo já com seu vínculo final, sem uma imagem EOC intermediária separada. A atual registra 4097 transições em 2065 imagens; upstream 2409 em 378. As médias são 1,984 e 6,373 entradas alteradas por imagem.

O traço inicial mostra a causa no avanço: na atual, a imagem 0 confirma EOC dos clusters 3 e 4 antes de retornar dados aceitos; após 512 bytes, a imagem 1 publica 3→4 e reserva EOC de 5; após 1024 bytes, a imagem 2 publica 4→5 e reserva EOC de 6. O ciclo se repete por cluster. O upstream aceita 3072 bytes antes da primeira conclusão FAT1 e essa imagem já contém 3→4→5→6→7→8→9→EOC. A reserva única atual já compartilha um EOC futuro com o vínculo anterior, mas não transforma esse fluxo em um lote de vários clusters por confirmação.

A contagem atual 2065 fecha exatamente por fase: **17 imagens em NEW_CLUSTER_DURABLE +2047 em FAT_DURABLE +1 no fechamento**. As 17 imagens EOC correspondem a 17 setores FAT distintos (inicial e 16 mudanças de página FAT); o fechamento devolve a reserva não utilizada. São 2048 clusters de dados: 2048 +16 mudanças de página +1 limpeza =2065 imagens FAT1. A quantidade permanece igual com pedidos de 257 ou 4096 bytes.

No código atual, NEW_CLUSTER_DURABLE confirma EOC nas duas FATs; UPDATE_FAT2 escreve o vínculo e uma eventual reserva seguinte; FAT_DURABLE drena novamente antes da publicação do diretório/conclusão. No upstream, UPDATE_FAT1 e UPDATE_FAT2 seguem para o diretório e COMPLETE quando as páginas estão modificadas no cache, sem essas duas fases explícitas de ACK. Essa diferença de protocolo reduz o agrupamento na atual. Remover os ACKs para imitar a contagem upstream não seria uma otimização com as mesmas garantias.

A observação sobre pre_background está correta como local do envio: no serial 2/4096 ele submete **8257 de 8324 metadados**. O controle sem background, porém, mantém **4160 FAT1 +4160 FAT2** e transfere essas 8320 gravações FAT para first_operations. O único setor poupado é de diretório. Portanto o excesso de FAT não nasce do número de chamadas pre_background; ele já é imposto pelas páginas/barreiras que ficam pendentes entre os clusters.

Não há atribuição de CPU nesta análise. Um custo de CPU associado ao I/O adicional continua possível; o nome do ponto de envio não identifica sozinho a causa de esse I/O existir.

## serial, latência 0

| Escritores/bloco | Versão | FAT1 | FAT2 | Diretório | FREE→EOC FAT1 | FREE→LINK FAT1 | EOC→LINK FAT1 | EOC→FREE FAT1 | Entradas alteradas/FAT1 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1/257 | base | 2065 | 2065 | 3 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/257 | no_background | 2065 | 2065 | 2 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/257 | upstream | 1056 | 0 | 1027 | 1025 | 1023 | 1024 | 0 | 2.909 |
| 2/257 | base | 4161 | 4161 | 4 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/257 | no_background | 4161 | 4161 | 3 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/257 | upstream | 2129 | 0 | 2036 | 4037 | 59 | 4035 | 0 | 3.819 |
| 1/4096 | base | 2065 | 2065 | 3 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/4096 | no_background | 2065 | 2065 | 2 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/4096 | upstream | 378 | 0 | 363 | 362 | 1686 | 361 | 0 | 6.373 |
| 2/4096 | base | 4160 | 4160 | 4 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/4096 | no_background | 4160 | 4160 | 3 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/4096 | upstream | 746 | 0 | 715 | 1055 | 3041 | 1053 | 0 | 6.902 |

### Primeiras imagens FAT1: um escritor/4096 bytes

| Versão | Imagem | Bytes já aceitos pela aplicação | Alterações cluster:anterior→novo |
|---|---:|---:|---|
| base | 0 | 0 | 3:0000000>FFFFFFF; 4:0000000>FFFFFFF |
| base | 1 | 512 | 3:FFFFFFF>0000004; 5:0000000>FFFFFFF |
| base | 2 | 1024 | 4:FFFFFFF>0000005; 6:0000000>FFFFFFF |
| base | 3 | 1536 | 5:FFFFFFF>0000006; 7:0000000>FFFFFFF |
| base | 4 | 2048 | 6:FFFFFFF>0000007; 8:0000000>FFFFFFF |
| base | 5 | 2560 | 7:FFFFFFF>0000008; 9:0000000>FFFFFFF |
| upstream | 0 | 3072 | 3:0000000>0000004; 4:0000000>0000005; 5:0000000>0000006; 6:0000000>0000007; 7:0000000>0000008; 8:0000000>0000009; 9:0000000>FFFFFFF |
| upstream | 1 | 4096 | 9:FFFFFFF>000000A; 10:0000000>FFFFFFF |
| upstream | 2 | 7168 | 10:FFFFFFF>000000B; 11:0000000>000000C; 12:0000000>000000D; 13:0000000>000000E; 14:0000000>000000F; 15:0000000>0000010; 16:0000000>0000011; 17:0000000>FFFFFFF |
| upstream | 3 | 7680 | 17:FFFFFFF>0000012; 18:0000000>FFFFFFF |
| upstream | 4 | 11264 | 18:FFFFFFF>0000013; 19:0000000>0000014; 20:0000000>0000015; 21:0000000>0000016; 22:0000000>0000017; 23:0000000>0000018; 24:0000000>0000019; 25:0000000>FFFFFFF |
| upstream | 5 | 11776 | 25:FFFFFFF>000001A; 26:0000000>FFFFFFF |

## interleaved, latência 0

| Escritores/bloco | Versão | FAT1 | FAT2 | Diretório | FREE→EOC FAT1 | FREE→LINK FAT1 | EOC→LINK FAT1 | EOC→FREE FAT1 | Entradas alteradas/FAT1 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1/257 | base | 2065 | 2065 | 3 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/257 | no_background | 2065 | 2065 | 2 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/257 | upstream | 1056 | 0 | 1027 | 1025 | 1023 | 1024 | 0 | 2.909 |
| 2/257 | base | 4160 | 4160 | 3 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/257 | no_background | 4160 | 4160 | 3 | 4097 | 0 | 4094 | 1 | 1.969 |
| 2/257 | upstream | 893 | 0 | 803 | 1573 | 2523 | 1571 | 0 | 6.346 |
| 1/4096 | base | 2065 | 2065 | 3 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/4096 | no_background | 2065 | 2065 | 2 | 2049 | 0 | 2047 | 1 | 1.984 |
| 1/4096 | upstream | 378 | 0 | 363 | 362 | 1686 | 361 | 0 | 6.373 |
| 2/4096 | base | 4128 | 4128 | 3 | 4097 | 0 | 4094 | 1 | 1.984 |
| 2/4096 | no_background | 4128 | 4128 | 3 | 4097 | 0 | 4094 | 1 | 1.984 |
| 2/4096 | upstream | 831 | 0 | 707 | 1156 | 2940 | 1154 | 0 | 6.318 |

### Primeiras imagens FAT1: um escritor/4096 bytes

| Versão | Imagem | Bytes já aceitos pela aplicação | Alterações cluster:anterior→novo |
|---|---:|---:|---|
| base | 0 | 0 | 3:0000000>FFFFFFF; 4:0000000>FFFFFFF |
| base | 1 | 512 | 3:FFFFFFF>0000004; 5:0000000>FFFFFFF |
| base | 2 | 1024 | 4:FFFFFFF>0000005; 6:0000000>FFFFFFF |
| base | 3 | 1536 | 5:FFFFFFF>0000006; 7:0000000>FFFFFFF |
| base | 4 | 2048 | 6:FFFFFFF>0000007; 8:0000000>FFFFFFF |
| base | 5 | 2560 | 7:FFFFFFF>0000008; 9:0000000>FFFFFFF |
| upstream | 0 | 3072 | 3:0000000>0000004; 4:0000000>0000005; 5:0000000>0000006; 6:0000000>0000007; 7:0000000>0000008; 8:0000000>0000009; 9:0000000>FFFFFFF |
| upstream | 1 | 4096 | 9:FFFFFFF>000000A; 10:0000000>FFFFFFF |
| upstream | 2 | 7168 | 10:FFFFFFF>000000B; 11:0000000>000000C; 12:0000000>000000D; 13:0000000>000000E; 14:0000000>000000F; 15:0000000>0000010; 16:0000000>0000011; 17:0000000>FFFFFFF |
| upstream | 3 | 7680 | 17:FFFFFFF>0000012; 18:0000000>FFFFFFF |
| upstream | 4 | 11264 | 18:FFFFFFF>0000013; 19:0000000>0000014; 20:0000000>0000015; 21:0000000>0000016; 22:0000000>0000017; 23:0000000>0000018; 24:0000000>0000019; 25:0000000>FFFFFFF |
| upstream | 5 | 11776 | 25:FFFFFFF>000001A; 26:0000000>FFFFFFF |

## cacheTimer: o que os 64 significam

releaseIdleReservation incrementa cacheTimer por poll que pode avançar, além dos incrementos nos acessos reais ao cache. Em deferDirectoryPage, cacheTimer − accessTimestamp >= 64 desativa o adiamento de background do diretório. Isso torna a página elegível; não obriga uma transferência imediata se o backend está ocupado ou outra página elegível é mais antiga. O limiar não é a política de elegibilidade das FATs, que já podem ser enviadas antes de 64. Flushes explícitos/barreiras não usam essa deferral.

Foram separados quatro controles: base; sem incremento por poll; sem liberação em idle (mantendo o incremento); sem ambos. **Nos 16 casos de escrita, as contagens de FAT1, FAT2, diretório e transições são idênticas entre os quatro.** Portanto esse incremento não explica o excesso destes workloads contínuos. Em workloads com pausas, envelhecer o diretório ou devolver uma reserva não utilizada pode provocar I/O; isso não foi quantificado como workload de idle nesta rodada.

## Controle upstream com retorno limitado a 512 bytes por chamada

A biblioteca upstream é byte-idêntica; somente o wrapper da aplicação limita cada chamada efetiva a 512 bytes, preservando os pedidos externos de 257/4096 e seu pump após cada tentativa. É um controle do progresso permitido entre polls, sem adicionar FAT2 ou barreiras à biblioteca upstream.

| Ritmo | Bloco/escritores | FAT1 upstream normal → limitado | Diretório normal → limitado | FAT1 atual |
|---|---|---:|---:|---:|
| serial | 257/1 | 1056 → 1056 | 1027 → 1027 | 2065 |
| serial | 257/2 | 2129 → 2129 | 2036 → 2036 | 4161 |
| serial | 4096/1 | 378 → 450 | 363 → 419 | 2065 |
| serial | 4096/2 | 746 → 929 | 715 → 835 | 4160 |
| interleaved | 257/1 | 1056 → 1056 | 1027 → 1027 | 2065 |
| interleaved | 257/2 | 893 → 893 | 803 → 803 | 4160 |
| interleaved | 4096/1 | 378 → 450 | 363 → 419 | 2065 |
| interleaved | 4096/2 | 831 → 896 | 707 → 802 | 4128 |

O limite de 512 bytes no wrapper não reproduz as confirmações da atual: um escritor/4096 passa de 378 para apenas 450 gravações FAT1, ainda muito abaixo de 2065. Portanto tamanho de retorno/pedido isolado não explica a diferença; o upstream continua avançando operações e acumulando alterações sem aguardar as mesmas barreiras de ACK.

## Controle da imagem capturada pelo backend

O backend histórico lê o buffer na conclusão. Foi repetido o estudo congelando os 512 bytes na submissão e mantendo o ponteiro original para o callback. **As contagens de FAT1, FAT2 e diretório permaneceram iguais em todos os 48 casos desse controle.** Logo, a diferença de quantidade de metadados não depende desse momento de captura.

Na atual não houve mudança de buffer durante o I/O nesses workloads. No upstream houve mudanças de FAT/diretório em trânsito; algumas categorias de transição diferem entre os dois modelos, e ambos estão no JSON. Os números destacados de transições para um escritor/4096, latência 0, são iguais nos dois modelos. Este controle não altera o código das bibliotecas nem mede CPU.

## Validação

Passaram 48 cenários de escrita e 48 releituras: três versões, dois ritmos, duas latências, um/dois arquivos de 1 MiB e 257/4096 bytes. Nas variantes atuais foram auditados EOC/vínculo/diretório por gravação e igualdade das FATs; upstream recebeu verificação de conteúdo. Todas as contagens de I/O reproduzem POLL_METADATA, incluindo as leituras de controle. Histogramas e soma das categorias de transição conferem para cada imagem/cópia/caso. Bytes retornados por fwrite conferem integralmente com os payloads.

Os controles de cacheTimer acrescentam 64 cenários de escrita/releitura; o limite de retorno upstream acrescenta outros 16; a captura na submissão acrescenta 48. Total: **176 cenários de escrita e 176 releituras**. As contagens dos controles de timer são reconciliadas com as mesmas categorias/histogramas, sem aumento de RAM de produção.

Instrumentação somente nas cópias tests/build/fat-transition. Sem testes de CPU nem injeção de falhas; RAM/pilha/persistência de produção permanecem as anteriores. O fluxo no_background é controle diagnóstico e não deve ser adotado como política de idle.

```powershell
python tests/fat_transition_audit.py --prepare
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition -Variants base,no_background,upstream
python tests/fat_transition_audit.py --prepare-timer
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/timer -Variants base,no_poll_age,no_idle_release,no_age_or_release
python tests/fat_transition_audit.py --prepare-limited
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/upstream-limited -Variants upstream
python tests/fat_transition_audit.py --prepare-snapshot
./tests/run_metadata_write_audit.ps1 -OutputDirectory tests/build/fat-transition/submission-snapshot -Variants base,no_background,upstream
python tests/fat_transition_audit.py
```
