"""Verify saved evidence and publish FAT32 power-cut/shared-journal results."""
import argparse
import hashlib
import json
from pathlib import Path

from fat_lfn_powercut import CREATED, DESTINATION, MAXIMUM, SOURCE, semantic_issues
from exfat_interop import close_volume, pattern, volume
from layout_memory_report import load_report
from FATtools.FAT import Chain


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("tests/build/fat-lfn-powercut"))
    parser.add_argument("--output", type=Path, default=Path("tests/FAT32_LFN_POWERCUT.md"))
    args = parser.parse_args()
    folder = args.input
    matrix = read_json(folder / "shared-torn/report.json")
    recovery = read_json(folder / "shared-recovery/report.json")
    guards = read_json(folder / "shared-guards/report.json")
    provision = read_json(folder / "shared-provision/report.json")
    exfat = read_json(folder / "shared-exfat/report.json")
    legacy = read_json(folder / "shared-exfat-legacy-recovery/report.json")
    assert all(report["status"] == "passed" for report in (matrix, recovery, guards, provision))
    assert len(matrix["cases"]) == 16
    for entry in matrix["cuts"]:
        completed = read_json(folder / "shared-torn" / f"{entry['cluster']}-{entry['scenario']}" / "completed.json")
        assert not semantic_issues(entry, completed), entry
        assert not entry["after"]["issues"] and entry["after"]["keep"] and entry["probe"]["code"] == 0
    for case in matrix["cases"]:
        root = volume(folder / "shared-torn" / f"{case['cluster']}-{case['scenario']}" / "final.img")
        try:
            checks = {"KEEP.BIN": pattern(4096, 9), "SURVIVE.BIN": pattern(1701, 67)}
            if case["scenario"].startswith("create"):
                checks[CREATED] = pattern(1703, 73) if case["scenario"] == "create-data" else b""
            elif case["scenario"].startswith("rename"):
                checks[DESTINATION if "same" in case["scenario"] else MAXIMUM] = (
                    b"" if case["scenario"].endswith("empty") else pattern(1703, 31))
                assert root.find(SOURCE) is None
            else:
                assert root.find(SOURCE) is None
            for name, expected in checks.items():
                entry = root.find(name)
                assert entry is not None, name
                assert entry.dwFileSize == len(expected), name
                actual = bytes(Chain(root.boot, root.fat, entry.Start(), entry.dwFileSize).read()) if expected else b""
                assert actual == expected, name
        finally:
            close_volume(root)
    assert all(entry["passed"] for entry in guards["cases"] + provision["cuts"] + legacy["cuts"] + legacy["guards"])
    assert all(not entry["failed"] for entry in recovery["cuts"])
    for field in ("keep_failures", "close_failures", "sync_failures", "premature_flush", "recovery_failures",
                  "recovery_fsck_errors", "recovery_target_prefix_lost", "unmarked_errors"):
        assert exfat["summary"][field] == 0, field
    before = load_report(Path("tests/build/windows-fat32-lfn-layout-after/report.json"))
    split = load_report(folder / "layout-after/report.json")
    after = load_report(folder / "shared-layout/report.json")
    assert before.keys() == split.keys() == after.keys() and len(after) == 24
    memory = []
    for key, current in sorted(after.items()):
        for member in ("afatfsFile_t", "afatfsRequest_t", "afatfsDirectory_t"):
            assert current[member] == before[key][member] == split[key][member]
        if key[0] in ("default", "fat_lfn", "fat_only"):
            assert current["afatfs_t"] <= split[key]["afatfs_t"]
        else:
            assert split[key]["afatfs_t"] - current["afatfs_t"] in (1024, 1032)
        memory.append({"configuration": key[0], "bits": key[1], "short_enums": key[2],
                       "before_journal": before[key]["afatfs_t"], "separate_engines": split[key]["afatfs_t"],
                       "shared_engine": current["afatfs_t"], "unification_saved": split[key]["afatfs_t"] - current["afatfs_t"],
                       "handle": current["afatfsFile_t"], "journal": current.get("afatfsJournal_t", 0)})
    report = {"status": "passed", "fat32_cuts": len(matrix["cuts"]), "fat32_recovery_cuts": len(recovery["cuts"]),
              "fat32_guards": len(guards["cases"]), "bootstrap_cuts": len(provision["cuts"]),
              "bootstrap_protected": sum(bool(entry["protected"]) for entry in provision["cuts"]),
              "exfat": exfat["summary"], "exfat_legacy_recovery": len(legacy["cuts"]),
              "exfat_legacy_guards": len(legacy["guards"]), "memory": memory,
              "cases": matrix["cases"], "source_sha256": {}, "evidence_sha256": {}}
    for path in sorted(Path("lib").glob("*")):
        if path.suffix in (".c", ".h", ".inc"):
            report["source_sha256"][str(path).replace("\\", "/")] = hashlib.sha256(path.read_bytes()).hexdigest()
    for name in ("shared-torn", "shared-recovery", "shared-guards", "shared-provision", "shared-exfat", "shared-exfat-legacy-recovery", "shared-layout"):
        path = folder / name / "report.json"
        report["evidence_sha256"][str(path).replace("\\", "/")] = hashlib.sha256(path.read_bytes()).hexdigest()
    lines = ["# FAT32/LFN: quedas de energia e diário compartilhado", "",
             "O diário FAT32 foi implementado e seu motor foi unificado com o exFAT. "
             "A configuração padrão continua com LFN habilitado e exFAT desabilitado.", "",
             "## Resultado", "",
             f"- **{report['fat32_cuts']} cortes FAT32**, em criação vazia/com dados, renomeação de arquivos "
             "vazios/com dados dentro e entre setores e exclusão. Clusters de 512 e 4096 bytes; nomes de até 255 caracteres.",
             f"- **{report['fat32_recovery_cuts']} novas quedas durante a recuperação**, incluindo gravações parciais; "
             "todos os volumes voltaram graváveis, sem erros no grafo FAT/diretórios. Essas imagens foram produzidas "
             "pela implementação FAT32 anterior à unificação, exercitando também compatibilidade do formato.",
             f"- **{report['fat32_guards']} proteções FAT32**: cabeçalhos/flags, último descritor/imagem publicados, "
             "destinos fora do volume ou dentro do log, epoch divergente com CRC válido, serial, "
             "cadeia do diário em cada FAT, entrada ausente/atributos inválidos, volume sujo de escritor externo "
             "e log ativo em volume limpo. "
             "Todos montaram somente leitura, com erro de recuperação e zero gravações.",
             f"- **{report['bootstrap_cuts']} cortes na reserva inicial**: "
             f"{report['bootstrap_protected']} montagens protegidas, "
             f"{report['bootstrap_cuts'] - report['bootstrap_protected']} recuperadas/limpas. "
             "Não se tenta reparar automaticamente uma reserva interrompida antes de existir um diário confiável.",
             f"- **{exfat['summary']['cuts']} cortes exFAT**, com `fsck.exfat -n` após cada recuperação; "
             "zero erros estruturais após recuperar, zero perda dos arquivos testemunha ou de operações já confirmadas. "
             f"{exfat['summary']['protected_startup_cuts']} cortes na transferência inicial de propriedade "
             "ficaram protegidos sem gravações, conforme o contrato exFAT anterior.",
             f"- **{report['exfat_legacy_recovery']} cortes adicionais + {report['exfat_legacy_guards']} proteções exFAT** "
             "usaram transações gravadas pelo código anterior à unificação; compatibilidade e recuperação passaram.", "",
             "O verificador FAT32 independente compara as duas FATs e percorre cadeias/diretórios: "
             "órfãos LFN, clusters perdidos, crosslinks, referências a clusters livres, ciclos e tamanhos inválidos. "
             "Cada remontagem ocorre em processo novo e confere os dados de KEEP.BIN, SURVIVE.BIN e arquivos "
             "renomeados. Foi verificada a existência de exatamente um nome após renomear e a preservação do "
             "resultado em todos os cortes após o callback de confirmação. FATtools 1.1.23 conferiu também "
             "nomes e payloads das 16 imagens concluídas.", "",
             "## Correções e estrutura", "",
             "A escrita agora atualiza as duas FATs; anteriormente apenas a primeira era alterada. "
             "A expansão de diretórios reserva e termina o novo cluster, inicializa-o e só então publica o elo. "
             "Reservas de clusters em andamento não são oferecidas a outro arquivo. Uma renomeação com as duas "
             "entradas curtas no mesmo setor publica/retira ambas juntas.", "",
             "As barreiras sozinhas ainda deixavam 75 de 185 cortes com órfãos, FATs divergentes ou renomeação "
             "incompleta. O diário autorizado pelo usuário fecha esses intervalos depois de provisionado: "
             "before-image → descritor → cabeçalho confirmado → alteração do metadado → checkpoint. "
             "Clusters liberados não são realocados antes de confirmar a transação.", "",
             "`asyncfatfs_journal.inc` contém um único motor de captura, índice em disco, CRC, I/O assíncrono, "
             "checkpoint, seleção de cabeçalhos e validação/replay. `asyncfatfs_fatjournal.inc` e "
             "`asyncfatfs_exfatjournal.inc` preservam apenas descoberta, alocação e marcadores específicos. "
             "Os formatos AFJFT001/AFFUNDO1 e AFJNL002/AFUNDO02 são preservados; não há migração de disco. "
             "O cache retém a imagem original até autorizar a modificação, eliminando os buffers dedicados "
             "`before` e `header` do exFAT. A reserva do freefile é tratada como reserva, permitindo que o "
             "checkpoint termine antes de o fechamento liberar seu bloqueio.", "",
             "## Memória", "",
             "Bytes de `afatfs_t`, com quatro setores de cache e três handles públicos, sem freefile. "
             "Handles já estão incluídos no total; não os some novamente.", "",
             "| Configuração / alvo | Antes do diário FAT32 | Motores separados | Motor comum | Economia da unificação |",
             "| --- | ---: | ---: | ---: | ---: |"]
    for row in memory:
        if row["configuration"] not in ("default", "exfat_only", "lfn_exfat"):
            continue
        label = f"{row['configuration']} / {row['bits']} bits" + (" / enums curtos" if row["short_enums"] else "")
        lines.append(f"| {label} | {row['before_journal']} | {row['separate_engines']} | {row['shared_engine']} | {row['unification_saved']} |")
    lines += ["", "O FAT32 acrescenta 596 bytes em PIC24/16 bits e 600 bytes em x86/x64 em relação ao LFN "
              "sem diário. A unificação não aumenta esse custo. O exFAT economiza 1.024 bytes com enums "
              "normais e 1.032 bytes no PIC24 com enums curtos. A posição do epoch junto aos timestamps "
              "remove oito bytes de padding do cache nessa ABI. "
              "O controlador comum tem 580 bytes no PIC24 e 584 no host, incluindo um único buffer alinhado "
              "de 512 bytes; não há heap nem aumento em file/directory/request. A tabela CRC32 `const` ocupa "
              "1 KiB de armazenamento somente leitura, sem inicialização em RAM.", "",
              "As 24 combinações de layout passaram: seis configurações, x86/x64 e XC16/PIC24 com enums "
              "normais/curtos. Bitfields C99, alinhamento natural, buffer de quatro bytes e prefixos de "
              "suboperações foram verificados. Os resultados PIC24 são compilação cruzada, sem execução física. "
              "A regressão cobre também opt-out do diário, FAT/LFN/freefile e AddressSanitizer FAT/LFN e combinado "
              "LFN+exFAT; falhas em leitura, before-image, descritor, publicação, commit e liberação impedem "
              "`canPowerOff` até concluir as tentativas e todas as gravações.", "",
              "## Limites", "",
              "Os cortes FAT32 simulam cada prefixo de gravações concluídas e o prefixo de 1, 31, 64, 256, "
              "480 ou 511 bytes da gravação seguinte. Na recuperação foram usados 1, 31, 256 e 511 bytes. "
              "O backend de teste atrasa confirmações e grava snapshots; não reproduz toda a eletrônica "
              "de um cartão real. Não há garantia para corrupção arbitrária, caches voláteis que confirmem "
              "antes de persistir, dados ainda em RAM ou sobrescrita de payload no lugar.", "",
              "FAT16 e FAT32 sem diário mantêm apenas ordenação. A reserva inicial pode precisar de reparo "
              "externo após uma queda; jamais se usa um log inválido para adivinhar o estado anterior. "
              "Depois de uma queda em sessão da biblioteca, recupere com ela antes de permitir outro escritor. "
              "Windows não executa este replay privado.", "",
              "Não há fsck.fat instalado neste ambiente. FAT32 foi auditado pelo verificador de grafo e "
              "FATtools; `fsck.exfat` foi usado somente em exFAT. A nova rodada VHD/CHKDSK foi recusada na "
              "solicitação de acesso administrativo e não foi executada. A validação Windows anterior "
              "permanece histórica, anterior ao diário FAT32.", "",
              "## Reprodução", "", "```powershell",
              "./tests/run_fat_lfn_powercut.ps1 -Python python -Torn",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/fat-lfn-powercut/shared-layout",
              "./tests/run_exfat_powercut.ps1 -Python python",
              "```", "",
              "FATtools 1.1.23 e exfatprogs 1.3.2 são dependências externas de teste descritas em "
              "[EXFAT_INTEROP.md](EXFAT_INTEROP.md). JSON resumido com hashes de fontes e evidências "
              "fica ao lado deste relatório; os relatórios completos e imagens permanecem em tests/build.", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
