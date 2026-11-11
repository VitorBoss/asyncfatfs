"""Verify cache batching measurements and publish a reproducible report."""
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report
from metadata_optimization_report import benchmark

ROOT = Path("tests/build/io-batching")
BASELINE = Path("tests/build/operation-lifetime")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def multi(path):
    rows = {}
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("MULTI,"):
            values = line.split(",")
            rows[values[1]] = dict(zip(("reads", "writes", "sessions", "grouped_blocks", "single_blocks"),
                                       map(int, values[2:])))
    return rows


def main():
    previous = load_report(BASELINE / "layout-after/report.json")
    current = load_report(ROOT / "layout/report.json")
    assert previous == current and len(current) == 24
    saved_sources = read_json(Path("tests/OPERATION_LIFETIME_MEMORY.json"))["source_sha256"]
    for name, digest in saved_sources.items():
        assert hashlib.sha256((ROOT / "before-lib" / name).read_bytes()).hexdigest() == digest
    for configuration in ("default", "combined"):
        assert benchmark(ROOT / f"benchmark/{configuration}-before.log") == benchmark(
            ROOT / f"benchmark/{configuration}-after.log")
    before_multi = multi(ROOT / "multi-combined-before/test.log")
    after_multi = multi(ROOT / "multi-combined-after/test.log")
    assert before_multi.keys() == after_multi.keys() and len(after_multi) == 2
    for configuration in ("default",):
        for variant, expected in (("before", before_multi), ("after", after_multi)):
            assert multi(ROOT / f"multi-{configuration}-{variant}/test.log")["fat32"] == expected["fat32"]
    for filesystem, after in after_multi.items():
        before = before_multi[filesystem]
        assert before["reads"] == after["reads"] and before["writes"] == after["writes"]
        assert after["sessions"] + after["single_blocks"] < before["sessions"] + before["single_blocks"]
        assert after["grouped_blocks"] + after["single_blocks"] == after["writes"]
    old_matrix = read_json(BASELINE / "powercut/report.json")
    matrix = read_json(ROOT / "powercut/report.json")
    recovery = read_json(ROOT / "powercut/recovery/report.json")
    guards = read_json(ROOT / "powercut/guards/report.json")
    provision = read_json(ROOT / "powercut/provision/report.json")
    assert all(report["status"] == "passed" for report in (old_matrix, matrix, recovery, guards, provision))
    operations = []
    for before, after in zip(old_matrix["cases"], matrix["cases"]):
        assert (before["cluster"], before["scenario"]) == (after["cluster"], after["scenario"])
        assert after["writes"] <= before["writes"] and not after["unclean"]
        operations.append({"cluster": before["cluster"], "scenario": before["scenario"],
                           "writes_before": before["writes"], "writes_after": after["writes"]})
    assert (ROOT / "regression.log").read_text(errors="replace").count("global idle and clean power-off status: passed") == 6
    assert "global idle and clean power-off status: passed" in (ROOT / "asan.log").read_text(errors="replace")
    for configuration in ("default", "no_multiblock"):
        assert "cache batching:" in (ROOT / f"cache-{configuration}/test.log").read_text(errors="replace")
    result = {"status": "passed", "unchanged_layouts": len(current), "operations": operations,
              "multi_before": before_multi, "multi_after": after_multi,
              "cuts": len(matrix["cuts"]), "recovery_cuts": len(recovery["cuts"]),
              "guards": len(guards["cases"]), "bootstrap_cuts": len(provision["cuts"]),
              "source_sha256": {}, "baseline_sha256": {}, "evidence_sha256": {}}
    for directory, field in ((Path("lib"), "source_sha256"), (ROOT / "before-lib", "baseline_sha256")):
        for path in sorted(directory.iterdir()):
            if path.suffix in (".c", ".h", ".inc"):
                result[field][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    evidence = list(ROOT.rglob("report.json")) + list(ROOT.glob("*.log"))
    evidence += list(ROOT.glob("multi-*/test.log")) + list(ROOT.glob("cache-*/test.log"))
    for path in evidence:
        result["evidence_sha256"][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    lines = ["# Agrupamento de I/O do cache", "",
             "Baseline: versão das uniões por tempo de vida registrada em OPERATION_LIFETIME_MEMORY.json. "
             "A seleção padrão continua FAT/LFN; o formato no volume e as APIs foram preservados.", "",
             "## Política", "",
             "O poll adia o escoamento oportunista nas fases de produção de entradas LFN/exFAT, "
             "alocação FAT32 protegida pelo diário, conversão de cadeia e liberação de clusters. "
             "Barreiras explícitas e falta de cache continuam drenando setores. Quando blockAllocation "
             "aguarda checkpoint, o escoamento é permitido, evitando espera circular.", "",
             "A seleção de setores mantém a ordem de writeTimestamp. Se os próximos setores sujos "
             "e desbloqueados nessa ordem forem também consecutivos fisicamente, usa "
             "AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT. Não há reordenação para formar um lote. "
             "Leituras ou endereços diferentes podem interromper a sequência conforme o contrato do backend.", "",
             "Metadados não anunciam setores futuros fora do cache: FATFillWithPattern podia "
             "repassar essa indicação antes de capturar suas imagens undo. Um backend que pré-apaga "
             "a faixa poderia destruir metadados ainda não protegidos. A indicação futura do freefile "
             "continua disponível para append de dados; novos lotes são limitados aos buffers prontos.", "",
             "O retorno de beginWriteBlocks agora é verificado: BUSY/FAILURE preservam o setor sujo "
             "e não submetem o bloco; FAILURE registra BACKEND_IO. Sync, close, flush, publicação e "
             "shutdown mantêm suas barreiras. Falhas deixam canPowerOff falso enquanto houver dados pendentes.", "",
             "## Setores gravados por operação FAT32/LFN", "",
             "As contagens incluem o diário. O mesmo workload e backend foram usados antes/depois.", "",
             "| Cluster (bytes) | Operação | Antes | Depois |", "| ---: | --- | ---: | ---: |"]
    for row in operations:
        lines.append(f"| {row['cluster']} | {row['scenario']} | {row['writes_before']} | {row['writes_after']} |")
    lines += ["", "## Gravação de 64 KiB pré-alocados", "",
              "| Sistema | Leituras antes/depois | Setores gravados antes/depois | Inícios de sequência antes → depois | Blocos agrupados depois |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for filesystem, after in after_multi.items():
        before = before_multi[filesystem]
        lines.append(f"| {filesystem} | {before['reads']} / {after['reads']} | {before['writes']} / {after['writes']} | "
                     f"{before['sessions'] + before['single_blocks']} → {after['sessions'] + after['single_blocks']} | {after['grouped_blocks']} |")
    lines += ["", "Inícios são sequências múltiplas mais escritas isoladas no backend de teste. "
              "As chamadas writeBlock e o número de setores de payload continuam necessários. "
              "127 dos 128 setores de dados foram agrupados; os demais totais incluem metadados/diário. "
              "O teste valida o conteúdo após remontar. Não há medição de tempo físico de cartão SD.", "",
              "O benchmark anterior de streaming (um/dois escritores, FAT32/exFAT) preservou "
              "exatamente suas leituras, gravações e I/O de montagem. Adiar toda a alocação, inclusive "
              "a busca por clusters, foi descartado porque aumentava leituras.", "",
              "## Limites e validação", "",
              "- FATs espelhadas geralmente não são adjacentes na ordem de gravação; não são reordenadas para montar lotes.",
              "- Bitmap, FAT, publicação de diretórios, dados/VDL e commit mantêm as dependências de persistência existentes.",
              "- Imagem, registro e cabeçalhos do diário mantêm a ordem e o buffer único; não foram agrupados especulativamente.",
              "- Nenhum campo, buffer ou alocação dinâmica foi adicionado à biblioteca. Os 24 layouts "
              "de 16/32/64 bits permaneceram idênticos, incluindo alinhamento PIC24 com enums normais/curtos.",
              "- Seis configurações de regressão, oito testes legados compilados e AddressSanitizer LFN+exFAT+freefile passaram.",
              "- Backend com pré-apagamento destrutivo: faixas consecutivas, ordem temporal não consecutiva, "
              "setores bloqueados, interrupção por leitura, begin BUSY/FAILURE e write submit/ACK inválidos; "
              "buffers pendentes e canPowerOff verificados. Também executado com múltiplos blocos desabilitados.",
              f"- FAT32: {result['cuts']} cortes de operação, {result['recovery_cuts']} cortes no replay, "
              f"{result['guards']} proteções e {result['bootstrap_cuts']} cortes de provisionamento passaram.", "",
              "Os cortes usam imagens regulares e backend de rastreamento; o teste destrutivo de "
              "pré-apagamento é separado. PIC24 foi compilado sem execução física. Não foi usado novo VHD.", "",
              "## Reprodução", "", "```powershell",
              "./tests/run_io_batching_tests.ps1 -BaselineDirectory tests/build/io-batching/before-lib",
              "./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/io-batching/before-lib -OutputDirectory tests/build/io-batching/benchmark",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/io-batching/layout",
              "./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/io-batching/powercut -Torn",
              "python tests/io_batching_report.py", "```", "",
              "Guarde lib/ em before-lib antes da mudança. A baseline de layouts/cortes é "
              "tests/build/operation-lifetime. O JSON ao lado registra fontes e evidências por hash.", ""]
    Path("tests/IO_BATCHING.md").write_text("\n".join(lines), encoding="utf-8")
    Path("tests/IO_BATCHING.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
