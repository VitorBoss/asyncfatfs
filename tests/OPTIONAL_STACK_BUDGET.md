# PIC24 optional-feature stack budget

XC16 2.10, PIC24FJ256GA702, C99, `-Os`. Values include internal direct calls and return PCs. Backend/application callbacks and compiler/libc helpers are excluded.

Peak across normal/short enums and single-block, multiblock and read-session-query profiles:

| Configuration | Before (bytes) | After (bytes) | Static RAM |
|---|---:|---:|---|
| default | 328 | 286 | unchanged |
| fat_legacy | 324 | 282 | unchanged |
| fat_only | 330 | 306 | unchanged |
| fat_lfn | 406 | 338 | unchanged |
| exfat_only | 444 | 368 | unchanged |
| exfat_freefile | 474 | 398 | unchanged |
| lfn_exfat | 462 | 386 | unchanged |
| lfn_no_journal | 390 | 326 | unchanged |

All 48 variants are strictly below 400 bytes. Writable section sizes (including alignment and the DMA cache) match the baseline in every variant. No heap or additional operation-state fields were introduced.

Directory expansion now returns `AFATFS_OPERATION_IN_PROGRESS` after preparing its existing state; polling performs allocation and completion. This releases create/rename/mount stack frames before nested FAT/cache/journal calls. Inlining the allocation dispatcher removes another wrapper from the deepest freefile path. Flushes, journal transitions, mirror FAT updates and completion ordering inside the continuation are unchanged.

Validation: ten existing synthetic functional scenarios, including backend failures, journal recovery, delayed ACKs and clean power-off; the added deferred-directory regression verifies no inline I/O, busy rejection, polling, zero-fill and remount. The stack-parser regression covers both fixed-size XC16 SP adjustment spellings and rejects variable growth.

The user was editing `sdcard.h` concurrently. Tests used fixed library snapshots with the same header on both sides; no CID layout change was required by this optimization. Stack reduction consists of the directory deferral and inlined allocation dispatcher.

Reproduce with:

```text
python tests/optional_stack_budget.py --library lib --baseline <baseline-lib> --output tests/build/optional-stack-check
python tests/test_pic24_stack_budget.py
```

The JSON report retains per-case call chains, source hashes and writable section sizes. Temporary evidence for this run is under `tests/build/optional-stack/`.

The subsequent feature-section cleanup grouped compatibility lifecycle checks,
exFAT truncation ownership, journal buffering eligibility, capture-epoch reset and
FAT journal write protection. Conditional groups decreased from 264 to 256 while
preserving AsyncFS → LFN → exFAT → journal. All 48 stack bounds and writable-section
sizes matched the immediate baseline; the peak remains 398 bytes. All 11 functional
scenarios passed, including journal CRC failure, delayed acknowledgements, clean
power-off, recovery and deferred-directory growth. The six native PIC24 instruction
comparisons also matched after normalizing build time and compiler-generated labels
and private symbol identifiers. Evidence is under `tests/build/section-hooks-2/`.

The next cleanup moved LFN deletion predicates and exFAT entry-set publication,
written-data tracking, post-checkpoint bookkeeping and stat adjustments into their
feature sections. Conditional groups decreased from 256 to 250. All 48 internal
stack bounds and writable section sizes again matched the immediate baseline, with
a maximum of 398 bytes. All 11 functional scenarios and the six normalized native
PIC24 instruction comparisons passed. The call order around short-entry deletion,
LFN cleanup, cached data, metadata publication and journal checkpoints is unchanged.
Evidence is under `tests/build/section-hooks-3/`.

Partition recognition, exFAT boot geometry checks, GPT phase dispatch and root-mount
progress were then grouped in the exFAT section. Conditional groups decreased from
250 to 243. All 48 stack bounds and writable section sizes matched the immediate
baseline; the peak remains 398 bytes. All 11 functional scenarios passed, including
12 GPT layout/recovery cases, 13 corruptions, dirty-volume and read-only protections.
The six normalized native PIC24 instruction comparisons also matched. The existing
partition candidate order, geometry rejection checks, per-pass GPT progress and
mount error transitions were preserved. Evidence is under `tests/build/section-hooks-4/`.

Truncate-range setup, NoFatChain classification and pre-publication data-length
bookkeeping were subsequently grouped in exFAT hooks. Conditional groups decreased
from 243 to 239. The bookkeeping retains its original behavior for FAT volumes in
builds with exFAT enabled. All 48 internal stack bounds and writable section sizes
matched the immediate baseline, with a peak of 398 bytes. All 11 functional scenarios
and six normalized native PIC24 instruction comparisons passed. Evidence is under
`tests/build/section-hooks-5/`.
