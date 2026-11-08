"""Build an empirical upstream write CPU reference, without a causal cost fit."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics


def upstream(path):
    data = json.loads(path.read_text())
    return (data.get('cpu') or data['results'])['upstream_native']


def percentile(values, fraction):
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--training', type=Path, required=True)
    parser.add_argument('--validation', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    training = upstream(args.training)
    validation = upstream(args.validation) if args.validation else {}
    model = dict(upstream_commit='60c5f3580152410cb08342942cbbe3a30a449c24',
                 scope=dict(cache_sectors=8, cluster_bytes=512, file_bytes=1048576,
                            writers=[1, 2], chunks=[257, 4096], abi='MSVC x64 /O2'),
                 sources={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (args.training, args.validation) if path}, cases={})
    lines = ['# Modelo empírico de CPU de escrita do upstream', '',
             'Referência upstream/master: `'+model['upstream_commit']+'`. FAT32 8.3, '
             'cache de oito setores, clusters de 512 bytes, arquivos de 1 MiB, '
             'um/dois escritores intercalados por bloco, escrita multibloco ligada. '
             'MSVC x64 /O2, backend em RAM, perfil de baixa latência do harness. '
             'As observações incluem abertura, fechamento, polling e preparação dos dados; '
             'não são ciclos isolados da biblioteca nem ciclos de PIC24/cartão físico.', '',
             '## Referência medida e modelo', '',
             'Para cada cenário s: **C_ref = bytes_agregados × k_s**. '
             'k_s é a mediana dos ciclos/byte medidos, não uma constante universal. '
             'No tamanho calibrado, C_ref é a referência medida da operação inteira. '
             'Para outros tamanhos a fórmula é apenas uma extrapolação linear não validada; '
             'ela dilui abertura/fechamento e não representa um custo fixo separado.', '',
             'P10–P90 é a faixa empírica das amostras, não intervalo de confiança da mediana '
             'nem garantia para uma execução futura.', '',
             '| Bloco | Escritores | Amostras | k, ciclos/byte | P10–P90 | C_ref, milhões de ciclos |',
             '|---|---:|---:|---:|---:|---:|']
    operational = []
    checks = []
    for key, row in training.items():
        if not key.startswith('write/'):
            continue
        _, profile, clusters, chunk, writers = key.split('/')
        assert profile == '0' and clusters == '1'
        assert row['payload_bytes'] == int(writers) * 1048576
        samples = row['samples']
        coefficient = statistics.median(samples)
        assert abs(coefficient - row['cycles_per_byte']) < 1e-10
        cycles = coefficient * row['payload_bytes']
        record = dict(bytes=row['payload_bytes'], cycles_per_byte=coefficient,
                      p10=percentile(samples, .1), p90=percentile(samples, .9),
                      reference_cycles=cycles, samples=samples,
                      counters={name: row[name] for name in
                                ('reads', 'writes', 'metadata_reads', 'metadata_writes', 'polls')},
                      ratios={name: cycles / row[name] for name in ('writes', 'metadata_writes', 'polls')})
        model['cases'][key] = record
        lines.append(f'| {chunk} | {writers} | {len(samples)} | {coefficient:.4f} | '
                     f"{record['p10']:.4f}–{record['p90']:.4f} | {cycles/1e6:.3f} |")
        operational.append(f"| {chunk} | {writers} | {row['reads']} | {row['writes']} | "
                           f"{row['metadata_writes']} | {row['polls']} | {cycles/row['polls']:.1f} |")
        if key in validation:
            observed = validation[key]['cycles_per_byte']
            error = (coefficient / observed - 1) * 100
            record['validation'] = dict(observed_cycles_per_byte=observed,
                                        prediction_error_percent=error,
                                        samples=validation[key]['samples'])
            checks.append(f'| {chunk} | {writers} | {observed:.4f} | {error:+.2f}% |')
    assert len(model['cases']) == 4
    lines += ['', '## Volume de trabalho de referência', '',
              '| Bloco | Escritores | R | W | Metadados W | Polls | Ciclos totais / poll |',
              '|---|---:|---:|---:|---:|---:|---:|', *operational, '',
              'R/W são setores de 512 bytes. Ciclos totais/poll (assim como ciclos/W no JSON) '
              'é uma razão descritiva do workload completo: **não é o custo marginal de '
              'executar um poll ou escrever um setor**. Não somar essas razões como parcelas de CPU.', '']
    if checks:
        lines += ['## Validação em rodada independente', '',
                  'Mesmo commit e cenário, outra rodada de execução. Erro = '
                  '(predição / medição independente − 1) × 100. Isso testa a estabilidade '
                  'entre rodadas; não valida outros tamanhos, latências ou hardware.', '',
                  '| Bloco | Escritores | Observado, ciclos/byte | Erro do modelo |',
                  '|---|---:|---:|---:|', *checks, '']
    lines += ['## Uso em otimizações futuras', '',
              '1. Repetir upstream e candidato na mesma rodada, com cache, volume, '
              'tamanhos, escalonamento, compilador e backend iguais; alternar a ordem e guardar amostras.',
              '2. Calcular CPU adicional = (ciclos/byte do candidato / k_upstream da rodada − 1) × 100. '
              'Usar esta referência histórica como orientação, não como aprovação automática.',
              '3. Separar alterações de CPU de alterações de trabalho: registrar R/W, '
              'metadados, polls, conteúdo remontado e igualdade das FATs. Menos ciclos '
              'com mais I/O não demonstra melhoria universal.', '',
              'As quatro observações de cenário não identificam custos independentes '
              'por byte, chamada fwrite, poll, setor e metadado. Não foi ajustada uma regressão '
              'com esses coeficientes: seria subdeterminada/confundida. Para decomposição causal, '
              'é necessário variar independentemente tamanhos de arquivo, latência, '
              'fragmentação e frequência de sync, e medir chamadas/tempo exclusivo dos '
              'caminhos em uma execução de instrumentação separada.', '',
              'A auditoria histórica detectou diferenças entre as duas FATs no upstream. '
              'Esta referência não é uma meta obtida com as mesmas garantias de persistência '
              'da versão atual; não remover espelhamento ou barreiras para atingir seus números.', '',
              '## Reprodução', '', '```powershell',
              f'python tests/upstream_write_cpu_model.py --training {args.training.as_posix()} '
              + (f'--validation {args.validation.as_posix()} ' if args.validation else '')
              + f'--output {args.output.as_posix()}', '```', '',
              'O JSON ao lado preserva coeficientes, amostras, contadores, razões, '
              'validação e hashes dos dados de entrada.', '']
    args.output.write_text('\n'.join(lines), encoding='utf-8')
    args.output.with_suffix('.json').write_text(json.dumps(model, indent=2) + '\n')
    print('Four scenario references built; independent validation:', bool(checks))


if __name__ == '__main__':
    main()
