# LFN/FAT32 com o driver nativo do Windows

Esta validação é anterior ao diário FAT32. A implementação e a validação posterior por imagens estão
em [FAT32_LFN_POWERCUT.md](FAT32_LFN_POWERCUT.md); a nova rodada nativa não foi executada.

Validação em 09/11/2026 com VHDs fixos de 512 MiB, MBR, formatados pelo Windows em FAT32. A biblioteca foi compilada com LFN habilitado e exFAT desabilitado, usando o backend original de arquivos de imagem e suas operações assíncronas.

Windows: `Microsoft Windows NT 10.0.26300.0`. Driver `fastfat.sys`: `10.0.26100.9711 (WinBuild.160101.0800)`.

## Resultado nativo

| Formatação | Cluster efetivo | VHD | CHKDSK sem reparo | Resultado |
| --- | ---: | ---: | ---: | --- |
| 512 | 512 bytes | 512 MiB | 4 × código 0 | Passou |
| 4096 | 4096 bytes | 512 MiB | 4 × código 0 | Passou |
| default | 4096 bytes | 512 MiB | 4 × código 0 | Passou |

As 12 verificações ocorreram após a preparação nativa, após as gravações da biblioteca, após as alterações pelo Windows e ao final do retorno pela biblioteca. Não foram usados `/f`, `/r`, `/x` nem reparos para obter aprovação. O CHKDSK concluiu as verificações de arquivos/pastas sem problemas no sistema de arquivos. Seu aviso sobre armazenamento de cópias de sombra também aparece nos registros, com código final 0.

O executor elevado e uma auditoria elevada posterior confirmaram os três VHDs desmontados. Os rodapés SHA-256 permaneceram idênticos, inclusive depois da última gravação nativa. As operações de disco verificam o vínculo com a imagem, tamanho, barramento virtual, partição e rótulo antes de acessar o volume.

## Cobertura

- Windows criou 19 nomes/aliases por imagem, incluindo 14 colisões, nome de 255 caracteres e `lower.txt` com flags NT de caixa. A biblioteca leu os dados por nome longo e pelo alias real obtido com `GetShortPathNameW`, conferindo o nome retornado por fstat.
- Foram aceitos aliases nativos numéricos e derivados de hash, por exemplo `COLLIS~4.BIN` e `CO483D~1.BIN`. Não se presume que o Windows gere o mesmo alias que a biblioteca.
- A biblioteca criou mais 14 nomes em colisão por imagem; o Windows leu o conteúdo tanto pelo nome longo quanto pelo alias. São 99 pares nome/alias registrados nas três imagens.
- Nomes com 13, 26 e 255 caracteres, escrita parcial, append, truncamento para 333 bytes, exclusão por alias, enumeração com padrões, troca apenas de caixa e renomeação de arquivos/diretórios.
- Crescimento de diretórios nativos e da biblioteca além de um cluster, diretório vazio, diretórios aninhados e getcwd após renomeação nativa do pai.
- Windows sobrescreveu/renomeou um arquivo da biblioteca, renomeou o arquivo de nome máximo e um diretório, excluiu outro arquivo e criou um filho. A biblioteca remontou, conferiu os dados e gravou um último arquivo de 701 bytes, relido pelo Windows antes do CHKDSK final.

## Correção encontrada

A primeira rodada parou ao fechar um diretório aberto com `afatfs_open`: `afatfs_apiFileRequest` aceitava apenas arquivos normais, embora open também devolvesse handles de diretório e as operações legadas já os suportassem. A validação agora aceita diretórios em close/sync. Dados e truncate continuam restritos a arquivos normais. A regressão cobre diretórios longos e raízes FAT16/FAT32, incluindo rejeição de truncate.

Após a correção, a rodada inteira foi repetida com imagens novas. Não foi necessário alterar o formato VFAT nem a ordem de campos dos registros de disco. A arquitetura de cache, máquinas de estado, buffers e estruturas persistentes foi preservada.

## Memória e alinhamento

| Alvo | Enums | Estado global antes → depois | Handle | Directory | Request | Acréscimo |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 16 bits | normais | 3544 → 3544 | 340 | 46 | 680 | 0 bytes |
| 16 bits | curtos | 3524 → 3524 | 338 | 46 | 680 | 0 bytes |
| 32 bits | normais | 3644 → 3644 | 356 | 52 | 708 | 0 bytes |
| 64 bits | normais | 3728 → 3728 | 376 | 64 | 760 | 0 bytes |

Todos os tamanhos e offsets medidos permaneceram iguais nas 24 combinações de configuração/ABI. Foram executados testes de bitfields/alinhamento em x86/x64 e compilação C99 real para XC16/PIC24, com enums normais/curtos. Não houve execução em hardware de 16 bits. As regressões das seis configurações e a compilação dos oito executáveis originais passaram. A regressão x64 FAT/LFN também passou com AddressSanitizer, sem violações relatadas.

O escopo continua sendo nomes ASCII na API, setores de 512 bytes e interfaces de 32 bits. O teste usou discos virtuais; não representa teste de cartão físico nem de queda de energia.

## Reprodução e evidências

```powershell
./tests/run_windows_fat32_lfn_tests.ps1
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/windows-fat32-lfn-layout-after
```

As APIs de VHD exigem um executor administrador; o runner compila antes de solicitar elevação, usa uma pasta nova por rodada e mantém a janela do worker oculta. `-NoWait` apenas inicia o trabalho: o resultado deve ser consultado em report.json.

[Registro nativo completo](build/windows-fat32-lfn/run-native-20261109-lfn-2/report.json) · [Auditoria de desmontagem](build/windows-fat32-lfn/run-native-20261109-lfn-2/cleanup-verification.json)

[Especificação do CHKDSK](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) · [Format-Volume](https://learn.microsoft.com/en-us/powershell/module/storage/format-volume)
