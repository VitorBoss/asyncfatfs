"""Summarize the native FAT32/VHD results and verify that runtime layouts did not grow."""
import argparse
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("tests/WINDOWS_FAT32_LFN_VALIDATION.md"))
    args = parser.parse_args()
    native = json.loads(args.native.read_text(encoding="utf-8-sig"))
    cleanup = json.loads((args.native.parent / "cleanup-verification.json").read_text(encoding="utf-8-sig"))
    assert native["status"] == "passed" and not native["cleanupErrors"] and not native["error"]
    assert cleanup["status"] == "passed" and all(not image["attached"] for image in cleanup["images"])
    assert len(native["cases"]) == 3 and len(cleanup["images"]) == 3
    assert all(case["status"] == "passed" and len(case["aliases"]) == 33 for case in native["cases"])
    assert all(command["exitCode"] == 0 for command in native["commands"])
    checks = [command for command in native["commands"] if "chkdsk" in command["name"]]
    assert len(checks) == 12 and all(len(command["arguments"]) == 1 for command in checks)
    for case in native["cases"]:
        audited = next(image for image in cleanup["images"] if image["name"] == case["name"])
        assert audited["footerSha256"] == case["footerSha256"]
    repository = Path(__file__).resolve().parents[1]
    for filename, digest in native["sourceSha256"].items():
        assert hashlib.sha256((repository / filename).read_bytes()).hexdigest().upper() == digest.upper(), filename
    assert hashlib.sha256((args.native.parent / "workload.exe").read_bytes()).hexdigest().upper() == native["binarySha256"]
    before = load_report(args.before)
    after = load_report(args.after)
    assert before == after and len(after) == 24, "A measured layout changed"
    rows = []
    for bits, short in ((16, False), (16, True), (32, False), (64, False)):
        values = after[("default", bits, short)]
        rows.append({"bits": bits, "short_enums": short, **values})
    lines = ["# LFN/FAT32 com o driver nativo do Windows", "",
             "Validação em 09/11/2026 com VHDs fixos de 512 MiB, MBR, formatados pelo Windows em FAT32. "
             "A biblioteca foi compilada com LFN habilitado e exFAT desabilitado, usando o backend original "
             "de arquivos de imagem e suas operações assíncronas.", "",
             f"Windows: `{native['windowsVersion']}`. Driver `fastfat.sys`: `{native['fatDriverVersion']}`.", "",
             "## Resultado nativo", "",
             "| Formatação | Cluster efetivo | VHD | CHKDSK sem reparo | Resultado |",
             "| --- | ---: | ---: | ---: | --- |"]
    for case in native["cases"]:
        lines.append(f"| {case['name']} | {case['clusterSize']} bytes | 512 MiB | 4 × código 0 | Passou |")
    lines += ["", "As 12 verificações ocorreram após a preparação nativa, após as gravações da biblioteca, "
              "após as alterações pelo Windows e ao final do retorno pela biblioteca. Não foram usados `/f`, "
              "`/r`, `/x` nem reparos para obter aprovação. O CHKDSK concluiu as verificações de arquivos/pastas "
              "sem problemas no sistema de arquivos. Seu aviso sobre armazenamento de cópias de sombra "
              "também aparece nos registros, com código final 0.", "",
              "O executor elevado e uma auditoria elevada posterior confirmaram os três VHDs desmontados. "
              "Os rodapés SHA-256 permaneceram idênticos, inclusive depois da última gravação nativa. "
              "As operações de disco verificam o vínculo com a imagem, tamanho, barramento virtual, "
              "partição e rótulo antes de acessar o volume.", "",
              "## Cobertura", "",
              "- Windows criou 19 nomes/aliases por imagem, incluindo 14 colisões, nome de 255 caracteres "
              "e `lower.txt` com flags NT de caixa. A biblioteca leu os dados por nome longo e pelo alias "
              "real obtido com `GetShortPathNameW`, conferindo o nome retornado por fstat.",
              "- Foram aceitos aliases nativos numéricos e derivados de hash, por exemplo `COLLIS~4.BIN` "
              "e `CO483D~1.BIN`. Não se presume que o Windows gere o mesmo alias que a biblioteca.",
              "- A biblioteca criou mais 14 nomes em colisão por imagem; o Windows leu o conteúdo tanto "
              "pelo nome longo quanto pelo alias. São 99 pares nome/alias registrados nas três imagens.",
              "- Nomes com 13, 26 e 255 caracteres, escrita parcial, append, truncamento para 333 bytes, "
              "exclusão por alias, enumeração com padrões, troca apenas de caixa e renomeação de arquivos/diretórios.",
              "- Crescimento de diretórios nativos e da biblioteca além de um cluster, diretório vazio, "
              "diretórios aninhados e getcwd após renomeação nativa do pai.",
              "- Windows sobrescreveu/renomeou um arquivo da biblioteca, renomeou o arquivo de nome máximo "
              "e um diretório, excluiu outro arquivo e criou um filho. A biblioteca remontou, conferiu "
              "os dados e gravou um último arquivo de 701 bytes, relido pelo Windows antes do CHKDSK final.", "",
              "## Correção encontrada", "",
              "A primeira rodada parou ao fechar um diretório aberto com `afatfs_open`: "
              "`afatfs_apiFileRequest` aceitava apenas arquivos normais, embora open também devolvesse "
              "handles de diretório e as operações legadas já os suportassem. A validação agora aceita "
              "diretórios em close/sync. Dados e truncate continuam restritos a arquivos normais. "
              "A regressão cobre diretórios longos e raízes FAT16/FAT32, incluindo rejeição de truncate.", "",
              "Após a correção, a rodada inteira foi repetida com imagens novas. Não foi necessário "
              "alterar o formato VFAT nem a ordem de campos dos registros de disco. A arquitetura de "
              "cache, máquinas de estado, buffers e estruturas persistentes foi preservada.", "",
              "## Memória e alinhamento", "",
              "| Alvo | Enums | Estado global antes → depois | Handle | Directory | Request | Acréscimo |",
              "| --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['bits']} bits | {'curtos' if row['short_enums'] else 'normais'} | "
                     f"{row['afatfs_t']} → {row['afatfs_t']} | {row['afatfsFile_t']} | "
                     f"{row['afatfsDirectory_t']} | {row['afatfsRequest_t']} | 0 bytes |")
    lines += ["", "Todos os tamanhos e offsets medidos permaneceram iguais nas 24 combinações de configuração/ABI. "
              "Foram executados testes de bitfields/alinhamento em x86/x64 e compilação C99 real para "
              "XC16/PIC24, com enums normais/curtos. Não houve execução em hardware de 16 bits. "
              "As regressões das seis configurações e a compilação dos oito executáveis originais passaram. "
              "A regressão x64 FAT/LFN também passou com AddressSanitizer, sem violações relatadas.", "",
              "O escopo continua sendo nomes ASCII na API, setores de 512 bytes e interfaces de 32 bits. "
              "O teste usou discos virtuais; não representa teste de cartão físico nem de queda de energia.", "",
              "## Reprodução e evidências", "", "```powershell",
              "./tests/run_windows_fat32_lfn_tests.ps1",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/windows-fat32-lfn-layout-after", "```", "",
              "As APIs de VHD exigem um executor administrador; o runner compila antes de solicitar elevação, "
              "usa uma pasta nova por rodada e mantém a janela do worker oculta. "
              "`-NoWait` apenas inicia o trabalho: o resultado deve ser consultado em report.json.", "",
              f"[Registro nativo completo]({args.native.relative_to(args.output.parent).as_posix()}) · "
              f"[Auditoria de desmontagem]({(args.native.parent / 'cleanup-verification.json').relative_to(args.output.parent).as_posix()})", "",
              "[Especificação do CHKDSK](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/chkdsk) · "
              "[Format-Volume](https://learn.microsoft.com/en-us/powershell/module/storage/format-volume)", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    summary = {"status": "passed", "native_report_sha256": hashlib.sha256(args.native.read_bytes()).hexdigest(),
               "binary_sha256": native["binarySha256"], "source_sha256": native["sourceSha256"],
               "windows": native["windowsVersion"], "fat_driver": native["fatDriverVersion"],
               "cases": [{"name": case["name"], "cluster_size": case["clusterSize"], "status": case["status"],
                          "aliases": case["aliases"], "footer_sha256": case["footerSha256"]} for case in native["cases"]],
               "chkdsk_checks": [{"name": command["name"], "exit_code": command["exitCode"]} for command in checks],
               "layouts_identical": len(after), "default_sizes": rows, "cleanup": cleanup}
    args.output.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: 3 native FAT32 VHDs, 12 CHKDSK checks, 24 unchanged layouts: {args.output}")


if __name__ == "__main__":
    main()
