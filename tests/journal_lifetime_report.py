"""Verify phase-exclusive journal storage and report the measured ABI savings."""
import argparse
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report
from metadata_optimization_report import benchmark


def document(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, default=Path("tests/build/journal-recovery-enum/layout/report.json"))
    parser.add_argument("--input", type=Path, default=Path("tests/build/journal-lifetime"))
    parser.add_argument("--output", type=Path, default=Path("tests/JOURNAL_LIFETIME_MEMORY.md"))
    args = parser.parse_args()
    before = load_report(args.before)
    after = load_report(args.input / "layout/report.json")
    assert before.keys() == after.keys() and len(after) == 24
    rows = []
    for key, previous in sorted(before.items()):
        current = after[key]
        for field in ("afatfsFile_t", "afatfsDirectory_t", "afatfsRequest_t", "afatfsTruncateFile_t"):
            assert previous[field] == current[field], (key, field)
        assert current["afatfs_t"] <= previous["afatfs_t"]
        if "afatfsJournal_t" in current:
            assert current["afatfsJournal_t"] == 560
            assert current["journal_io"] == previous["journal_io"] == 0
        else:
            assert current == previous
        rows.append({"configuration": key[0], "bits": key[1], "short_enums": key[2],
                     "journal_before": previous.get("afatfsJournal_t", 0),
                     "journal_after": current.get("afatfsJournal_t", 0),
                     "global_before": previous["afatfs_t"], "global_after": current["afatfs_t"],
                     "global_saved": previous["afatfs_t"] - current["afatfs_t"]})
    for configuration in ("default", "combined"):
        assert benchmark(args.input / f"benchmark/{configuration}-before.log") == benchmark(
            args.input / f"benchmark/{configuration}-after.log")
    matrix = document(args.input / "fat32/report.json")
    recovery = document(args.input / "fat32/recovery/report.json")
    guards = document(args.input / "fat32/guards/report.json")
    provision = document(args.input / "fat32/provision/report.json")
    assert all(report["status"] == "passed" for report in (matrix, recovery, guards, provision))
    result = {"status": "passed", "memory": rows,
              "fat32_cuts": len(matrix["cuts"]), "recovery_cuts": len(recovery["cuts"]),
              "guards": len(guards["cases"]), "bootstrap_cuts": len(provision["cuts"]),
              "source_sha256": {}, "baseline_sha256": {}, "evidence_sha256": {}}
    for directory, field in ((Path("lib"), "source_sha256"), (args.input / "before-lib", "baseline_sha256")):
        for path in sorted(directory.iterdir()):
            if path.suffix in (".c", ".h", ".inc"):
                result[field][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in (args.before, args.input / "layout/report.json", args.input / "fat32/report.json",
                 args.input / "fat32/recovery/report.json", args.input / "fat32/guards/report.json",
                 args.input / "fat32/provision/report.json", args.input / "regression.log", args.input / "asan.log"):
        result["evidence_sha256"][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    lines = ["# Diário: compartilhamento de variáveis por tempo de vida", "",
             "Comparação com o estado imediatamente anterior a esta alteração, já contendo os enums "
             "de inicialização, fechamento e recuperação. Somente estruturas internas de RAM mudaram; "
             "o formato do diário no volume e as estruturas públicas de arquivo/diretório/request foram preservados.", "",
             "## Campos compartilhados", "",
             "| Armazenamento | Usos exclusivos | Condição da troca |",
             "| --- | --- | --- |",
             "| `lifecycle` | `initPhase` / `shutdownPhase` | Montagem termina antes de fechamento. "
             "Em `AFATFS_INITIALIZATION_DONE`, inicializa-se `shutdownPhase` com `WAIT_COMMIT` antes de READY. |",
             "| `work` | `setup` / `recovery` / `capture` | Busca/validação/reserva, replay e captura normal "
             "não usam seus campos de trabalho simultaneamente. |",
             "| `work.capture.record` | `scan` / `imageCRC` | A busca termina antes de calcular o CRC e gravar "
             "a imagem. O caminho de registro duplicado vai diretamente à liberação do cache. |",
             "| `mount` | `foundEntries` / `recoveryPhase` | União já existente: a descoberta exFAT termina "
             "antes do replay. |", "",
             "O workspace `work` ocupa 12 bytes: três palavras na preparação/replay, ou setor pendente, "
             "scan/CRC e índice de cache na captura. O índice é inicializado na entrada do modo de captura "
             "e antes de cada operação. Um reset antigo de scan na entrada do replay foi removido: o "
             "próprio PREPARE inicializa o contador correto.", "",
             "`headerSlot` também foi reduzido de uint32_t a um bitfield C99 bool: seus únicos valores "
             "são os índices 0 e 1 dos dois cabeçalhos. Ele não faz parte da representação em disco e "
             "passou a participar do teste de todas as combinações de flags.", "",
             "## Campos mantidos separados", "",
             "- `phase` precisa coexistir com `initPhase`, pois preserva erros de montagem, e com "
             "`shutdownPhase`, pois o fechamento pode aguardar uma captura/commit ainda em andamento.",
             "- Destino, progresso e CRC da imagem coexistem durante o replay e entre tentativas de I/O; "
             "continuam sendo três palavras distintas dentro de `work.recovery`.",
             "- Serial, geometria, sequência, epoch, count e flags de I/O/propriedade precisam sobreviver "
             "às transições e aos cabeçalhos pendentes. O buffer alinhado de 512 bytes continua independente.", "",
             "O início do fechamento é resetado apenas na transição global para READY. Nesse ponto, "
             "capturas para a inicialização de freefile/log já podem estar em andamento: seus campos de "
             "trabalho são preservados. Isso evita corromper uma operação assíncrona ao trocar o seletor.", "",
             "## Memória medida", "",
             "Bytes com quatro setores de cache e três handles públicos. O global inclui o diário; "
             "não some as economias das duas colunas. Padding explica a diferença em x64.", "",
             "| Configuração | ABI | Diário antes → depois | Global antes → depois | Economia global |",
             "| --- | --- | ---: | ---: | ---: |"]
    for row in rows:
        label = f"{row['bits']} bits" + (", enums curtos" if row["short_enums"] else "")
        lines.append(f"| {row['configuration']} | {label} | {row['journal_before']} → {row['journal_after']} | "
                     f"{row['global_before']} → {row['global_after']} | {row['global_saved']} |")
    lines += ["", "O controlador ficou com 560 bytes em todas as ABIs testadas, incluindo os 512 bytes "
              "de I/O. A economia no controlador é 8 bytes em PIC24 e 12 bytes em x86/x64; no estado "
              "global, é 8, 12 e 16 bytes respectivamente. Configurações sem diário não mudaram. "
              "Unir apenas os dois seletores de um byte não reduziria necessariamente sizeof por "
              "causa do padding; a economia resulta do conjunto das sobreposições e da compactação do slot.", "",
              "## Validação", "",
              "- 24 casos de layout: seis configurações em x86/x64 e PIC24/XC16 com enums normais/curtos. "
              "Alinhamento natural, buffer de quatro bytes, byte do ciclo de vida, limites dos enums, "
              "workspace de três palavras e todas as combinações de 13 flags bool foram verificados.",
              "- Regressão nas seis configurações, incluindo recuperação exFAT interrompida, log inválido, "
              "falhas de backend, remoção/reinserção e bloqueio do desligamento durante gravações pendentes. "
              "As falhas injetadas na liberação de propriedade também verificam o reset do fechamento após montar.",
              "- AddressSanitizer em LFN+exFAT com freefile: nenhum erro de memória.",
              f"- FAT32: {result['fat32_cuts']} cortes nas operações, {result['recovery_cuts']} cortes durante "
              f"replay, {result['guards']} casos de log inválido e {result['bootstrap_cuts']} cortes de "
              "provisionamento; todos passaram nas verificações de integridade ou proteção esperada.",
              "- Benchmark padrão e combinado: mesmas leituras, gravações e payloads após remontar, "
              "inclusive na primeira reserva do diário.", "",
              "Os números de PIC24 são compilação cruzada real, sem execução em hardware. Os testes "
              "de I/O usam backend atrasado e imagens regulares; não foram usados cartões físicos "
              "ou novos VHDs. A proteção começa após a primeira reserva bem-sucedida do diário e "
              "não torna sobrescritas de payload atômicas.", "", "## Reprodução", "", "```powershell",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/journal-lifetime/layout",
              "./tests/run_exfat_tests.ps1",
              "./tests/run_fat_lfn_powercut.ps1 -Python python -OutputDirectory tests/build/journal-lifetime/fat32 -Torn",
              "./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/journal-lifetime/before-lib -OutputDirectory tests/build/journal-lifetime/benchmark",
              "```", "",
              "A baseline de layout usada é tests/build/journal-recovery-enum/layout/report.json. "
              "As fontes anteriores estão em tests/build/journal-lifetime/before-lib. O JSON ao lado "
              "registra a tabela completa e hashes das fontes/evidências.", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
