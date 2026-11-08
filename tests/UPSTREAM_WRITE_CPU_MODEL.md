# Modelo empírico de CPU de escrita do upstream

Este documento é apenas a referência numérica agregada. O modelo de etapas executadas no código, solicitado pelo usuário, está em [UPSTREAM_WRITE_EXECUTION_MODEL.md](UPSTREAM_WRITE_EXECUTION_MODEL.md).

Referência upstream/master: `60c5f3580152410cb08342942cbbe3a30a449c24`. FAT32 8.3, cache de oito setores, clusters de 512 bytes, arquivos de 1 MiB, um/dois escritores intercalados por bloco, escrita multibloco ligada. MSVC x64 /O2, backend em RAM, perfil de baixa latência do harness. As observações incluem abertura, fechamento, polling e preparação dos dados; não são ciclos isolados da biblioteca nem ciclos de PIC24/cartão físico.

## Referência medida e modelo

Para cada cenário s: **C_ref = bytes_agregados × k_s**. k_s é a mediana dos ciclos/byte medidos, não uma constante universal. No tamanho calibrado, C_ref é a referência medida da operação inteira. Para outros tamanhos a fórmula é apenas uma extrapolação linear não validada; ela dilui abertura/fechamento e não representa um custo fixo separado.

P10–P90 é a faixa empírica das amostras, não intervalo de confiança da mediana nem garantia para uma execução futura.

| Bloco | Escritores | Amostras | k, ciclos/byte | P10–P90 | C_ref, milhões de ciclos |
|---|---:|---:|---:|---:|---:|
| 257 | 1 | 21 | 3.8639 | 3.8393–4.6231 | 4.052 |
| 257 | 2 | 21 | 3.8219 | 3.7925–3.9623 | 8.015 |
| 4096 | 1 | 21 | 3.4506 | 3.4285–3.4815 | 3.618 |
| 4096 | 2 | 21 | 3.3717 | 3.3487–3.4061 | 7.071 |

## Volume de trabalho de referência

| Bloco | Escritores | R | W | Metadados W | Polls | Ciclos totais / poll |
|---|---:|---:|---:|---:|---:|---:|
| 257 | 1 | 18 | 4131 | 2083 | 4165 | 972.8 |
| 257 | 2 | 34 | 8261 | 4165 | 8327 | 962.5 |
| 4096 | 1 | 34 | 2789 | 741 | 2853 | 1268.2 |
| 4096 | 2 | 50 | 5557 | 1461 | 5653 | 1250.8 |

R/W são setores de 512 bytes. Ciclos totais/poll (assim como ciclos/W no JSON) é uma razão descritiva do workload completo: **não é o custo marginal de executar um poll ou escrever um setor**. Não somar essas razões como parcelas de CPU.

## Validação em rodada independente

Mesmo commit e cenário, outra rodada de execução. Erro = (predição / medição independente − 1) × 100. Isso testa a estabilidade entre rodadas; não valida outros tamanhos, latências ou hardware.

| Bloco | Escritores | Observado, ciclos/byte | Erro do modelo |
|---|---:|---:|---:|
| 257 | 1 | 4.0331 | -4.20% |
| 257 | 2 | 3.8741 | -1.35% |
| 4096 | 1 | 3.5384 | -2.48% |
| 4096 | 2 | 3.4270 | -1.61% |

## Uso em otimizações futuras

1. Repetir upstream e candidato na mesma rodada, com cache, volume, tamanhos, escalonamento, compilador e backend iguais; alternar a ordem e guardar amostras.
2. Calcular CPU adicional = (ciclos/byte do candidato / k_upstream da rodada − 1) × 100. Usar esta referência histórica como orientação, não como aprovação automática.
3. Separar alterações de CPU de alterações de trabalho: registrar R/W, metadados, polls, conteúdo remontado e igualdade das FATs. Menos ciclos com mais I/O não demonstra melhoria universal.

As quatro observações de cenário não identificam custos independentes por byte, chamada fwrite, poll, setor e metadado. Não foi ajustada uma regressão com esses coeficientes: seria subdeterminada/confundida. Para decomposição causal, é necessário variar independentemente tamanhos de arquivo, latência, fragmentação e frequência de sync, e medir chamadas/tempo exclusivo dos caminhos em uma execução de instrumentação separada.

A auditoria histórica detectou diferenças entre as duas FATs no upstream. Esta referência não é uma meta obtida com as mesmas garantias de persistência da versão atual; não remover espelhamento ou barreiras para atingir seus números.

## Reprodução

```powershell
python tests/upstream_write_cpu_model.py --training tests/CACHE8_WRITE_COMPARISON.json --validation tests/build/cache8-group-cpu/report.json --output tests/UPSTREAM_WRITE_CPU_MODEL.md
```

O JSON ao lado preserva coeficientes, amostras, contadores, razões, validação e hashes dos dados de entrada.
