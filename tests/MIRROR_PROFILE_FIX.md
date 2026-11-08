# Correção das regressões por seleção do espelhamento FAT

Registro histórico da seleção por perfil. A otimização posterior do escalonador
está em [WRITE_DISPATCH_CPU.md](WRITE_DISPATCH_CPU.md); ela mantém o código multibloco
e modifica somente o caminho padrão de continuação dos escritores.

Cache mantido em **oito setores**, FAT32 8.3, clusters de 512 bytes. A configuração padrão recupera o caminho anterior de aquisição e cópia integral da FAT. A cópia localizada de quatro bytes permanece nas builds com AFATFS_MIN_MULTIPLE_BLOCK_READ_COUNT, com e sem consulta do backend. Não há uma nova opção pública nem seleção dinâmica por arquivo.

O parâmetro e o bool local usados para classificar o espelho não são compilados no padrão. AFATFS_FAT_MIRROR_STATE mantém chamadas e assinatura internas coerentes com o perfil. Nenhuma reserva, página, campo estático, barreira, ACK ou regra de publicação foi acrescentada ou removida.

**Recebida** é a versão com as regressões de 10,14% na leitura 1/4096 e 2,11% na escrita 2/257; **original** é o snapshot imediatamente anterior à introdução da cópia localizada; **final** aplica a seleção por perfil. Os percentuais históricos não são misturados às novas amostras.

MSVC x64 /O2, 21 repetições pareadas, ordem alternada, afinidade fixa, arquivos de 1 MiB por escritor e 32 passagens de leitura. Sem compilações concorrentes na medição. Hora FAT fixada em 12:00:00 apenas nos snapshots COMPARISON_CPU_ONLY. Ciclos incluem biblioteca, backend simulado, polling e harness; não são medições em PIC24 ou cartão físico. Redução positiva significa menos CPU; valores próximos de zero são ruído possível.

## Identidade do código gerado

Todas as seções de instruções dos objetos asyncfatfs, fat_standard e harness foram comparadas integralmente, incluindo posicionamento dentro das seções. No x64: padrão final = original; multibloco final = recebida; multibloco com consulta final = recebida. No x86, padrão final = original. Isso evita depender apenas de uma rodada favorável de CPU para afirmar restauração do padrão ou preservação do caminho multibloco.

Igualdade das seções de instruções nos executáveis ligados: default/reference_default: sim, after_no_query/before: sim, after/before_query: sim.

## Cenários que motivaram a correção

| Cenário padrão | Recebida ciclos/B | Final ciclos/B | Original ciclos/B | Redução vs recebida | Redução vs original |
|---|---:|---:|---:|---:|---:|
| read/0/1/4096/1 | 3.4144 | 3.1264 | 3.1024 | +9.06% | +0.02% |
| write/0/1/257/2 | 7.6002 | 7.5334 | 7.4975 | +1.25% | -0.24% |

## Escrita e preservação multibloco

O código multibloco é idêntico ao recebido. Variações entre essas duas cópias são controles da medição, não ganhos novos. A comparação com a original usa o mesmo perfil para verificar os ganhos já obtidos.

| Perfil | Bloco | Escritores | Final ciclos/B | Redução vs recebida | Redução vs original | CPU adicional upstream |
|---|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 7.1366 | -0.26% | -0.36% | +82.09% |
| Padrão | 257 | 2 | 7.5334 | +1.25% | -0.24% | +93.65% |
| Padrão | 4096 | 1 | 6.1576 | -0.41% | +0.30% | +76.91% |
| Padrão | 4096 | 2 | 6.4181 | -0.05% | -0.06% | +88.28% |
| Multibloco | 257 | 1 | 6.2501 | +0.14% | -0.37% | +59.47% |
| Multibloco | 257 | 2 | 6.5574 | +0.18% | -0.42% | +68.56% |
| Multibloco | 4096 | 1 | 5.5229 | +1.28% | +0.80% | +58.67% |
| Multibloco | 4096 | 2 | 5.7457 | +0.75% | -0.36% | +68.55% |
| Multibloco + consulta | 257 | 1 | 6.2992 | -0.59% | +10.29% | +60.72% |
| Multibloco + consulta | 257 | 2 | 6.6029 | +0.08% | +9.95% | +69.73% |
| Multibloco + consulta | 4096 | 1 | 5.4721 | -0.01% | +10.28% | +57.21% |
| Multibloco + consulta | 4096 | 2 | 5.7399 | -0.29% | +9.83% | +68.38% |

## I/O

Leituras, gravações, metadados, polling, sessões e tempo virtual são idênticos entre recebida, final e original, em todos os casos e perfis. Contagens em setores de 512 bytes, incluindo fechamento e drenagem.

| Bloco | Escritores | R final/upstream | W final/upstream | Metadados W final/upstream |
|---|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 |
| 257 | 2 | 34 / 34 | 12422 / 8261 | 8326 / 4165 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 |
| 4096 | 2 | 34 / 50 | 12420 / 5557 | 8324 / 1461 |

Referência local upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24`, também com oito setores. Ele não mantém a segunda FAT atualizada nesse workload; a diferença restante inclui espelhamento e ordenação adicionais da biblioteca atual.

## RAM, código e pilha

| ABI/perfil | RAM recebida → final | Código recebido → final |
|---|---:|---:|
| x64/Padrão | 4872 → 4872 B | 36261 → 36190 B |
| x64/Multibloco | 4872 → 4872 B | 37680 → 37680 B |
| x64/Multibloco + consulta | 4872 → 4872 B | 37441 → 37441 B |
| x86/padrão | 4760 → 4760 B | 26506 → 26412 B |

| PIC24/padrão | RAM recebida → final | Pilha recebida → final |
|---|---:|---:|
| Enums normal | 4614 → 4614 B | 274 → 270 B |
| Enums short | 4586 → 4586 B | 268 → 264 B |

As 48 variantes PIC24 passaram com seções estáticas iguais e pilha interna máxima de **378 bytes**, estritamente abaixo de 400. XC16 2.10, PIC24FJ256GA702, C99 -Os; inclui chamadas internas e retornos, exclui backend, callbacks da aplicação e rotinas do compilador/libc.

## Testes e evidências

Passaram os 11 casos funcionais e 12 checks de alocação. O teste de espelho agora executa explicitamente os três perfis: cópia integral no padrão e localizada nos dois multiblocos. Verifica vizinhos, bits altos, página nova, espelho limpo antigo, falha de ACK/retry e expulsão da primária com espelho ainda sujo. Os outros checks preservam ordenação, reserva única, pressão e escritores pendentes. Doxygen: 295 definições; fontes continuam no monólito. Mídia sintética, sem dependência dos VHDs perdidos.

`tests/build/mirror-profile-fix` contém before/lib (recebida), reference/lib (original), normal/lib (final sem hora fixada), after/lib (benchmark), upstream, objetos, executáveis, trials, testes, docs e matrix/report.json. O JSON ao lado conserva amostras, hashes, comparações de código e controles de leitura de todos os cenários.

```powershell
python tests/mirror_profile_fix_report.py --directory tests/build/mirror-profile-fix --repetitions 21 --output tests/MIRROR_PROFILE_FIX.md
```
