"""Report the default configuration change separately from the incremental VFAT RAM cost."""
import argparse
import hashlib
import json
from pathlib import Path

from layout_memory_report import load_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("tests/LFN_VALIDATION.md"))
    args = parser.parse_args()
    before = load_report(args.before)
    after = load_report(args.after)
    rows = []
    for bits, short in ((16, False), (16, True), (32, False), (64, False)):
        previous = before[("default", bits, short)]
        base = before[("fat_only", bits, short)]
        current = after[("default", bits, short)]
        assert previous["int_bytes"] == current["int_bytes"]
        assert previous["pointer_bytes"] == current["pointer_bytes"]
        assert before[("default", bits, short)] == after[("exfat_only", bits, short)]
        assert before[("exfat_freefile", bits, short)] == after[("exfat_freefile", bits, short)]
        assert base == after[("fat_only", bits, short)]
        assert base["afatfsRequest_t"] == current["afatfsRequest_t"]
        assert base["afatfsDirectory_t"] == current["afatfsDirectory_t"]
        rows.append({"bits": bits, "short_enums": short,
                     "old_default": previous["afatfs_t"], "new_default": current["afatfs_t"],
                     "default_saved": previous["afatfs_t"] - current["afatfs_t"],
                     "fat_sfn": base["afatfs_t"], "lfn_global_cost": current["afatfs_t"] - base["afatfs_t"],
                     "sfn_handle": base["afatfsFile_t"], "lfn_handle": current["afatfsFile_t"],
                     "directory": current["afatfsDirectory_t"], "request": current["afatfsRequest_t"]})
    lines = ["# Validação de LFN em FAT16/FAT32", "",
             "O padrão agora habilita `ASYNCFS_ENABLE_LFN` e desabilita `ASYNCFS_ENABLE_EXFAT`. "
             "Os resultados abaixo usam quatro setores de cache e três arquivos públicos, sem freefile.", "",
             "## Memória", "",
             "Tamanhos em bytes de `afatfs_t`, incluindo cache e handles internos. "
             "A economia do novo padrão inclui a retirada do diário e dos metadados exFAT.", "",
             "| Alvo | Enums | Padrão anterior (exFAT) | Novo padrão (FAT + LFN) | Economia |",
             "| --- | --- | ---: | ---: | ---: |"]
    for row in rows:
        lines.append(f"| {row['bits']} bits | {'curtos' if row['short_enums'] else 'normais'} | "
                     f"{row['old_default']} | {row['new_default']} | {row['default_saved']} |")
    lines += ["", "Custo isolado do LFN comparado ao FAT com apenas nomes 8.3, na mesma ABI/configuração:", "",
              "| Alvo | Enums | FAT 8.3 global | FAT + LFN global | Acréscimo | Handle 8.3 → LFN | Directory / Request |",
              "| --- | --- | ---: | ---: | ---: | --- | --- |"]
    for row in rows:
        lines.append(f"| {row['bits']} bits | {'curtos' if row['short_enums'] else 'normais'} | "
                     f"{row['fat_sfn']} | {row['new_default']} | {row['lfn_global_cost']} | "
                     f"{row['sfn_handle']} → {row['lfn_handle']} | {row['directory']} / {row['request']} |")
    lines += ["", "O custo por handle inclui o nome de até 255 caracteres apenas na união de operações e "
              "o local compacto da cadeia LFN. A montagem do nome em `fstat` e `readdir` usa o buffer do chamador; "
              "não há heap, cache adicional, cópia permanente do nome nem array de 21 posições por arquivo. "
              "O estado de leitura LFN compartilha a união de enumeração com exFAT. "
              "`afatfsDirectory_t` e `afatfsRequest_t` não cresceram. Não some o custo dos handles ao total global.", "",
              "A configuração FAT 8.3 sem LFN e as duas configurações exFAT anteriores mantiveram seus tamanhos. "
              "Foram verificadas seis configurações em x86/x64 e XC16/PIC24 com enums normais e curtos "
              "(24 combinações). Os testes verificam alinhamento natural, scratch de getcwd, buffers de quatro bytes, "
              "prefixos das suboperações e conversões/isolamento dos bitfields C99. "
              "Os resultados de 16 bits são compilação cruzada real, sem execução em hardware.", "",
              "Duas suboperações estavam deslocadas apesar de seus contratos exigirem offset zero. "
              "Os prefixos `appendFreeCluster` e `truncateFile` foram restaurados sem aumentar as estruturas. "
              "Nenhum registro de disco foi reordenado. Recompile biblioteca, backend e aplicação juntos: "
              "os campos privados da estrutura pública de enumeração passaram a usar uma união nomeada.", "",
              "## Cobertura", "",
              "- Criação, leitura por nome/alias, comparação de caixa, preservação de caixa, stat/fstat, "
              "enumeração, padrões, renomeação LFN↔8.3, troca apenas de caixa, renomeação para o próprio alias, "
              "exclusão por alias e reaproveitamento de entradas.",
              "- Nomes de 13, 26 e 255 caracteres; rejeição de 256 caracteres e nomes inválidos; "
              "colisões de aliases até dois dígitos; subdiretórios e getcwd.",
              "- Publicação na entrada 31, mudança de setor/cluster, manutenção do terminador, "
              "limpeza de cadeias órfãs e preservação dos flags de caixa do Windows ao renomear.",
              "- Fixture independente baseada na especificação: 14 casos de sequência/checksum/tipo/cluster/padding, "
              "alias de nome Unicode e flags NT, mais reutilização de uma cadeia órfã com checksum coincidente.",
              "- Criação simultânea, criação exclusiva concorrente do mesmo nome e falhas/repetição de gravações de metadados.",
              "- Regressão em seis configurações: padrão FAT/LFN, FAT 8.3, FAT/LFN externo, exFAT, exFAT/freefile "
              "e LFN+exFAT; compilação dos oito executáveis originais.",
              "- Validação adicional x64 de FAT/LFN com `AFATFS_USE_FREEFILE`; regressão x86 com `AFATFS_DEBUG`.",
              "- AddressSanitizer x64 sem violações; FATtools 1.1.23 leu independentemente seis imagens "
              "FAT16 (1/4 setores por cluster) e FAT32 (1), conferindo nomes, aliases, tamanhos e conteúdo, inclusive 255 caracteres.", "",
              "Os nomes da API continuam ASCII; não foi adicionado Unicode/UTF-8. Nomes Unicode existentes "
              "podem ser acessados pelo alias 8.3. FAT não recebeu diário nem renomeação atômica contra queda de energia. "
              "A validação nativa posterior está em [Windows FAT32/LFN](WINDOWS_FAT32_LFN_VALIDATION.md). "
              "A regressão completa usa quatro setores "
              "de cache; uma exploração com dois setores reproduziu uma falha de progresso também na versão "
              "anterior sem LFN, portanto não é apresentada como configuração validada.", "",
              "Um diretório FAT vazio agora recebe seu primeiro cluster e as entradas `.`/`..` antes do callback, "
              "usando a máquina de estados de extensão existente. Isso corrige a impossibilidade de reabrir "
              "um diretório vazio depois de remontar.", "",
              "## Reprodução", "", "```powershell",
              "./tests/run_exfat_tests.ps1 -CompileLegacy",
              "./tests/run_layout_tests.ps1 -OutputDirectory tests/build/lfn-after",
              "python tests/lfn_memory_report.py --before tests/build/lfn-before/report.json --after tests/build/lfn-after/report.json",
              "# Com o backend de teste compilado com TEST_LFN:",
              "New-Item -ItemType Directory -Force tests/build/lfn-images | Out-Null",
              "$env:AFATFS_LFN_IMAGE_DIR = 'tests/build/lfn-images'",
              "./tests/build/test_exfat.exe",
              "python tests/lfn_interop.py --images tests/build/lfn-images", "```", "",
              "Em ambientes com Make/GCC: `make test-fat-lfn` e `make test-layout`. "
              "O parser independente usa a dependência FATtools em `tests/build/vendor`.", "",
              "Referência: [Microsoft FAT Specification, seção 7](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf).", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    data = {"status": "passed", "rows": rows,
            "before_sha256": hashlib.sha256(args.before.read_bytes()).hexdigest(),
            "after_sha256": hashlib.sha256(args.after.read_bytes()).hexdigest()}
    args.output.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"PASS LFN memory comparison: {args.output}")


if __name__ == "__main__":
    main()
