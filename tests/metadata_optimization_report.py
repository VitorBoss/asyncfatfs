"""Publish measured RAM/I/O changes with the accompanying recovery evidence."""
import argparse
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report


def document(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def benchmark(path):
    result = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("BENCH,"):
            continue
        fields = line.split(",")
        result[(fields[1], int(fields[2]))] = dict(zip(
            ("bytes", "reads", "writes", "mount_reads", "mount_writes"), map(int, fields[3:])))
    assert result, path
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("tests/build/optimization"))
    parser.add_argument("--output", type=Path, default=Path("tests/MEMORY_IO_OPTIMIZATION.md"))
    args = parser.parse_args()
    before = load_report(Path("tests/build/optimization-before/report.json"))
    after = load_report(Path("tests/build/optimization-after/report.json"))
    assert before.keys() == after.keys() and len(after) == 24
    rows = []
    for key, previous in sorted(before.items()):
        current = after[key]
        assert current["afatfs_t"] <= previous["afatfs_t"]
        for field in ("afatfsFile_t", "afatfsRequest_t", "afatfsDirectory_t", "afatfsTruncateFile_t"):
            assert current[field] == previous[field], (key, field)
        if "afatfsJournal_t" in current:
            assert previous["afatfsJournal_t"] - current["afatfsJournal_t"] == 12
        rows.append({"configuration": key[0], "bits": key[1], "short_enums": key[2],
                     "before": previous["afatfs_t"], "after": current["afatfs_t"],
                     "saved": previous["afatfs_t"] - current["afatfs_t"]})
    initial = benchmark(args.input / "benchmark/combined-before.log")
    final = benchmark(args.input / "benchmark/combined-after.log")
    assert initial.keys() == final.keys() and len(initial) == 4
    for side, values in (("before", initial), ("after", final)):
        default = benchmark(args.input / f"benchmark/default-{side}.log")
        assert all(counts == values[key] for key, counts in default.items())
    transfers = []
    for key, previous in sorted(initial.items()):
        current = final[key]
        assert current["bytes"] == previous["bytes"]
        assert current["mount_writes"] < previous["mount_writes"]
        assert current["reads"] + current["writes"] <= previous["reads"] + previous["writes"]
        transfers.append({"filesystem": key[0], "files": key[1], "before": previous, "after": current})
    matrix = document(args.input / "fat32/report.json")
    recovery = document(args.input / "fat32/recovery/report.json")
    guards = document(args.input / "fat32/guards/report.json")
    provision = document(args.input / "fat32/provision/report.json")
    assert all(report["status"] == "passed" for report in (matrix, recovery, guards, provision))
    assert all(not entry.get("semantic_issues") and not entry["after"]["issues"] for entry in matrix["cuts"])
    exfat = document(args.input / "exfat/report.json")["summary"]
    for field in ("keep_failures", "close_failures", "sync_failures", "premature_flush", "recovery_failures",
                  "recovery_fsck_errors", "recovery_target_prefix_lost", "unmarked_errors"):
        assert exfat[field] == 0, field
    result = {"status": "passed", "baseline_revision": "fc6a2939fcea90efd53f905c3381a68fc82d93ae",
              "memory": rows, "io": transfers,
              "fat32": {"cuts": len(matrix["cuts"]), "recovery_cuts": len(recovery["cuts"]),
                        "guards": len(guards["cases"]), "bootstrap_cuts": len(provision["cuts"]),
                        "bootstrap_read_only": sum(bool(entry["protected"]) for entry in provision["cuts"])},
              "exfat": exfat, "source_sha256": {}, "evidence_sha256": {}}
    for path in sorted(Path("lib").glob("*")):
        if path.suffix in (".c", ".h", ".inc"):
            result["source_sha256"][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    for path in (args.input / "fat32/report.json", args.input / "fat32/recovery/report.json",
                 args.input / "fat32/guards/report.json", args.input / "fat32/provision/report.json",
                 args.input / "exfat/report.json", Path("tests/build/optimization-before/report.json"),
                 Path("tests/build/optimization-after/report.json"),
                 args.input / "benchmark/default-before.log", args.input / "benchmark/default-after.log",
                 args.input / "benchmark/combined-before.log", args.input / "benchmark/combined-after.log",
                 Path("tests/metadata_batch.c"), Path("tests/test_exfat.c"), Path("tests/run_metadata_benchmark.ps1")):
        result["evidence_sha256"][path.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    lines = ["# Otimizações de memória e I/O de metadados", "",
             "Comparação com a revisão fc6a2939, já contendo o diário FAT32/exFAT compartilhado. "
             "A configuração padrão continua FAT/LFN, sem exFAT. Valores em bytes e setores de 512 bytes.", "",
             "## Memória estática", "",
             "| Configuração | ABI | Antes | Depois | Economia |", "| --- | --- | ---: | ---: | ---: |"]
    for row in rows:
        label = f"{row['bits']} bits" + (", enums curtos" if row["short_enums"] else "")
        lines.append(f"| {row['configuration']} | {label} | {row['before']} | {row['after']} | {row['saved']} |")
    lines += ["", "`afatfs_t` inclui cache e handles internos: não some componentes novamente. "
              "As estruturas de arquivo, diretório, request e suboperação de truncamento não cresceram. "
              "O controlador do diário diminuiu 12 bytes: candidato da reserva, setor pendente de captura "
              "e destino de replay compartilham uma união; comprimento da reserva e índice de busca "
              "compartilham outra. Essas fases nunca se sobrepõem.", "",
              "O exFAT passou a usar temporariamente `openFiles[0]` durante a montagem e o devolve antes "
              "de READY, inclusive em saídas protegidas e GPT somente leitura. Isso elimina um handle "
              "permanente inteiro. O limite público de arquivos permanece igual; as proteções do nome e "
              "da alocação do diário não dependem de um endereço fixo de handle. O serial exFAT também "
              "deixou de existir em duplicata e usa o campo já presente no diário.", "",
              "Foram verificadas seis configurações em x86/x64 e PIC24/XC16 com enums normais/curtos "
              "(24 casos), incluindo bitfields C99, alinhamento natural das uniões, buffers alinhados "
              "a quatro bytes e prefixos de suboperações. Os resultados de 16 bits são compilação "
              "cruzada real, sem execução em hardware. Não há heap, buffers de setor adicionais ou "
              "alteração dos registros em disco.", "", "## I/O medido", "",
              "Benchmark de 64 KiB por arquivo, um ou dois escritores alternados em blocos de 257 bytes, "
              "clusters de 512 bytes, quatro setores de cache, três handles públicos e backend com "
              "confirmação atrasada. Inclui sincronização/fechamento e desmontagem. O payload é conferido "
              "depois de remontar. São contagens de transferências, não medições de MB/s de cartão físico. "
              "O benchmark padrão e o combinado LFN+exFAT produziram os mesmos números FAT32.", "",
              "| Sistema | Arquivos | Leituras antes → depois | Gravações antes → depois | I/O total antes → depois |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for row in transfers:
        previous = row["before"]
        current = row["after"]
        lines.append(f"| {row['filesystem']} | {row['files']} | {previous['reads']} → {current['reads']} | "
                     f"{previous['writes']} → {current['writes']} | "
                     f"{previous['reads'] + previous['writes']} → {current['reads'] + current['writes']} |")
    lines += ["", "| Primeira montagem / reserva do diário | Leituras antes → depois | Gravações antes → depois |",
              "| --- | ---: | ---: |"]
    for row in transfers:
        if row["files"] != 1:
            continue
        previous = row["before"]
        current = row["after"]
        lines.append(f"| {row['filesystem']} | {previous['mount_reads']} → {current['mount_reads']} | "
                     f"{previous['mount_writes']} → {current['mount_writes']} |")
    lines += ["", "Em FAT32 com diário válido, a alocação de arquivos normais pode atualizar o EOC "
              "novo e o elo anterior antes de forçar uma descarga intermediária. A descarga automática "
              "é adiada apenas nessas fases; pressão do cache força uma gravação para evitar impasse "
              "com FAT fragmentada, leitores retendo buffers ou outros escritores. Publicação de "
              "diretório, checkpoint, fsync, close e desligamento mantêm a espera necessária. Diretórios "
              "e volumes sem diário conservam suas barreiras.", "",
              "Na reserva inicial, a FAT e o bitmap permanecem no cache enquanto as atualizações "
              "contíguas cabem nele. A barreira final continua aguardando todas as gravações antes de "
              "publicar a extensão do diário. Os before-images e descritores ainda precisam estar "
              "confirmados antes de alterar metadados protegidos.", "",
              "O agrupamento amplo na alocação normal exFAT foi experimentado e descartado: aumentava "
              "leituras e I/O total no escritor único. Seu caminho normal foi preservado; o ganho exFAT "
              "de I/O ficou na reserva inicial. No FAT32 de escritor único também há mais leituras, "
              "mas a redução das gravações diminui o total. Os ganhos dependem do cache e do workload.", "",
              "## Integridade e limites", "",
              f"- FAT32: {len(matrix['cuts'])} cortes nas operações, {len(recovery['cuts'])} cortes durante "
              f"o replay, {len(guards['cases'])} casos de diário inválido e {len(provision['cuts'])} cortes "
              "na reserva inicial. Gravações completas e parciais, criação, renomeação e exclusão. "
              "Zero falhas nos resultados esperados; diários não confiáveis ficam protegidos.",
              f"- exFAT: {exfat['cuts']} cortes com fsck.exfat 1.3.2 após remontar; zero erros após "
              "recuperação, zero perda dos arquivos testemunha e de operações confirmadas. "
              f"{exfat['protected_startup_cuts']} transferências iniciais de propriedade permaneceram "
              "somente leitura, sem gravações, conforme o contrato anterior.",
              "- Regressão em seis configurações e compilação dos oito testes originais. "
              "AddressSanitizer em LFN+exFAT com freefile, incluindo concorrência, falhas no backend, "
              "remoção/reinserção e bloqueio de canPowerOff com operações pendentes.", "",
              "O teste de exclusividade durante close passou a aceitar que localizar uma entrada de "
              "diretório fora do cache pode aguardar I/O. Seu callback verifica que um novo escritor "
              "só é aceito depois do callback de fechamento anterior; a checagem de durabilidade "
              "continua descartando a RAM imediatamente após a confirmação do close.", "",
              "A rodada externa complementar de novas quedas durante o replay exFAT não foi executada: "
              "a solicitação de acesso ao verificador foi recusada. Os testes locais de replay e a "
              "matriz exFAT acima foram executados. Não foram usados cartões físicos ou novos VHDs. "
              "A primeira reserva ainda precisa terminar antes de existir proteção pelo diário; "
              "payload sobrescrito no lugar, dados em RAM e caches que confirmem antes de persistir "
              "continuam fora da garantia.", "", "## Reprodução", "", "```powershell",
              "./tests/run_metadata_benchmark.ps1 -BaselineDirectory tests/build/optimization/before-lib",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/optimization-after",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_fat_lfn_powercut.ps1 -Python python -Torn",
              "./tests/run_exfat_powercut.ps1 -Python python", "```", "",
              "Para medir apenas o código atual, omita BaselineDirectory. A pasta de baseline usada "
              "nesta medição contém os arquivos lib da revisão indicada no início. O JSON ao lado "
              "contém tabelas completas e hashes das fontes e das evidências.", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
