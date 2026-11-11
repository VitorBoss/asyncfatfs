"""Report measured operation/mount workspace savings against a saved baseline."""
import argparse
import hashlib
import json
import re
from pathlib import Path

from layout_memory_report import load_report
from metadata_optimization_report import benchmark


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def operation_sizes(path):
    report = read_json(path)
    result = {}
    for entry in report["host"]:
        line = next(value for value in entry["output"] if value.startswith("OPERATIONS,"))
        result[(entry["configuration"], entry["bits"], False)] = list(map(int, line.split(",")[1:]))
    for entry in report["xc16"]:
        source = Path(entry["assembly"]).read_text()
        body = source.split("_afatfs_operation_layout_sizes:", 1)[1]
        values = []
        for line in body.splitlines():
            match = re.match(r"\s*\.word\s+(\d+)", line)
            if match:
                values.append(int(match[1]))
            if len(values) == 4:
                break
        assert len(values) == 4
        result[(entry["configuration"], 16, entry["shortEnums"])] = values
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("tests/build/operation-lifetime"))
    parser.add_argument("--output", type=Path, default=Path("tests/OPERATION_LIFETIME_MEMORY.md"))
    args = parser.parse_args()
    previous_path = args.input / "layout-before/report.json"
    current_path = args.input / "layout-after/report.json"
    before = load_report(previous_path)
    after = load_report(current_path)
    operations_before = operation_sizes(previous_path)
    operations_after = operation_sizes(current_path)
    assert before.keys() == after.keys() == operations_before.keys() == operations_after.keys()
    assert len(after) == 24
    rows = []
    for key, previous in sorted(before.items()):
        current = after[key]
        for field in ("afatfsDirectory_t", "afatfsRequest_t", "afatfsJournal_t", "journal_io"):
            assert previous.get(field) == current.get(field), (key, field)
        for field in ("afatfs_t", "afatfsFile_t", "afatfsTruncateFile_t"):
            assert current[field] <= previous[field], (key, field)
        for old, new in zip(operations_before[key], operations_after[key]):
            assert new <= old, key
        rows.append({"configuration": key[0], "bits": key[1], "short_enums": key[2],
                     "global_before": previous["afatfs_t"], "global_after": current["afatfs_t"],
                     "global_saved": previous["afatfs_t"] - current["afatfs_t"],
                     "file_before": previous["afatfsFile_t"], "file_after": current["afatfsFile_t"],
                     "truncate_before": previous["afatfsTruncateFile_t"],
                     "truncate_after": current["afatfsTruncateFile_t"],
                     "operations_before": operations_before[key], "operations_after": operations_after[key]})
    for configuration in ("default", "combined"):
        assert benchmark(args.input / f"benchmark/{configuration}-before.log") == benchmark(
            args.input / f"benchmark/{configuration}-after.log")
    checks = {}
    for name, relative, field in (("powercut", "report.json", "cuts"),
                                   ("recovery", "recovery/report.json", "cuts"),
                                   ("guards", "guards/report.json", "cases"),
                                   ("bootstrap", "provision/report.json", "cuts")):
        report = read_json(args.input / "powercut" / relative)
        assert report["status"] == "passed"
        checks[name] = len(report[field])
    assert (args.input / "regression.log").read_text(errors="replace").count(
        "global idle and clean power-off status: passed") == 6
    assert (args.input / "asan.log").read_text(errors="replace").count(
        "global idle and clean power-off status: passed") == 1
    result = {"status": "passed", "memory": rows, "checks": checks,
              "operation_metrics": ["create", "append", "extend", "finder"],
              "source_sha256": {}, "baseline_sha256": {}, "evidence_sha256": {}}
    for directory, field in ((Path("lib"), "source_sha256"), (args.input / "before-lib", "baseline_sha256")):
        for path in sorted(directory.iterdir()):
            if path.suffix in (".c", ".h", ".inc"):
                result[field][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    evidence = [previous_path, current_path, args.input / "regression.log", args.input / "asan.log"]
    evidence += list((args.input / "powercut").rglob("report.json"))
    evidence += list((args.input / "benchmark").glob("*.log"))
    for path in evidence:
        result["evidence_sha256"][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    lines = ["# Memória compartilhada nas operações e na montagem", "",
             "Comparação com a versão imediatamente anterior, já contendo as uniões do diário "
             "descritas em [JOURNAL_LIFETIME_MEMORY.md](JOURNAL_LIFETIME_MEMORY.md). "
             "Os números abaixo são economias adicionais, em bytes.", "",
             "## Tempos de vida", "",
             "| Área | Campos compartilhados | Transição segura |",
             "| --- | --- | --- |",
             "| Criação | Alias/reserva FAT e contadores exFAT | O tipo do volume fixa qual membro é utilizado. |",
             "| Criação LFN | Parser e contadores de reserva/publicação | Inicializa os contadores após terminar a busca do nome/alias. |",
             "| Truncamento | Contador de clusters, próximo cluster e início do trecho contíguo | A busca termina antes da liberação; liberação normal e devolução ao freefile usam caminhos distintos. |",
             "| Expansão | Conversão da cadeia e setor de zeragem | Inicializa o setor zero após concluir a conversão, antes de publicar o novo cluster. |",
             "| Montagem exFAT | Boot, busca no diretório e decodificação up-case | Reinicializa o workspace a cada transição; metadados da tabela permanecem independentes. |",
             "| Inicialização | Montagem exFAT e busca/preparo do freefile FAT | Inicialização de freefile é pulada no exFAT; montagem exFAT não ocorre em FAT. |", "",
             "Os valores necessários para retomar I/O pendente continuam válidos entre chamadas. "
             "O índice de órfão LFN permanece separado dos contadores de publicação. Nome longo e "
             "alias curto coexistem; entradas e posições exFAT precisam sobreviver às operações; "
             "cursores e callbacks também não foram sobrepostos. O formato no volume, APIs e seleção "
             "padrão FAT/LFN foram preservados.", "",
             "## Memória global e por arquivo", "",
             "O global inclui cache, diário e handles internos. Não some economias de componentes "
             "ao total global. Os tamanhos de Request, Directory e diário não mudaram.", "",
             "| Configuração | ABI | Global antes → depois | Economia | Arquivo antes → depois |",
             "| --- | --- | ---: | ---: | ---: |"]
    for row in rows:
        label = f"{row['bits']} bits" + (", enums curtos" if row["short_enums"] else "")
        lines.append(f"| {row['configuration']} | {label} | {row['global_before']} → {row['global_after']} | "
                     f"{row['global_saved']} | {row['file_before']} → {row['file_after']} |")
    lines += ["", "## Workspaces da configuração padrão", "",
              "| ABI | Criação | Append | Extend | Truncamento |", "| --- | ---: | ---: | ---: | ---: |"]
    for row in rows:
        if row["configuration"] != "default":
            continue
        label = f"{row['bits']} bits" + (", enums curtos" if row["short_enums"] else "")
        pairs = [f"{old} → {new}" for old, new in zip(row["operations_before"][:3], row["operations_after"][:3])]
        lines.append(f"| {label} | " + " | ".join(pairs) +
                     f" | {row['truncate_before']} → {row['truncate_after']} |")
    lines += ["", "A montagem exFAT usa 32 bytes de scratch, compartilhados com a inicialização "
              "do freefile quando habilitado. A tabela ASCII/GPT já compartilhada permanece "
              "independente: a tabela precisa coexistir com o decodificador durante a montagem. "
              "O ganho global depende do maior membro de cada union e do padding da ABI.", "",
              "## Validação", "",
              "- 24 layouts: x86/x64 executados e PIC24/XC16 C99 com enums normais/curtos compilados. "
              "Alinhamento natural, buffers alinhados, flags e prefixos de suboperações preservados.",
              "- Regressão em seis configurações, mais compilação dos oito testes legados. APIs "
              "de criação/rename/truncamento/expansão, diretórios, GPT/up-case, falhas de I/O, "
              "remoção/reinserção, diário e desligamento pendente passaram.",
              "- AddressSanitizer em LFN+exFAT+freefile sem erros de memória.",
              f"- FAT32: {checks['powercut']} cortes nas operações, {checks['recovery']} cortes no "
              f"replay, {checks['guards']} casos de proteção e {checks['bootstrap']} cortes no provisionamento.",
              "- Benchmark padrão/combinado: contagens de leituras/gravações idênticas às anteriores; "
              "conteúdo validado após remontar. Esta mudança reduz RAM, sem ganho de I/O medido.", "",
              "Os testes usam backend simulado e imagens regulares; PIC24 foi compilado sem execução "
              "no hardware. Nenhum novo VHD ou cartão físico foi usado nesta etapa.", "",
              "## Reprodução", "", "```powershell",
              "# Guarde lib/ em tests/build/operation-lifetime/before-lib antes de alterar.",
              "# Rode o probe no checkout anterior para gerar layout-before/report.json.",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/operation-lifetime/layout-after",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/operation-lifetime/powercut -Torn",
              "./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/operation-lifetime/before-lib -OutputDirectory tests/build/operation-lifetime/benchmark",
              "python tests/operation_lifetime_report.py", "```", "",
              "O JSON ao lado guarda os 24 casos, componentes e hashes das fontes/baseline/evidências. "
              "Os tamanhos medidos dependem da configuração e ABI do compilador.", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
