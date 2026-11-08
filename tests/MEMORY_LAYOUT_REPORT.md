# Runtime structure memory comparison

Baseline: runtime structures with Boolean bit-fields, before member reordering. All sizes are bytes.

The global `afatfs_t` instance includes its cache, handles and journal. Component sizes below must not be added again to this total.

| Configuration | Target | Enums | Before | After | Saved |
| --- | --- | --- | ---: | ---: | ---: |
| default | 16-bit | normal | 9384 | 9380 | 4 |
| default | 16-bit | short | 9380 | 9376 | 4 |
| default | 32-bit | normal | 9720 | 9692 | 28 |
| default | 64-bit | normal | 9880 | 9824 | 56 |
| exfat_freefile | 16-bit | normal | 10452 | 10448 | 4 |
| exfat_freefile | 16-bit | short | 10452 | 10444 | 8 |
| exfat_freefile | 32-bit | normal | 10848 | 10816 | 32 |
| exfat_freefile | 64-bit | normal | 11040 | 10968 | 72 |
| fat_only | 16-bit | normal | 2524 | 2520 | 4 |
| fat_only | 16-bit | short | 2512 | 2500 | 12 |
| fat_only | 32-bit | normal | 2640 | 2636 | 4 |
| fat_only | 64-bit | normal | 2744 | 2736 | 8 |

## Per-instance components, default configuration

These rows use normal enums. Request/directory workspaces are caller-owned; their instance count belongs to the application.

The backend-owned `sdcardMetadata_t` also shrank from 24 to 22 bytes on XC16/PIC24; it remains 24 bytes on x86/x64. It is outside `afatfs_t`, so the global totals exclude this additional per-instance saving.

| Structure | Target | Before | After | Saved |
| --- | --- | ---: | ---: | ---: |
| `afatfs_t` | 16-bit | 9384 | 9380 | 4 |
| `afatfsFile_t` | 16-bit | 1050 | 1050 | 0 |
| `afatfsRequest_t` | 16-bit | 680 | 680 | 0 |
| `afatfsDirectory_t` | 16-bit | 46 | 46 | 0 |
| `afatfsTruncateFile_t` | 16-bit | 40 | 40 | 0 |
| `afatfsJournal_t` | 16-bit | 1604 | 1604 | 0 |
| `afatfs_t` | 32-bit | 9720 | 9692 | 28 |
| `afatfsFile_t` | 32-bit | 1108 | 1104 | 4 |
| `afatfsRequest_t` | 32-bit | 708 | 708 | 0 |
| `afatfsDirectory_t` | 32-bit | 52 | 52 | 0 |
| `afatfsTruncateFile_t` | 32-bit | 48 | 48 | 0 |
| `afatfsJournal_t` | 32-bit | 1608 | 1608 | 0 |
| `afatfs_t` | 64-bit | 9880 | 9824 | 56 |
| `afatfsFile_t` | 64-bit | 1136 | 1128 | 8 |
| `afatfsRequest_t` | 64-bit | 760 | 760 | 0 |
| `afatfsDirectory_t` | 64-bit | 64 | 64 | 0 |
| `afatfsTruncateFile_t` | 64-bit | 56 | 56 | 0 |
| `afatfsJournal_t` | 64-bit | 1608 | 1608 | 0 |

## Layout decisions and validation

Runtime members are grouped by required alignment. Word/pointer members precede byte flags and odd-byte tails where this removes gaps. An odd array element count does not imply an odd byte size; type alignment and total bytes determine padding.

Aligned cache/journal buffers retain four-byte alignment. The request scratch remains aligned for `afatfsPathFrame_t`. Embedded truncate/append sub-operations retain their mandatory first-member position. Disk records and Windows protocol structures retain their fixed field order.

All measured structures stayed the same size or shrank. Natural member alignment, four-byte sector-buffer alignment, scratch-frame alignment and embedded sub-operation prefixes passed the layout probes.

Do not sum nested component savings with the global-instance saving. Remaining tail padding may be required by natural alignment. No `packed` attribute, new controller storage, heap allocation or non-Boolean type narrowing was introduced.

These are ABI changes to memory structures: rebuild the library, backend and consumers together, and use named/designated fields rather than relying on their old offsets or positional aggregate order.

16-bit results are XC16/PIC24 C99 cross-compilation measurements, not execution on hardware. Host flag tests execute on x86 and x64. Exact padding depends on the compiler ABI and configuration.

## Reproduction

```powershell
# Capture this before applying a layout change:
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/reorder-before
# Apply the layout change, then measure again:
./tests/run_layout_tests.ps1 -OutputDirectory tests/build/reorder-after
python tests/layout_memory_report.py --before tests/build/reorder-before/report.json --after tests/build/reorder-after/report.json --output tests/MEMORY_LAYOUT_REPORT.md
./tests/run_exfat_tests.ps1 -CompileLegacy
```

The saved before report and XC16 constants were captured before reordering. Host sizes and flag tests execute on x86/x64; XC16 assembly contains compiler-evaluated `sizeof` constants. The comparison fails if an ABI differs, a measured structure grows or a journal buffer loses alignment.

Before report: `tests/build/reorder-before/report.json`
After report: `tests/build/reorder-after/report.json`
