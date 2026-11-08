"""Validate and summarize the common FAT32 8.3 benchmark and memory probes."""

import argparse
import csv
import hashlib
import json
import os
import platform
import re
import statistics
import struct
import subprocess
from collections import defaultdict
from pathlib import Path


VARIANTS = ("upstream_native", "upstream_equal", "current_native")


def coff_sections(path):
    data = path.read_bytes()
    count = struct.unpack_from("<H", data, 2)[0]
    offset = 20 + struct.unpack_from("<H", data, 16)[0]
    sections = []
    for index in range(count):
        entry = data[offset + index * 40 : offset + (index + 1) * 40]
        sections.append(
            {
                "name": entry[:8].split(b"\0")[0].decode(),
                "size": struct.unpack_from("<I", entry, 16)[0],
                "offset": struct.unpack_from("<I", entry, 20)[0],
                "flags": struct.unpack_from("<I", entry, 36)[0],
            }
        )
    return data, sections


def coff_layout(path):
    data, sections = coff_sections(path)
    symbol_offset, symbol_count = struct.unpack_from("<II", data, 8)
    string_offset = symbol_offset + symbol_count * 18
    index = 0
    while index < symbol_count:
        entry = data[symbol_offset + index * 18 : symbol_offset + (index + 1) * 18]
        if entry[:4] == b"\0\0\0\0":
            start = string_offset + struct.unpack_from("<I", entry, 4)[0]
            name = data[start : data.index(b"\0", start)].decode()
        else:
            name = entry[:8].split(b"\0")[0].decode()
        if name.lstrip("_") == "comparison_layout":
            value = struct.unpack_from("<I", entry, 8)[0]
            section = struct.unpack_from("<h", entry, 12)[0]
            return struct.unpack_from("<HHH", data, sections[section - 1]["offset"] + value)
        index += 1 + entry[17]
    raise ValueError(f"Layout symbol absent: {path}")


def memory_results(root):
    results = []
    for variant in VARIANTS:
        for abi in ("32", "64", "16-normal", "16-short"):
            directory = root / f"{variant}-{abi}"
            ram = 0
            if abi in ("32", "64"):
                layout = coff_layout(directory / "layout.obj")
                for source in ("asyncfatfs", "fat_standard"):
                    _, sections = coff_sections(directory / f"{source}.obj")
                    ram += sum(
                        section["size"]
                        for section in sections
                        if section["flags"] & 0x80000000 and section["flags"] & 0xC0
                    )
            else:
                assembly = (directory / "layout.s").read_text()
                body = assembly.split("_comparison_layout:", 1)[1]
                layout = tuple(map(int, re.findall(r"^\s*\.word\s+(\d+)", body, re.M)[:3]))
                assert len(layout) == 3
                for source in ("asyncfatfs", "fat_standard"):
                    listing = (directory / f"{source}-sections.txt").read_text(encoding="utf-8-sig")
                    pattern = (
                        r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+"
                        r"[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+2\*\*\d+\n\s*([^\n]*)"
                    )
                    for match in re.finditer(pattern, listing, re.M):
                        _, size, flags = match.groups()
                        if "ALLOC" in flags and not any(tag in flags for tag in ("CODE", "READONLY", "PSV")):
                            ram += int(size, 16)
            results.append(
                dict(variant=variant, abi=abi, static_ram=ram, global_state=layout[0], handle=layout[1], descriptor=layout[2])
            )
    return results


def summarize(root):
    results = []
    audit = {}
    integer_fields = (
        "profile", "cluster_sectors", "chunk_bytes", "files", "repetition", "payload_bytes", "reads", "writes",
        "multi_sessions", "multi_blocks", "polls", "virtual_us", "metadata_reads", "metadata_writes",
    )
    for variant in VARIANTS:
        directory = root / f"{variant}-64"
        rows = list(csv.DictReader((directory / "results.csv").open(encoding="utf-8-sig")))
        assert len(rows) == 224, (variant, len(rows))
        groups = defaultdict(list)
        for row in rows:
            for field in integer_fields:
                row[field] = int(row[field])
            row["host_seconds"] = float(row["host_seconds"])
            assert row["host_seconds"] > 0
            assert row["writes"] >= row["metadata_writes"]
            assert row["reads"] >= row["metadata_reads"]
            key = (row["phase"], row["profile"], row["cluster_sectors"], row["chunk_bytes"], row["files"])
            groups[key].append(row)
        for key, samples in sorted(groups.items()):
            assert len(samples) == 7
            assert sorted(row["repetition"] for row in samples) == list(range(7))
            for field in integer_fields:
                if field != "repetition":
                    assert len({row[field] for row in samples}) == 1, (variant, key, field)
            row = dict(samples[0])
            row.pop("repetition")
            seconds = [sample["host_seconds"] for sample in samples]
            row.update(
                variant=variant,
                repetitions=7,
                host_seconds=statistics.median(seconds),
                host_seconds_min=min(seconds),
                host_seconds_max=max(seconds),
            )
            row["host_mib_s"] = row["payload_bytes"] / 1048576 / row["host_seconds"]
            row["modeled_mib_s"] = row["payload_bytes"] / 1048576 / (row["virtual_us"] / 1000000)
            results.append(row)
        mirrors = re.findall(r"MIRROR,(\d+),(\d+),(\d+)", (directory / "audit.log").read_text())
        assert len(mirrors) == 112
        audit[variant] = {
            "volumes": len(mirrors),
            "primary_content_size_chain_passed": True,
            "mirror_identical_volumes": sum(int(row[2]) == 0 for row in mirrors),
            "mirror_different_sectors_min": min(int(row[2]) for row in mirrors),
            "mirror_different_sectors_max": max(int(row[2]) for row in mirrors),
        }
    assert audit["current_native"]["mirror_identical_volumes"] == 112
    return results, audit


def table(headers, rows):
    return "\n".join(
        ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
        + ["| " + " | ".join(map(str, row)) + " |" for row in rows]
    )


def render(report):
    memory = {(row["variant"], row["abi"]): row for row in report["memory"]}
    samples = {
        (row["variant"], row["phase"], row["profile"], row["cluster_sectors"], row["chunk_bytes"], row["files"]): row
        for row in report["benchmark"]
    }
    case_keys = [(cluster, chunk, files) for cluster in (1, 8) for chunk in (257, 4096) for files in (1, 2)]
    matching_reads = sum(
        samples[("current_native", "read", 1, *key)]["reads"]
        == samples[("upstream_equal", "read", 1, *key)]["reads"]
        for key in case_keys
    )
    write_deltas_512 = [
        (samples[("current_native", "write", 1, *key)]["modeled_mib_s"]
         / samples[("upstream_equal", "write", 1, *key)]["modeled_mib_s"] - 1) * 100
        for key in case_keys if key[0] == 1
    ]
    lines = [
        "# Comparação FAT32 8.3 com upstream master",
        "",
        "Data: 16/11/2026, America/Sao_Paulo.",
        "",
        "A configuração atual reduz a RAM estática em relação ao padrão upstream, principalmente por usar metade "
        "do cache. Com cache igualado, ela usa um pouco mais de RAM. "
        f"A quantidade de setores lidos coincide em {matching_reads} dos oito cenários de leitura do perfil de latência. "
        "Com clusters de 512 bytes, a diferença da taxa modelada de escrita em relação ao upstream igualado "
        f"fica entre {min(write_deltas_512):+.1f}% e {max(write_deltas_512):+.1f}%. "
        "O upstream deixa a segunda FAT desatualizada em todos os volumes deste ensaio; a versão atual mantém "
        "as duas FATs iguais. Portanto, menos gravações no upstream também refletem comportamentos diferentes "
        "de manutenção dos metadados.",
        "",
        "As velocidades abaixo são de execução no computador com mídia em RAM e de um modelo explícito de "
        "latência. Elas não medem um cartão SD físico nem permitem afirmar a velocidade real em PIC24.",
        "",
        "## Revisões e configurações",
        "",
        f"- Upstream: `{report['upstream_commit']}`, referência local `upstream/master`, sem atualização por fetch.",
        f"- Atual: checkout baseado em `{report['current_head']}`, com as alterações locais registradas nos hashes "
        f"de `{report['json_artifact']}`. A comparação não representa somente o commit HEAD.",
        "- Upstream padrão: 8 setores de cache, 3 handles, limiar de múltiplos blocos 4.",
        "- Upstream igualado: 4 setores de cache, 3 handles, limiar de múltiplos blocos 2.",
        "- Atual padrão: 4 setores de cache, 3 handles, limiar de múltiplos blocos 2; API de compatibilidade, "
        "LFN, exFAT, freefile e diário desabilitados.",
        "",
        "## RAM estática",
        "",
        "Bytes de todas as seções graváveis de `asyncfatfs.c` e `fat_standard.c`, incluindo variáveis estáticas "
        "locais. Cache e handles já estão incluídos. Não inclui backend, heap do ensaio, pilha nem buffers da "
        "aplicação; a biblioteca não usa heap.",
        "",
    ]
    rows = []
    for abi, label in (("16-normal", "PIC24"), ("16-short", "PIC24 enums curtos"), ("32", "x86"), ("64", "x64")):
        upstream = memory[("upstream_native", abi)]["static_ram"]
        equal = memory[("upstream_equal", abi)]["static_ram"]
        current = memory[("current_native", abi)]["static_ram"]
        rows.append([label, upstream, equal, current, f"{current-upstream:+d} ({(current/upstream-1)*100:+.1f}%)", f"{current-equal:+d} ({(current/equal-1)*100:+.1f}%)"])
    lines += [table(["ABI", "Upstream padrão", "Upstream igualado", "Atual", "Atual vs padrão", "Atual vs igualado"], rows), ""]
    lines += ["### Estruturas com cache igualado", ""]
    rows = []
    for abi in ("16-normal", "16-short", "32", "64"):
        before = memory[("upstream_equal", abi)]
        after = memory[("current_native", abi)]
        rows.append([abi, before["global_state"], after["global_state"], before["handle"], after["handle"]])
    lines += [table(["ABI", "afatfs_t upstream", "afatfs_t atual", "Handle upstream", "Handle atual"], rows), ""]
    lines += [
        "`sizeof(afatfs_t)` não é toda a RAM estática. Os totais anteriores incluem também os demais símbolos "
        "graváveis. Os valores dos handles são informativos e não devem ser somados novamente.",
        "",
        "## Carga e método",
        "",
        "- FAT32 sintético com MBR, 65.530 clusters, dois FATs, setores de 512 bytes; clusters de 512 ou 4.096 "
        "bytes. FSInfo e boot de backup ausentes, indicados como 0xFFFF. Cada caso começa com volume vazio idêntico.",
        "- Um ou dois arquivos de 1 MiB, nomes 8.3, blocos da aplicação de 257 ou 4.096 bytes. Dois arquivos "
        "são processados alternadamente na mesma thread.",
        "- Escrita inclui criação, transferência, fechamento e drenagem explícita. Depois da drenagem, o "
        "desmonte não pode fazer gravações adicionais: essa condição é verificada por assert.",
        "- Leitura usa remontagem para esvaziar o cache da biblioteca e inclui abertura, consulta de tamanho, "
        "leitura com verificação byte a byte, fechamento e drenagem. Montagem, formatação e auditoria independente "
        "ficam fora das fases cronometradas.",
        "- MSVC x64 `/O2 /std:c11 /W3 /WX`, mesmo computador e backend assíncrono para ambas as revisões. "
        "Sete repetições por cenário; taxas do computador calculadas pela mediana dos tempos. Execuções das "
        "revisões foram sequenciais, sem randomização; variação de frequência, escalonamento e aquecimento "
        "podem influenciar os tempos. Mínimos e máximos estão no JSON. O perfil de latência normal foi usado; "
        "o diagnóstico de latência reduzida da FAT secundária não está habilitado nesta comparação.",
        "- RAM medida também em MSVC x86 e XC16 2.10 para `24FJ256GA702`, enums normais/curtos. PIC24 usa "
        "compilação cruzada, sem execução em hardware. A licença XC16 informa restrições de otimização; isso "
        "não altera os valores de sizeof e das seções de RAM usados aqui.",
        "- O header upstream usa atributos GCC para registros de disco. No MSVC, um header forçado aplica "
        "`#pragma pack(push, 1)` apenas a esse header e neutraliza `__attribute__`. Os fontes upstream "
        "extraídos do commit permanecem intactos.",
        "",
        "### Perfis do backend",
        "",
        "Perfil 0: cada transferência completa no próximo poll, com mídia em RAM; usado para medir o custo "
        "de execução no computador. Não simula uma velocidade física.",
        "",
        "Perfil 1: cada poll avança 10 µs virtuais; leitura de setor custa 200 µs, escrita isolada 800 µs e "
        "escrita em sequência múltipla 400 µs, com 100 µs para iniciar/encerrar uma sequência ativa. São "
        "premissas do ensaio, não especificações de um cartão. O tempo virtual inclui polls de progresso "
        "da máquina de estados e pode alterar a oportunidade de agrupar metadados.",
        "",
        "## I O e velocidade no modelo de latência",
        "",
        "R/W são setores de 512 bytes lidos/escritos. A taxa é o payload agregado dividido pelo tempo virtual, "
        "em MiB/s. As três configurações usam o perfil 1. A tabela de escrita conta dados e metadados.",
        "",
    ]
    for phase, title in (("write", "Escrita"), ("read", "Leitura")):
        lines += [f"### {title}", ""]
        rows = []
        for cluster in (1, 8):
            for chunk in (257, 4096):
                for files in (1, 2):
                    values = [samples[(variant, phase, 1, cluster, chunk, files)] for variant in VARIANTS]
                    rows.append([cluster*512, chunk, files] + [f"{row['reads']}/{row['writes']}; {row['modeled_mib_s']:.3f}" for row in values])
        lines += [table(["Cluster B", "Bloco B", "Arquivos", "Upstream padrão R/W; MiB/s", "Upstream igualado R/W; MiB/s", "Atual R/W; MiB/s"], rows), ""]
    lines += ["### Diferenças com cache igualado", "", "Variações da versão atual em relação ao upstream com quatro páginas. "
              "Reads e writes referem-se à fase de escrita; as taxas vêm das respectivas fases. "
              "Taxa negativa significa menor throughput; I/O negativo significa menos setores transferidos.", ""]
    rows = []
    for key in case_keys:
        current_write = samples[("current_native", "write", 1, *key)]
        upstream_write = samples[("upstream_equal", "write", 1, *key)]
        current_read = samples[("current_native", "read", 1, *key)]
        upstream_read = samples[("upstream_equal", "read", 1, *key)]
        rows.append([
            key[0] * 512, key[1], key[2],
            f"{current_write['reads'] - upstream_write['reads']:+d}",
            f"{current_write['writes'] - upstream_write['writes']:+d}",
            f"{(current_read['modeled_mib_s'] / upstream_read['modeled_mib_s'] - 1) * 100:+.2f}%",
            f"{(current_write['modeled_mib_s'] / upstream_write['modeled_mib_s'] - 1) * 100:+.2f}%",
        ])
    lines += [table(["Cluster B", "Bloco B", "Arquivos", "Delta reads", "Delta writes", "Delta taxa leitura", "Delta taxa escrita"], rows), ""]
    lines += ["### Gravações de metadados e múltiplos blocos", "", "Perfil 1. Metadados incluem MBR/boot, FATs e cluster raiz; os demais setores são dados dos arquivos.", ""]
    rows = []
    for cluster in (1, 8):
        for chunk in (257, 4096):
            for files in (1, 2):
                values = [samples[(variant, "write", 1, cluster, chunk, files)] for variant in VARIANTS]
                rows.append([cluster*512, chunk, files] + [f"{row['metadata_writes']}; {row['multi_blocks']}" for row in values])
    lines += [table(["Cluster B", "Bloco B", "Arquivos", "Upstream padrão meta/multi", "Upstream igualado meta/multi", "Atual meta/multi"], rows), ""]
    lines += [
        "## Velocidade medida no computador",
        "",
        "Mediana em MiB/s, perfil 0, sem latências físicas. Inclui chamadas da biblioteca, backend em RAM e "
        "geração/verificação do payload. Essas taxas não estimam diretamente o throughput em um SD ou em "
        "um microcontrolador.",
        "",
    ]
    rows = []
    for phase in ("write", "read"):
        for cluster in (1, 8):
            for chunk in (257, 4096):
                for files in (1, 2):
                    values = [samples[(variant, phase, 0, cluster, chunk, files)] for variant in VARIANTS]
                    rows.append([phase, cluster*512, chunk, files] + [f"{row['host_mib_s']:.1f}" for row in values])
    lines += [table(["Fase", "Cluster B", "Bloco B", "Arquivos", "Upstream padrão", "Upstream igualado", "Atual"], rows), ""]
    lines += [
        "## Integridade e interpretação",
        "",
        "Foram concluídas 336 execuções de volume: 112 por configuração, com 672 fases medidas. Em cada "
        "volume, um auditor independente percorreu a FAT primária, confirmou tamanho, conteúdo byte a byte e "
        "fim exato da cadeia, além da quantidade total de clusters alocados, para detectar caudas extras. "
        "A biblioteca também releu os arquivos após remontar. As contagens de I/O e "
        "tempos virtuais foram idênticos nas sete repetições de cada cenário.",
        "",
    ]
    rows = []
    for variant in VARIANTS:
        audit = report["audit"][variant]
        rows.append([variant, audit["volumes"], audit["mirror_identical_volumes"], f"{audit['mirror_different_sectors_min']}–{audit['mirror_different_sectors_max']}"])
    lines += [table(["Configuração", "Volumes auditados", "Volumes com FATs iguais", "Setores FAT divergentes"], rows), ""]
    lines += [
        "A divergência da FAT secundária upstream foi registrada como resultado da auditoria, não ocultada "
        "para declarar interoperabilidade completa. A validação de conteúdo pelo FAT primário não elimina "
        "essa diferença. O ensaio não valida perda de energia nem equivale a CHKDSK/fsck.",
        "",
        "A versão atual mantém espelhamento e barreiras de persistência adicionais. "
        "O ganho de RAM do padrão atual não implica ganho de escrita. "
        "O agrupamento de múltiplos blocos ajuda em alguns cenários com clusters de 4 KiB, mas não garante "
        "menos setores escritos. Esta comparação não isola quanto do custo vem do espelhamento, das barreiras "
        "ou de outras alterações; seria necessário medir variantes específicas para atribuir cada parcela.",
        "",
        f"Na leitura do perfil 1, {matching_reads} dos oito cenários têm contagens de setores iguais ao upstream igualado. "
        "As diferenças por cenário aparecem nas tabelas. Diferenças de taxa com as mesmas contagens de I/O "
        "decorrem dos polls de progresso no modelo; os tempos do computador "
        "também incluem o custo de implementação e variação do host.",
        "",
        "## Evidências e reprodução",
        "",
        "- `tests/run_upstream_comparison.ps1`: extrai o commit, copia o checkout, compila, executa e gera este relatório.",
        "- `tests/upstream_comparison.c`: carga comum, backend e auditor independente.",
        "- `tests/upstream_comparison_report.py`: valida repetições e calcula as tabelas.",
        f"- `{report['evidence_directory']}/`: snapshots completos de lib, configuração igualada, "
        "shim MSVC, binários, objetos, logs, CSVs, probes de RAM e resultados brutos. Essa pasta é ignorada pelo Git.",
        f"- `{report['json_artifact']}`: resultados consolidados, dispersão dos tempos, revisão e hashes "
        "das fontes medidas. Os números se aplicam a esses snapshots.",
        "",
        "```powershell",
        f"./tests/run_upstream_comparison.ps1 -OutputDirectory {report['evidence_directory']} "
        f"-ReportPath {report['report_artifact']}",
        "```",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path("tests/build/upstream-fat32-83-20261116"))
    parser.add_argument("--output", type=Path, default=Path("tests/UPSTREAM_FAT32_83_COMPARISON.md"))
    args = parser.parse_args()
    benchmark, audit = summarize(args.directory)
    manifest_path = args.directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
    report = {
        "evidence_directory": args.directory.as_posix(),
        "report_artifact": args.output.as_posix(),
        "json_artifact": args.output.with_suffix(".json").as_posix(),
        "upstream_commit": manifest.get("upstream_commit") or subprocess.check_output(["git", "rev-parse", "upstream/master"], text=True).strip(),
        "current_head": manifest.get("current_head") or subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
        "manifest": manifest,
        "host": dict(os=platform.platform(), processor=os.environ.get("PROCESSOR_IDENTIFIER"), architecture=platform.machine()),
        "memory": memory_results(args.directory),
        "benchmark": benchmark,
        "audit": audit,
        "source_sha256": {
            str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(args.directory.glob("*/lib/*"))
            if path.is_file()
        },
        "harness_sha256": hashlib.sha256(Path("tests/upstream_comparison.c").read_bytes()).hexdigest(),
    }
    args.output.write_text(render(report), encoding="utf-8")
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"memory": report["memory"], "audit": audit}, indent=2))


if __name__ == "__main__":
    main()
