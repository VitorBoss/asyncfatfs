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
| fsck.exfat on generated images | Incomplete: the first exFAT image was reported clean, but the tool exited with access violation inside the sandbox; execution outside it was not authorized |

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

## Reproduction

```powershell
./tests/run_exfat_tests.ps1 -CompileLegacy

# Emit independent-reader fixtures while running the same matrix:
New-Item -ItemType Directory -Force tests/build/api-images
$env:AFATFS_API_IMAGE_DIR = 'tests/build/api-images'
./tests/run_exfat_tests.ps1
Remove-Item Env:AFATFS_API_IMAGE_DIR
python tests/api_interop.py

# Optional, where the supplied tool can run successfully:
python tests/api_interop.py --exfatprogs tests/build/external-tools/exfatprogs-1.3.2-1.exe
```

`api_interop.py` requires the FATtools dependency described in
`EXFAT_INTEROP.md`. It writes `tests/build/api-images/verification.json`.
Generated binaries, logs and images stay under the ignored `tests/build/`.
The validation does not establish equivalence for unsupported Unicode, 64-bit
files, multiple volumes, full-path resolution or cross-directory moves.
