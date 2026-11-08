# FAT32/LFN: quedas de energia e diário compartilhado

As medições deste relatório antecedem as otimizações posteriores de memória e agrupamento de
metadados, registradas em [MEMORY_IO_OPTIMIZATION.md](MEMORY_IO_OPTIMIZATION.md).

O diário FAT32 foi implementado e seu motor foi unificado com o exFAT. A configuração padrão continua com LFN habilitado e exFAT desabilitado.

## Resultado

- **4538 cortes FAT32**, em criação vazia/com dados, renomeação de arquivos vazios/com dados dentro e entre setores e exclusão. Clusters de 512 e 4096 bytes; nomes de até 255 caracteres.
- **2142 novas quedas durante a recuperação**, incluindo gravações parciais; todos os volumes voltaram graváveis, sem erros no grafo FAT/diretórios. Essas imagens foram produzidas pela implementação FAT32 anterior à unificação, exercitando também compatibilidade do formato.
- **28 proteções FAT32**: cabeçalhos/flags, último descritor/imagem publicados, destinos fora do volume ou dentro do log, epoch divergente com CRC válido, serial, cadeia do diário em cada FAT, entrada ausente/atributos inválidos, volume sujo de escritor externo e log ativo em volume limpo. Todos montaram somente leitura, com erro de recuperação e zero gravações.
- **3047 cortes na reserva inicial**: 3027 montagens protegidas, 20 recuperadas/limpas. Não se tenta reparar automaticamente uma reserva interrompida antes de existir um diário confiável.
- **1898 cortes exFAT**, com `fsck.exfat -n` após cada recuperação; zero erros estruturais após recuperar, zero perda dos arquivos testemunha ou de operações já confirmadas. 56 cortes na transferência inicial de propriedade ficaram protegidos sem gravações, conforme o contrato exFAT anterior.
- **44 cortes adicionais + 8 proteções exFAT** usaram transações gravadas pelo código anterior à unificação; compatibilidade e recuperação passaram.

O verificador FAT32 independente compara as duas FATs e percorre cadeias/diretórios: órfãos LFN, clusters perdidos, crosslinks, referências a clusters livres, ciclos e tamanhos inválidos. Cada remontagem ocorre em processo novo e confere os dados de KEEP.BIN, SURVIVE.BIN e arquivos renomeados. Foi verificada a existência de exatamente um nome após renomear e a preservação do resultado em todos os cortes após o callback de confirmação. FATtools 1.1.23 conferiu também nomes e payloads das 16 imagens concluídas.

## Correções e estrutura

A escrita agora atualiza as duas FATs; anteriormente apenas a primeira era alterada. A expansão de diretórios reserva e termina o novo cluster, inicializa-o e só então publica o elo. Reservas de clusters em andamento não são oferecidas a outro arquivo. Uma renomeação com as duas entradas curtas no mesmo setor publica/retira ambas juntas.

As barreiras sozinhas ainda deixavam 75 de 185 cortes com órfãos, FATs divergentes ou renomeação incompleta. O diário autorizado pelo usuário fecha esses intervalos depois de provisionado: before-image → descritor → cabeçalho confirmado → alteração do metadado → checkpoint. Clusters liberados não são realocados antes de confirmar a transação.

`asyncfatfs_journal.inc` contém um único motor de captura, índice em disco, CRC, I/O assíncrono, checkpoint, seleção de cabeçalhos e validação/replay. `asyncfatfs_fatjournal.inc` e `asyncfatfs_exfatjournal.inc` preservam apenas descoberta, alocação e marcadores específicos. Os formatos AFJFT001/AFFUNDO1 e AFJNL002/AFUNDO02 são preservados; não há migração de disco. O cache retém a imagem original até autorizar a modificação, eliminando os buffers dedicados `before` e `header` do exFAT. A reserva do freefile é tratada como reserva, permitindo que o checkpoint termine antes de o fechamento liberar seu bloqueio.

## Memória

Bytes de `afatfs_t`, com quatro setores de cache e três handles públicos, sem freefile. Handles já estão incluídos no total; não os some novamente.

| Configuração / alvo | Antes do diário FAT32 | Motores separados | Motor comum | Economia da unificação |
| --- | ---: | ---: | ---: | ---: |
| default / 16 bits | 3544 | 4140 | 4140 | 0 |
| default / 16 bits / enums curtos | 3524 | 4128 | 4120 | 8 |
| default / 32 bits | 3644 | 4244 | 4244 | 0 |
| default / 64 bits | 3728 | 4328 | 4328 | 0 |
| exfat_only / 16 bits | 9380 | 9380 | 8356 | 1024 |
| exfat_only / 16 bits / enums curtos | 9376 | 9376 | 8344 | 1032 |
| exfat_only / 32 bits | 9692 | 9692 | 8668 | 1024 |
| exfat_only / 64 bits | 9824 | 9824 | 8800 | 1024 |
| lfn_exfat / 16 bits | 9560 | 9560 | 8536 | 1024 |
| lfn_exfat / 16 bits / enums curtos | 9544 | 9544 | 8512 | 1032 |
| lfn_exfat / 32 bits | 9872 | 9872 | 8848 | 1024 |
| lfn_exfat / 64 bits | 10024 | 10024 | 9000 | 1024 |

O FAT32 acrescenta 596 bytes em PIC24/16 bits e 600 bytes em x86/x64 em relação ao LFN sem diário. A unificação não aumenta esse custo. O exFAT economiza 1.024 bytes com enums normais e 1.032 bytes no PIC24 com enums curtos. A posição do epoch junto aos timestamps remove oito bytes de padding do cache nessa ABI. O controlador comum tem 580 bytes no PIC24 e 584 no host, incluindo um único buffer alinhado de 512 bytes; não há heap nem aumento em file/directory/request. A tabela CRC32 `const` ocupa 1 KiB de armazenamento somente leitura, sem inicialização em RAM.

As 24 combinações de layout passaram: seis configurações, x86/x64 e XC16/PIC24 com enums normais/curtos. Bitfields C99, alinhamento natural, buffer de quatro bytes e prefixos de suboperações foram verificados. Os resultados PIC24 são compilação cruzada, sem execução física. A regressão cobre também opt-out do diário, FAT/LFN/freefile e AddressSanitizer FAT/LFN e combinado LFN+exFAT; falhas em leitura, before-image, descritor, publicação, commit e liberação impedem `canPowerOff` até concluir as tentativas e todas as gravações.

## Limites

Os cortes FAT32 simulam cada prefixo de gravações concluídas e o prefixo de 1, 31, 64, 256, 480 ou 511 bytes da gravação seguinte. Na recuperação foram usados 1, 31, 256 e 511 bytes. O backend de teste atrasa confirmações e grava snapshots; não reproduz toda a eletrônica de um cartão real. Não há garantia para corrupção arbitrária, caches voláteis que confirmem antes de persistir, dados ainda em RAM ou sobrescrita de payload no lugar.

FAT16 e FAT32 sem diário mantêm apenas ordenação. A reserva inicial pode precisar de reparo externo após uma queda; jamais se usa um log inválido para adivinhar o estado anterior. Depois de uma queda em sessão da biblioteca, recupere com ela antes de permitir outro escritor. Windows não executa este replay privado.

Não há fsck.fat instalado neste ambiente. FAT32 foi auditado pelo verificador de grafo e FATtools; `fsck.exfat` foi usado somente em exFAT. A nova rodada VHD/CHKDSK foi recusada na solicitação de acesso administrativo e não foi executada. A validação Windows anterior permanece histórica, anterior ao diário FAT32.

## Reprodução

```powershell
./tests/run_fat_lfn_powercut.ps1 -Python python -Torn
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/fat-lfn-powercut/shared-layout
./tests/run_exfat_powercut.ps1 -Python python
```

FATtools 1.1.23 e exfatprogs 1.3.2 são dependências externas de teste descritas em [EXFAT_INTEROP.md](EXFAT_INTEROP.md). JSON resumido com hashes de fontes e evidências fica ao lado deste relatório; os relatórios completos e imagens permanecem em tests/build.
