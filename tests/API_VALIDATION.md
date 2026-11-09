# Extended API validation

Validated on Windows with MSVC, using regular image files and a delayed,
fault-injecting in-memory backend. No physical card or disk is accessed.

## Coverage and results

| Check | Result |
| --- | --- |
| x64 default exFAT, FAT-only and exFAT + freefile configurations, `/W3 /WX` | Passed |
| x86 default configuration | Passed |
| x64 AddressSanitizer | Passed, no reported memory violations |
| Caller scratch reduced from 256 to 32 bytes | Passed |
| Eight original image-based executables | Compiled |
| Original file-modes and file-deletion workloads on independent FAT16 image copies | Passed |
| FATtools independent verification of library-formatted FAT16/MBR, FAT32/MBR, exFAT/MBR and exFAT/GPT | Passed: 2,003-byte payload, size, modification timestamp and deleted entries |
| fsck.exfat 1.3.2 on library-generated exFAT/MBR and exFAT/GPT images | Passed: clean filesystems, exit code 0 for both |
| External-format interoperability matrix with the current library | Passed: seven layouts each in MBR and GPT; 56 fsck checks, all clean with exit code 0 |
| Native Windows driver, new APIs in fixed VHDs | Passed: MBR 4/64 KiB/default and GPT 4 KiB; cursor/fragmented truncation, rename, preallocation/growth, native allocation queries and 16 read-only CHKDSK checks |
| Power loss in truncate/rename/expand request APIs | Passed: 1,119 operation cuts and 224 interrupted undo-replay cuts, with clean fsck results; 72 pre-operation startup handoffs remained safely read-only with zero writes |
| C99 Boolean bit-fields and alignment | Passed: x86/x64 flag-combination tests and XC16 16-bit C99 layout assertions with normal/short enums; measured RAM decreased in all configurations. See [alignment report](BITFIELD_ALIGNMENT.md). |
| Runtime member ordering | Passed: aligned scratch and embedded sub-operation prefixes, unchanged disk records and lower global RAM across 12 target/configuration cases. See [before/after memory report](MEMORY_LAYOUT_REPORT.md). |

`tests/test_exfat.c` exercises cursor truncation down through cluster/sector
boundaries to zero, unchanged EOF, preserved prefixes and positions, FAT chains,
freefile mode and exFAT contiguous preallocation/VDL. It also checks archive/time
updates, exclusive creation, per-request errors/transferred counts, unified
metadata, rename of files and directories, read-only metadata, active directory
protection, nonempty directory rejection, wildcard enumeration, label updates,
root extension with nonzero unused storage, exact free-space counting, expand
hint/allocation, printf/puts/gets and forward consumer backpressure.

Formatting tests remount and exercise each generated filesystem. They check
MBR/GPT replacement, mounted-volume rejection, held I/O, submission/ACK failures,
and final power-off guards. FAT32 clean shutdown is tested with failed FSInfo
write acknowledgments: power-off stays blocked and both FSInfo hints become
unknown only after the successful retry. Existing regression coverage also
checks simultaneous file/cache/prefetch/journal activity, interrupted writes,
media replacement and stale callbacks.

Extended recovery tests fail reads and rename writes, remove/reinsert the card
while rename publication is pending, tear the interrupted sector and replay a
retired callback. The journal restores the original file and rejects the
uncommitted name. Separate tests preserve benign exFAT secondary entries,
NoFatChain and valid-data length during rename, and reject entry sets exceeding
the existing 19-entry limit before any write.

The original deletion test now waits for outstanding asynchronous unlinks before
starting its next scenario; otherwise it reused names/the shared contiguous
writer while the previous scenario still held them.

## Completed fsck validation (2026-11-09)

The previously blocked fsck check completed outside the restricted process
sandbox. Both the existing fixtures and newly generated fixtures passed. The
current source was rebuilt with `/W3 /WX`; the default, FAT-only and exFAT +
freefile regression matrix passed. Fresh generated images are under
`tests/build/api-images-current/`, with a successful `verification.json` recording
source/image/tool hashes, FATtools version, fsck output and exit codes.

The external matrix also completed with the rebuilt library: exfatprogs cluster
sizes 512, 4096, 32768 and 65536 bytes, packed bitmap at 4096 bytes, and FATtools
formats at 4096 and 65536 bytes. Each layout was tested in both MBR and GPT.
For every case, fsck ran on the external baseline, after library writes, after
FATtools wrote back to the volume, and after the library remounted and verified
the round trip. All 56 checks passed. Together with the two fresh
library-generated exFAT images, this gives 58 successful fsck checkpoints.
Reports: `tests/build/fsck-current-mbr/report.json` and
`tests/build/fsck-current-gpt/report.json`.

No filesystem incompatibility was found, so no library implementation or RAM
allocation changed. The validation harness now saves an incomplete report when
a check fails, replacing any older success report; a printed clean message
followed by a nonzero process exit still fails validation. The Windows tool's
sandbox access violation remains an execution-environment limitation, not an
accepted fsck result.

## Reproduction

```powershell
./tests/run_exfat_tests.ps1 -CompileLegacy

# Emit independent-reader fixtures while running the same matrix:
New-Item -ItemType Directory -Force tests/build/api-images
$env:AFATFS_API_IMAGE_DIR = 'tests/build/api-images'
./tests/run_exfat_tests.ps1
Remove-Item Env:AFATFS_API_IMAGE_DIR
python tests/api_interop.py

# Complete check, in an environment where exfatprogs exits normally:
python tests/api_interop.py --exfatprogs tests/build/external-tools/exfatprogs-1.3.2-1.exe
```

`api_interop.py` requires the FATtools dependency described in
`EXFAT_INTEROP.md`. It writes `tests/build/api-images/verification.json`.
Generated binaries, logs and images stay under the ignored `tests/build/`.
The validation does not establish equivalence for unsupported Unicode, 64-bit
files, multiple volumes, full-path resolution or cross-directory moves.
