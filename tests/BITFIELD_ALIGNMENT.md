# C99 Boolean bit-fields and 16-bit alignment

Validated on 2026-11-09 with MSVC on x86/x64 and Microchip XC16 v2.10 targeting
PIC24FJ256GA702, with `-std=c99`. Both normal and short enums were checked on XC16.

Runtime structures containing multiple Boolean members now group them into
adjacent `bool member : 1` declarations, one member per line. This covers the
directory cursor owner, request, journal, create/truncate operations, file handle,
GPT state and global controller. Single-Boolean structures retain their ordinary
`bool` member. Conditional FAT-only definitions retain ordinary single Booleans
when exFAT flags are absent. Boolean members of the delayed test backends are
packed in the same way.

The fields use `bool` from `<stdbool.h>` (`_Bool` in C99), preserving conversion
of every nonzero integer to true. They do not use signed one-bit integers or
implementation-specific `uint8_t` bit-field bases. C99 permits `_Bool` bit-fields
but leaves allocation order and storage-unit alignment to the implementation;
therefore no raw masks, casts or serialization depend on their physical layout.
See [C99, sections 6.7.2.1 and 7.16](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf).

## Alignment and compatibility

Internal structures retain natural compiler alignment; no `packed` attribute or
`#pragma pack(1)` is applied to them. The existing four-byte sector-buffer
alignment is also applied to the journal's I/O, before-image and header buffers.
An explicit word-alignment check initially failed with XC16 `-fshort-enums`;
adding these annotations corrected it. It now passes with both enum modes.

C99 compile-time assertions check the cache, sector buffers, pointer members,
request values, file-operation union and journal state. Additional generated
checks compiled every measured addressable member of the private/public runtime
records for natural alignment on the 16-bit target. The target also asserts
`sizeof(int) == 2`; this is an actual 16-bit ABI, not a PC build with packing
options. These are cross-compilation/layout checks, not execution on PIC hardware.

FAT/exFAT disk records retain their original packed format and offsets. Their
sizes, alignments and measured field offsets matched the pre-change baseline on
XC16; the host probe also checks critical entry sizes and offsets. The journal
is still encoded field by field into its original 512-byte records.

Public workspace structures have changed ABI. Rebuild the library and its
consumers together. Bit-field values can be read, assigned and copied as part of
their containing structure, but cannot be addressed with `&` or passed directly
to `sizeof`. The existing serialized/main-loop calling contract remains in effect.

## Measured RAM

This table records the Boolean-packing step. The later member-reordering step is
reported separately in [the current memory comparison](MEMORY_LAYOUT_REPORT.md).
Its savings are additional to the numbers below.

Bytes occupied by the global `afatfs_t` instance, using four cache sectors and
three public handles. These include alignment padding; no tested configuration
grew. Caller-owned request workspace also shrank by two bytes on XC16 and four
bytes on x86; its x64 size stayed unchanged.

| Configuration | XC16 before → after | x86 before → after | x64 before → after |
| --- | ---: | ---: | ---: |
| Default exFAT | 9420 → 9384 | 9760 → 9720 | 9896 → 9880 |
| FAT-only | 2536 → 2524 | 2660 → 2640 | 2776 → 2744 |
| exFAT + freefile | 10488 → 10452 | 10892 → 10848 | 11056 → 11040 |

Padding can prevent a particular structure from shrinking even when its flags
use fewer bits. Measurements are specific to these compiler ABIs and settings.

## Tests and reproduction

```powershell
./tests/run_layout_tests.ps1
./tests/run_exfat_tests.ps1 -CompileLegacy
```

`run_layout_tests.ps1` compiles and runs `bitfield_layout.c` on x86 and x64 for
the three configurations. It checks every combination of flags in each affected
runtime record, using 2 and `0x80000000` as nonzero source values. This verifies
Boolean conversion and isolation of neighboring fields. Its compile-time checks
use C99 typedef assertions rather than C11 `_Static_assert`/`_Alignof`.

When XC16 is available, the runner compiles the complete probe/library in C99
mode for all three configurations, with and without `-fshort-enums`. Override
`-Xc16Compiler` to point to another installation. Read `xc16Available` in the JSON
report when running on machines without that toolchain. XC16 logs retain existing
baseline switch/signedness/range warnings; host probes compile with `/W3 /WX`.

All six host probes and six XC16 cross-compiles passed. The default, FAT-only
and exFAT/freefile functional regressions passed, including pending cache/journal
I/O and recovery. Eight legacy tests and the external/power-cut harnesses compiled.
The default functional suite also passed AddressSanitizer. No heap, additional
controller state or changed on-disk journal format was introduced.

Evidence under ignored `tests/build/`:

```text
bitfield-tests/report.json
bitfield-layout/comparison.json
bitfield-layout/*-before.s
bitfield-layout/*-after.s
bitfield-final-regression.log
bitfield-asan/result.log
bitfield-harness/build.log
```
