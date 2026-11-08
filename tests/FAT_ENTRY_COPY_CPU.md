# Escrita: atualizações localizadas do espelho da FAT

Registro histórico da primeira implementação. A seleção posterior por perfil,
que restaura o caminho padrão e mantém a cópia localizada no multibloco, está em
[MIRROR_PROFILE_FIX.md](MIRROR_PROFILE_FIX.md).

FAT32 8.3, clusters de 512 bytes e **oito setores de cache** em todas as versões. Referência local upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24`. A é o código anterior; B é a alteração mantida.

No pipeline nativo FAT32, uma página espelho já DIRTY conserva a imagem do último par preparado. O helper captura essa condição antes de marcar as páginas sujas e a devolve por um bool local. Depois de alterar a primária, o chamador copia quatro bytes nesse caso. Espelho EMPTY, limpo ou em outro estado recebe a imagem integral atualizada de 512 bytes, sem uma cópia adicional de quatro bytes; não se presume igualdade apenas pela presença no cache. FAT16 e os caminhos opcionais sem pipeline mantêm a cópia integral. Preenchimentos de várias entradas continuam espelhando o setor inteiro.

Não há novo campo, buffer ou reserva. Aquisição das duas páginas, marcação DIRTY, submissões, ACKs e barreiras EOC → vínculo → diretório permanecem. A otimização reduz cópias em RAM, não o tamanho dos setores gravados.

MSVC x64 /O2, 21 repetições pareadas, ordem alternada e afinidade fixa, sem compilações concorrentes durante a medição. Arquivos de 1 MiB por escritor, backend simulado e 32 passagens de leitura para conferir conteúdo. Hora FAT fixada em 12:00:00 apenas nos snapshots COMPARISON_CPU_ONLY. Ciclos incluem biblioteca, backend, polling e preparação do harness; auditoria completa do volume fora da janela de escrita. Não são tempos de PIC24 ou cartão físico.

Redução B/A positiva significa menos CPU, calculada pela mediana das razões pareadas. CPU adicional ao upstream usa razão das medianas. Diferenças pequenas podem refletir ruído do host.

## Por bloco

O harness termina o pedido de um escritor antes do outro.

| Perfil | Bloco | Escritores | A ciclos/B | B ciclos/B | Redução B/A | Upstream ciclos/B | CPU adicional upstream |
|---|---:|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 7.1225 | 7.1065 | -0.50% | 3.9845 | +78.35% |
| Padrão | 257 | 2 | 7.4258 | 7.5577 | -2.11% | 3.8952 | +94.02% |
| Padrão | 4096 | 1 | 6.1306 | 6.1435 | +0.13% | 3.4867 | +76.20% |
| Padrão | 4096 | 2 | 6.3826 | 6.3807 | +0.07% | 3.4539 | +84.74% |
| Leitura multibloco | 257 | 1 | 6.4665 | 6.2813 | +1.92% | 3.9845 | +57.64% |
| Leitura multibloco | 257 | 2 | 6.6572 | 6.5361 | +2.08% | 3.8952 | +67.80% |
| Leitura multibloco | 4096 | 1 | 5.7915 | 5.5723 | +3.21% | 3.4867 | +59.81% |
| Leitura multibloco | 4096 | 2 | 5.8866 | 5.7454 | +2.53% | 3.4539 | +66.35% |
| Leitura multibloco + consulta | 257 | 1 | 6.4555 | 6.2283 | +2.83% | 3.9845 | +56.31% |
| Leitura multibloco + consulta | 257 | 2 | 6.7740 | 6.5825 | +3.37% | 3.8952 | +68.99% |
| Leitura multibloco + consulta | 4096 | 1 | 5.5682 | 5.5041 | +1.84% | 3.4867 | +57.86% |
| Leitura multibloco + consulta | 4096 | 2 | 5.8016 | 5.7066 | +2.40% | 3.4539 | +65.22% |

I/O determinístico entre repetições e **idêntico antes/depois** para cada perfil e escalonamento, incluindo sessões e polling. A tabela compara B padrão com upstream; R/W são setores de 512 bytes, incluindo fechamento e drenagem.

| Bloco | Escritores | R B/upstream | W B/upstream | Metadados W B/upstream | Polls B/upstream |
|---|---:|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 | 10282 / 4165 |
| 257 | 2 | 34 / 34 | 12422 / 8261 | 8326 / 4165 | 20614 / 8327 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 | 6457 / 2853 |
| 4096 | 2 | 34 / 50 | 12420 / 5557 | 8324 / 1461 | 12970 / 5653 |
## Pedidos intercalados

O harness tenta atender ambos os escritores antes de aguardar progresso (COMPARISON_INTERLEAVED_REQUESTS).

| Perfil | Bloco | Escritores | A ciclos/B | B ciclos/B | Redução B/A | Upstream ciclos/B | CPU adicional upstream |
|---|---:|---:|---:|---:|---:|---:|---:|
| Padrão | 257 | 1 | 8.5742 | 8.6322 | -0.45% | 4.0364 | +113.86% |
| Padrão | 257 | 2 | 9.3756 | 9.2868 | +0.69% | 4.1372 | +124.47% |
| Padrão | 4096 | 1 | 23.7055 | 23.6128 | +0.05% | 10.1601 | +132.41% |
| Padrão | 4096 | 2 | 40.8658 | 40.9827 | -0.20% | 18.2743 | +124.26% |

I/O determinístico entre repetições e **idêntico antes/depois** para cada perfil e escalonamento, incluindo sessões e polling. A tabela compara B padrão com upstream; R/W são setores de 512 bytes, incluindo fechamento e drenagem.

| Bloco | Escritores | R B/upstream | W B/upstream | Metadados W B/upstream | Polls B/upstream |
|---|---:|---:|---:|---:|---:|
| 257 | 1 | 18 / 18 | 6181 / 4131 | 4133 / 2083 | 10282 / 4165 |
| 257 | 2 | 34 / 35 | 12420 / 5792 | 8324 / 1696 | 16539 / 5860 |
| 4096 | 1 | 18 / 34 | 6181 / 2789 | 4133 / 741 | 6457 / 2853 |
| 4096 | 2 | 34 / 453 | 12643 / 5634 | 8547 / 1538 | 12937 / 6410 |

## RAM, código e pilha

| ABI/perfil | RAM A → B / upstream | Código A → B / upstream |
|---|---:|---:|
| x64/Padrão | 4872 → 4872 / 4784 B | 36190 → 36261 / 17355 B |
| x64/Leitura multibloco | 4872 → 4872 / 4784 B | 37609 → 37680 / 17355 B |
| x64/Leitura multibloco + consulta | 4872 → 4872 / 4784 B | 37370 → 37441 / 17355 B |
| x86/Padrão | 4760 → 4760 / 4668 B | 26412 → 26506 / 13375 B |

| PIC24 padrão | RAM A → B / upstream | Pilha A → B / upstream |
|---|---:|---:|
| Enums normal | 4614 → 4614 / 4544 B | 270 → 274 / 200 B |
| Enums short | 4586 → 4586 / 4520 B | 264 → 268 / 202 B |

As **48 variantes PIC24** passaram com seções de RAM estática iguais antes/depois. Pico interno conservador: **378 bytes**, estritamente abaixo de 400. XC16 2.10, PIC24FJ256GA702, C99 -Os, enums normais/curtos. Inclui chamadas internas e retornos; exclui backend, callbacks da aplicação e rotinas do compilador/libc.

## Interpretação da CPU

A redução de cópias foi confirmada pelo teste focado. O efeito na CPU deve ser lido por cenário, incluindo resultados negativos quando presentes e o ruído da medição. A otimização não reduziu submissões, polling ou o custo das barreiras.

| Controle de leitura por bloco | Menor / maior redução pareada B/A |
|---|---:|
| Padrão | -10.14% / +1.20% |
| Leitura multibloco | +2.14% / +5.52% |
| Leitura multibloco + consulta | -5.14% / +0.97% |

## Persistência e controles

A validade da atualização localizada é restrita ao pipeline nativo: as páginas espelho não são carregadas por READ nesse caminho; entram pelo helper de aquisição pareada. A primeira imagem é integral, e cada chamador atualiza o espelho antes de devolver controle (por entrada ou por setor no preenchimento). Não há yield entre as alterações do par. Falha de ACK conserva o mesmo buffer e DIRTY. Quando o espelho está limpo ou em voo, o fallback integral continua obrigatório. Estado DIRTY isoladamente não seria prova de igualdade para um futuro chamador que alterasse apenas a primária; esse chamador teria de preservar o contrato de atualização pareada.

Passaram os 11 casos funcionais e os 10 checks de alocação. O teste focado conta cópias: espelho novo exige uma cópia de 512 bytes e nenhuma de quatro; a alteração seguinte do par sujo exige zero cópias de 512 e uma de quatro. Verifica vizinhos, bits altos, reparação integral de espelho limpo antigo, falha de ACK seguida de nova alteração localizada e regravação das duas FATs com conteúdo esperado, além de primária expulsa após ACK enquanto o espelho ainda está sujo. Os checks existentes cobrem ordenação, reserva, pressão, três arquivos e alocações sobrepostas. Auditorias do benchmark confirmaram igualdade das duas FATs atuais. Upstream não mantém a segunda cópia atualizada nesse workload; a comparação não representa contratos de persistência iguais.

A cobertura Doxygen continua em 295 definições. Não foram usados VHDs externos; estes testes geram mídia sintética.

Os resultados de leitura de controle, amostras e contagens completas estão no JSON ao lado. Não atribuir os ciclos agregados exclusivamente ao memcpy. A diferença restante para upstream inclui duas FATs e ordenação mais forte.

## Reprodução e evidências

`tests/build/fat-entry-copy-result` conserva snapshots before/after, normal/lib sem hora fixada, upstream canônico, objetos, trials, scripts derivados para x86 e pedidos intercalados, testes e matriz PIC24. O JSON registra hashes.

```powershell
python tests/fat_entry_copy_cpu_report.py --directory tests/build/fat-entry-copy-result --repetitions 21 --output tests/FAT_ENTRY_COPY_CPU.md --initial-directory tests/build/fat-entry-copy-cpu
```

O primeiro arranjo, não selecionado, repetia os testes de ponteiro/FS antes da cópia localizada e apresentou regressão de leitura de 8,71% em um leitor/4096, repetida em outra rodada (10,78%). fread, cacheSector, poll e fwrite tinham código de objeto idêntico nesse A/B; efeito de posicionamento do código é uma hipótese, sem causa isolada demonstrada. Uma compactação intermediária também não resolveu esse controle (11,05%, evidências em fat-entry-copy-final). A versão final devolve a condição da imagem por um bool local e compartilha o fallback de 512 bytes já existente no chamador. Assim evita inicializar a página e depois alterá-la novamente e evita duplicar a cópia integral no helper. Preenchimento em lote não é compilado no perfil nativo analisado; a tentativa entryUpdate em fat-entry-copy-selected não foi selecionada nem medida em CPU. A regressão desse controle de leitura permaneceu na versão final (10,14%); a alteração não é melhoria global de CPU. Todos os números principais pertencem à nova rodada; não foram combinadas amostras das duas implementações. Evidências iniciais: `tests/build/fat-entry-copy-cpu`, também preservadas no JSON.
