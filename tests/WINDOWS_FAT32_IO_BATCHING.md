# Agrupamento de I/O e LFN simultâneo: driver nativo Windows/FAT32

Rodada iniciada em `2026-11-11T01:15:55.2979912Z`. Windows `Microsoft Windows NT 10.0.26300.0`; driver `fastfat.sys` `10.0.26100.9711 (WinBuild.160101.0800)`. Três VHDs fixos novos de 512 MiB, MBR, formatados pelo Windows. Configuração original: FAT/LFN, exFAT desabilitado, quatro páginas de cache e três handles públicos.

## Resultado

| Formatação | Cluster efetivo | CHKDSK sem reparo | Sequências múltiplas | Blocos em sequência |
| --- | ---: | --- | ---: | ---: |
| 512 | 512 bytes | 4 × código 0 | 43 | 128 |
| 4096 | 4096 bytes | 4 × código 0 | 73 | 197 |
| default | 4096 bytes | 4 × código 0 | 73 | 197 |

As 12 execuções de CHKDSK ocorreram antes/depois da biblioteca, após alterações nativas e no fim do retorno pela biblioteca. Não houve `/f`, `/r`, `/x` ou reparos para fazer a imagem passar. Os conteúdos foram lidos pelo Windows e pela biblioteca.

A auditoria posterior montou os VHDs somente para leitura, verificou quatro arquivos concorrentes por seus nomes longos e aliases nativos distintos, conferiu os bytes e confirmou as três imagens desmontadas e os rodapés SHA-256 intactos.

## Simultaneidade e cobertura

- Três criações LFN ficam pendentes juntas, exercitando serialização do percurso do diretório e colisões de aliases.
- Renomeação de um LFN com outro escritor ativo. A operação usa dois handles temporários; o teste respeita o limite original de três, sem aumentar AFATFS_MAX_OPEN_FILES.
- Depois, dois escritores são intercalados com um leitor e chamadas de prefetch. Um arquivo usa pré-alocação de 64 KiB; outro cresce por append até 33.101 bytes. Os três fechamentos ficam pendentes juntos e canPowerOff permanece falso durante as esperas.
- Windows cria dois arquivos em tarefas paralelas sincronizadas por barreira, a biblioteca lê os dados, e o Windows sobrescreve paralelamente dois arquivos da biblioteca. A biblioteca remonta, confere as alterações e grava um último arquivo, relido pelo Windows.
- Mantida a cobertura de nomes de 13/26/255 caracteres, aliases Windows numéricos/hash, enumeração, truncamento, rename de caixa, exclusão e diretórios aninhados.

As operações da biblioteca são assíncronas e intercaladas na mesma thread; isso não adiciona suporte a chamadas multithread. Windows e biblioteca alternam a propriedade do volume: acesso bruto ocorre somente com o VHD desmontado. O backend de teste implementa pré-apagamento destrutivo e confirmações atrasadas. Contadores são opcionais e pertencem exclusivamente a tests/sdcard_sim, sem campos na biblioteca.

## Correção encontrada

A primeira rodada nativa encontrou espera infinita ao criar o nome de 255 caracteres no volume com cluster de 4 KiB. Três páginas sujas deixavam uma página reutilizável; o percurso FAT e a entrada LFN expulsavam um ao outro. As leituras conseguiam alocar, então a salvaguarda de falta de cache não disparava e o poll continuava adiando o flush.

A condição de adiamento agora permite escoamento quando a busca/reserva ou escrita de entradas LFN precisa de duas páginas e há menos de duas reutilizáveis. Ela também contempla criação de freefile/log. Não foram adicionados campos ou buffers. Um teste específico preserva a condição de três páginas sujas com uma criação publicando.

A salvaguarda foi limitada a essas fases: aplicá-la a toda alocação aumentava as escritas de dois escritores. Na versão final, as contagens do benchmark de streaming FAT32/exFAT com um/dois escritores e da montagem permaneceram iguais às da baseline. A gravação pré-alocada preservou as sequências 137 → 43 em FAT32 e 136 → 51 em exFAT. O tempo físico de SD não é medido pelo VHD.

A rodada completa foi repetida em três imagens novas após o ajuste final; nenhuma imagem da rodada interrompida foi reparada e reaproveitada para obter aprovação.

## Memória e regressões

| Alvo padrão | Estado global antes → depois | Handle | Acréscimo |
| --- | ---: | ---: | ---: |
| 16 bits | 4112 → 4112 | 338 | 0 bytes |
| 16 bits, enums curtos | 4092 → 4092 | 336 | 0 bytes |
| 32 bits | 4204 → 4204 | 352 | 0 bytes |
| 64 bits | 4272 → 4272 | 368 | 0 bytes |

24 layouts idênticos: x86/x64 executados e XC16/PIC24 C99 compilado com enums normais/curtos. Alinhamento, flags, setores e prefixos de suboperações preservados. Seis configurações passaram na regressão, oito testes legados compilaram e AddressSanitizer LFN+exFAT+freefile não encontrou erros. O backend destrutivo passou com begin BUSY/FAILURE, falhas de envio/ACK, locks, interrupção e recurso desabilitado.

FAT32: 3957 cortes nas operações, 2142 no replay, 28 proteções e 86 cortes de provisionamento passaram. Essas falhas usam backend simulado; não são cortes de energia no driver nativo. O escopo continua ASCII, setores de 512 bytes e endereços de 32 bits.

## Reprodução

```powershell
./tests/run_windows_fat32_lfn_tests.ps1
# Após concluir, audite o mesmo RunName como administrador:
./tests/audit_windows_fat32_lfn.ps1 -RunName run-...
python tests/windows_fat32_batching_report.py
```

Os executores administrativos validam vínculo entre arquivo VHD, disco virtual, partição, rótulo e diretório exclusivo da rodada. A biblioteca permanece sem elevação em produção; elevação é necessária para montar e consultar os VHDs de teste.

[Evidência nativa](build/windows-fat32-lfn/run-io-batching-native-final-20261110/report.json) · [Auditoria](build/windows-fat32-lfn/run-io-batching-native-final-20261110/cleanup-verification.json). O JSON ao lado inclui hashes das fontes, binário e evidências.
