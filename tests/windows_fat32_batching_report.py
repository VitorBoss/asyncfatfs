"""Verify native Windows VHD evidence for concurrent LFN/cache batching."""
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report
from metadata_optimization_report import benchmark
from io_batching_report import multi

NATIVE = Path("tests/build/windows-fat32-lfn/run-io-batching-native-final-20261110")
CHECKS = Path("tests/build/windows-native-batching-final")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    native = read_json(NATIVE / "report.json")
    cleanup = read_json(NATIVE / "cleanup-verification.json")
    assert native["status"] == cleanup["status"] == "passed"
    assert not native["error"] and not native["cleanupErrors"]
    assert len(native["cases"]) == len(cleanup["images"]) == 3
    assert digest(Path("tests/audit_windows_fat32_lfn.ps1")).upper() == cleanup["auditScriptSha256"]
    for name, checksum in native["sourceSha256"].items():
        assert digest(Path(name)).upper() == checksum, name
    assert digest(NATIVE / "workload.exe").upper() == native["binarySha256"]
    checks = [entry for entry in native["commands"] if "chkdsk" in entry["name"]]
    assert len(checks) == 12 and all(len(entry["arguments"]) == 1 for entry in checks)
    assert all(entry["exitCode"] == 0 for entry in native["commands"])
    for case in native["cases"]:
        assert case["status"] == "passed" and len(case["aliases"]) == 33
        assert case["batching"]["multiSessions"] > 0 and case["batching"]["multiBlocks"] > 0
        audit = next(entry for entry in cleanup["images"] if entry["name"] == case["name"])
        assert not audit["attached"] and audit["footerSha256"] == case["footerSha256"]
        assert len(audit["aliases"]) == 4 and len({entry["alias"] for entry in audit["aliases"]}) == 4
        command = next(entry for entry in native["commands"] if entry["name"] == "library-concurrent-" + case["name"])
        assert "3 pending creates/closes, 2 writers, prefetch reader, rename overlap" in command["output"]
    before = load_report(Path("tests/build/io-batching/layout/report.json"))
    after = load_report(CHECKS / "layout/report.json")
    assert before == after and len(after) == 24
    for configuration in ("default", "combined"):
        assert benchmark(CHECKS / f"benchmark/{configuration}-before.log") == benchmark(
            CHECKS / f"benchmark/{configuration}-after.log")
    assert multi(CHECKS / "multi-combined-after/test.log") == multi(
        Path("tests/build/io-batching/multi-combined-after/test.log"))
    assert (CHECKS / "regression.log").read_text(errors="replace").count("global idle and clean power-off status: passed") == 6
    assert "global idle and clean power-off status: passed" in (CHECKS / "asan.log").read_text(errors="replace")
    for configuration in ("default", "no_multiblock"):
        assert "cache batching:" in (CHECKS / f"cache-{configuration}/test.log").read_text(errors="replace")
    counts = {}
    for name, relative, key in (("operations", "report.json", "cuts"),
                                ("recovery", "recovery/report.json", "cuts"),
                                ("guards", "guards/report.json", "cases"),
                                ("bootstrap", "provision/report.json", "cuts")):
        report = read_json(CHECKS / "powercut" / relative)
        assert report["status"] == "passed"
        counts[name] = len(report[key])
    result = {"status": "passed", "native_report": str(NATIVE / "report.json"),
              "native_report_sha256": digest(NATIVE / "report.json"), "native": native,
              "cleanup": cleanup, "layouts_identical": len(after), "powercuts": counts,
              "source_sha256": native["sourceSha256"], "evidence_sha256": {}}
    evidence = list(CHECKS.rglob("report.json")) + list(CHECKS.glob("*.log"))
    evidence += list(CHECKS.glob("multi-*/test.log")) + list(CHECKS.glob("cache-*/test.log"))
    evidence += [NATIVE / "cleanup-verification.json"]
    for path in evidence:
        result["evidence_sha256"][path.as_posix()] = digest(path)
    lines = ["# Agrupamento de I/O e LFN simultâneo: driver nativo Windows/FAT32", "",
             f"Rodada iniciada em `{native['startedUtc']}`. Windows `{native['windowsVersion']}`; "
             f"driver `fastfat.sys` `{native['fatDriverVersion']}`. Três VHDs fixos novos de 512 MiB, "
             "MBR, formatados pelo Windows. Configuração original: FAT/LFN, exFAT desabilitado, "
             "quatro páginas de cache e três handles públicos.", "",
             "## Resultado", "",
             "| Formatação | Cluster efetivo | CHKDSK sem reparo | Sequências múltiplas | Blocos em sequência |",
             "| --- | ---: | --- | ---: | ---: |"]
    for case in native["cases"]:
        lines.append(f"| {case['name']} | {case['clusterSize']} bytes | 4 × código 0 | "
                     f"{case['batching']['multiSessions']} | {case['batching']['multiBlocks']} |")
    lines += ["", "As 12 execuções de CHKDSK ocorreram antes/depois da biblioteca, após alterações "
              "nativas e no fim do retorno pela biblioteca. Não houve `/f`, `/r`, `/x` ou reparos "
              "para fazer a imagem passar. Os conteúdos foram lidos pelo Windows e pela biblioteca.", "",
              "A auditoria posterior montou os VHDs somente para leitura, verificou quatro arquivos "
              "concorrentes por seus nomes longos e aliases nativos distintos, conferiu os bytes e "
              "confirmou as três imagens desmontadas e os rodapés SHA-256 intactos.", "",
              "## Simultaneidade e cobertura", "",
              "- Três criações LFN ficam pendentes juntas, exercitando serialização do percurso do diretório e colisões de aliases.",
              "- Renomeação de um LFN com outro escritor ativo. A operação usa dois handles temporários; "
              "o teste respeita o limite original de três, sem aumentar AFATFS_MAX_OPEN_FILES.",
              "- Depois, dois escritores são intercalados com um leitor e chamadas de prefetch. Um "
              "arquivo usa pré-alocação de 64 KiB; outro cresce por append até 33.101 bytes. "
              "Os três fechamentos ficam pendentes juntos e canPowerOff permanece falso durante as esperas.",
              "- Windows cria dois arquivos em tarefas paralelas sincronizadas por barreira, a biblioteca "
              "lê os dados, e o Windows sobrescreve paralelamente dois arquivos da biblioteca. "
              "A biblioteca remonta, confere as alterações e grava um último arquivo, relido pelo Windows.",
              "- Mantida a cobertura de nomes de 13/26/255 caracteres, aliases Windows numéricos/hash, "
              "enumeração, truncamento, rename de caixa, exclusão e diretórios aninhados.", "",
              "As operações da biblioteca são assíncronas e intercaladas na mesma thread; isso não "
              "adiciona suporte a chamadas multithread. Windows e biblioteca alternam a propriedade "
              "do volume: acesso bruto ocorre somente com o VHD desmontado. O backend de teste "
              "implementa pré-apagamento destrutivo e confirmações atrasadas. Contadores são opcionais "
              "e pertencem exclusivamente a tests/sdcard_sim, sem campos na biblioteca.", "",
              "## Correção encontrada", "",
              "A primeira rodada nativa encontrou espera infinita ao criar o nome de 255 caracteres "
              "no volume com cluster de 4 KiB. Três páginas sujas deixavam uma página reutilizável; "
              "o percurso FAT e a entrada LFN expulsavam um ao outro. As leituras conseguiam "
              "alocar, então a salvaguarda de falta de cache não disparava e o poll continuava adiando o flush.", "",
              "A condição de adiamento agora permite escoamento quando a busca/reserva ou escrita "
              "de entradas LFN precisa de duas páginas e há menos de duas reutilizáveis. "
              "Ela também contempla criação de freefile/log. Não foram adicionados campos ou buffers. "
              "Um teste específico preserva a condição de três páginas sujas com uma criação publicando.", "",
              "A salvaguarda foi limitada a essas fases: aplicá-la a toda alocação aumentava as "
              "escritas de dois escritores. Na versão final, as contagens do benchmark de streaming "
              "FAT32/exFAT com um/dois escritores e da montagem permaneceram iguais às da baseline. "
              "A gravação pré-alocada preservou as sequências 137 → 43 em FAT32 e 136 → 51 em exFAT. "
              "O tempo físico de SD não é medido pelo VHD.", "",
              "A rodada completa foi repetida em três imagens novas após o ajuste final; nenhuma "
              "imagem da rodada interrompida foi reparada e reaproveitada para obter aprovação.", "",
              "## Memória e regressões", "",
              "| Alvo padrão | Estado global antes → depois | Handle | Acréscimo |",
              "| --- | ---: | ---: | ---: |"]
    for bits, short in ((16, False), (16, True), (32, False), (64, False)):
        row = after[("default", bits, short)]
        label = f"{bits} bits" + (", enums curtos" if short else "")
        lines.append(f"| {label} | {row['afatfs_t']} → {row['afatfs_t']} | {row['afatfsFile_t']} | 0 bytes |")
    lines += ["", "24 layouts idênticos: x86/x64 executados e XC16/PIC24 C99 compilado com enums "
              "normais/curtos. Alinhamento, flags, setores e prefixos de suboperações preservados. "
              "Seis configurações passaram na regressão, oito testes legados compilaram e "
              "AddressSanitizer LFN+exFAT+freefile não encontrou erros. O backend destrutivo passou "
              "com begin BUSY/FAILURE, falhas de envio/ACK, locks, interrupção e recurso desabilitado.", "",
              f"FAT32: {counts['operations']} cortes nas operações, {counts['recovery']} no replay, "
              f"{counts['guards']} proteções e {counts['bootstrap']} cortes de provisionamento passaram. "
              "Essas falhas usam backend simulado; não são cortes de energia no driver nativo. "
              "O escopo continua ASCII, setores de 512 bytes e endereços de 32 bits.", "",
              "## Reprodução", "", "```powershell",
              "./tests/run_windows_fat32_lfn_tests.ps1",
              "# Após concluir, audite o mesmo RunName como administrador:",
              "./tests/audit_windows_fat32_lfn.ps1 -RunName run-...",
              "python tests/windows_fat32_batching_report.py", "```", "",
              "Os executores administrativos validam vínculo entre arquivo VHD, disco virtual, "
              "partição, rótulo e diretório exclusivo da rodada. A biblioteca permanece sem elevação "
              "em produção; elevação é necessária para montar e consultar os VHDs de teste.", "",
              f"[Evidência nativa]({NATIVE.relative_to('tests').as_posix()}/report.json) · "
              f"[Auditoria]({NATIVE.relative_to('tests').as_posix()}/cleanup-verification.json). "
              "O JSON ao lado inclui hashes das fontes, binário e evidências.", ""]
    Path("tests/WINDOWS_FAT32_IO_BATCHING.md").write_text("\n".join(lines), encoding="utf-8")
    Path("tests/WINDOWS_FAT32_IO_BATCHING.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
