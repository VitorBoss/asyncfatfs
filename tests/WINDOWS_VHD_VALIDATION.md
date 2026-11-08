# Native Windows exFAT validation

FAT32/LFN results are recorded separately in [native FAT32 validation](WINDOWS_FAT32_LFN_VALIDATION.md).

Validated on 2026-11-06 using the native Windows storage stack, .NET file I/O,
`Format-Volume` and `chkdsk.exe`. The OS reported `Microsoft Windows NT 10.0.26300.0`;
the installed exFAT driver version was `10.0.26100.9711`.

## Extended API validation (2026-11-09)

The new request APIs were tested against the same native Windows driver on fresh
256-MiB fixed VHDs. Windows created and formatted each volume; the library touched
raw image bytes only while the VHD was detached. The x64 workload compiled with
MSVC `/W3 /WX` and used the original simulated asynchronous backend.

| Partition table | Format request | Actual cluster size | Extended API / native round trip | Read-only CHKDSK |
| --- | --- | ---: | --- | --- |
| MBR | 4096 | 4 KiB | Passed | 4 checks, exit code 0 |
| MBR | 65536 | 64 KiB | Passed | 4 checks, exit code 0 |
| MBR | Windows default | 4 KiB | Passed | 4 checks, exit code 0 |
| GPT | 4096 | 4 KiB | Passed | 4 checks, exit code 0 |

The workload `tests/exfat_native_api.inc`, included by the existing C harness,
exercises the result-bearing open/read/write/sync/close APIs and:

- Truncation at EOF, at the cursor inside a cluster, and to zero, preserving the
  prefix and cursor. A Windows-created contiguous file and an independently
  verified fragmented FAT chain are truncated. Windows checks lengths, prefixes
  and cluster-rounded allocation, including zero allocation after emptying.
- File and nonempty-directory rename, preserved child data, missing old names,
  collision rejection and missing-source errors. Windows renames a library-renamed
  file again and truncates a library-truncated file; the library verifies both.
- Contiguous allocation hint without allocation, real preallocation of four
  clusters with VDL zero, and partial write followed by truncation of a
  preallocated NoFatChain file. Exact free-cluster changes are checked through
  `getfree`. Windows reads unwritten contents as zeros and checks allocated size.
- Windows writes into the middle of the preallocated file, leaving a zero gap,
  then appends beyond the original allocation. Native allocation grows to five
  clusters; after detachment, the library verifies every zero and written byte.
- Unified metadata, packed modification time, hidden attribute, filtered directory
  enumeration, label/current-directory queries, unlink, printf/puts output and
  gets input after a native text append.

Native allocation checks use
[`GetFileInformationByHandleEx` / `FILE_STANDARD_INFO`](https://learn.microsoft.com/windows/win32/api/winbase/ns-winbase-file_standard_info)
to obtain `AllocationSize` and `EndOfFile` independently of library metadata.
CHKDSK runs without repair flags at the external baseline, after library writes,
after native writes and after the library's final verification: **16 successful
checks** in the final matrix. VHD footer hashes remain unchanged.

No filesystem incompatibility was found. No library code, cache/state-machine
structure or embedded RAM allocation changed. Changes are confined to the test
workload, runner, build dependencies and documentation. The runner now accepts
GPT, records source hashes and isolates compiler object files per run; its quoted
MSVC output-directory argument was corrected before native testing. The first
compile-only attempt did not create or mount any VHD.

Final reports, with native allocation values, command outputs, driver version,
source/binary hashes and cleanup results:

```text
tests/build/windows-native/run-api-20261109-final-mbr/report.json
tests/build/windows-native/run-api-20261109-final-gpt/report.json
```

The preliminary matrix also passed; the final runs add fragmented truncation and
growth beyond preallocation. All test VHDs were detached, temporary access paths
removed, and cleanup completed without errors. Only the verified VHD partitions
were initialized/formatted; no physical disk was modified.

```powershell
./tests/run_windows_vhd_tests.ps1
./tests/run_windows_vhd_tests.ps1 -Cases 4096 -PartitionTable GPT
```

An administrator worker is required by the native VHD APIs. The existing image
identity, boot/system-disk, size and bus checks apply to both partition schemes.

## Results

| Native format request | Actual cluster size | VHD size | Library/Windows roundtrip | Read-only CHKDSK checks |
| --- | ---: | ---: | --- | --- |
| Explicit 4096 | 4 KiB | 256 MiB | Passed | 4 checks, all exit code 0 |
| Explicit 65536 | 64 KiB | 256 MiB | Passed | 4 checks, all exit code 0 |
| Windows default | 4 KiB | 256 MiB | Passed | 4 checks, all exit code 0 |

No new library incompatibility was found. No filesystem core changes were needed for
this validation; the existing asynchronous cache, state machines and storage backend
were used. The test workload compiled as x64 with MSVC `/W3 /WX`.

The complete successful run, including native command output, driver version, VHD
footer hashes and workload binary hash, is recorded in:

```text
tests/build/windows-native/run-20261106-225904-620c6483/report.json
```

All three VHDs were detached at the end, temporary mount points were removed, and
the run reported no cleanup errors. No physical disk was initialized or formatted.

## What ran

DiskPart created fresh **fixed-size** VHDs. Each was attached through
`Mount-DiskImage`, initialized as MBR and formatted as exFAT by `Format-Volume`.
Windows then created the seed files and directory through native file I/O. The library
was not used to format or seed these volumes, and no FATtools/exfatprogs operation
was involved in the native checks.

The VHD was detached before running the C workload on its raw disk bytes. A fixed
VHD contains those bytes followed by a 512-byte footer, allowing the existing file-based
SD-card simulator to operate without a new backend. The runner verified the fixed-VHD
cookie, type, size and footer SHA-256 before/after library access.

The C workload verified Windows-created file data, then performed append, truncation,
deletion, empty-file creation, a 255-character filename, and directory growth beyond
the original cluster. It exercised both an existing Windows directory and a new
library directory, and checked the data again after an unmount/remount.

Windows subsequently reattached the same VHD. Its native driver read the modified
files and every generated child file byte for byte, including the long filename and
empty files. Windows appended to a library-created file and created another file;
the detached library workload then verified those changes. Native CHKDSK ran at the
baseline, after library writes, after Windows writes, and after the final library read.

CHKDSK ran **without `/f`, `/r`, `/x` or repair flags**. All twelve executions returned
0 and reported no filesystem problems. On these small VHDs it also printed a warning
that shadow-copy storage was unavailable, then completed the read-only filesystem
check. No workload wrote to the volume during a checkpoint; passing was based on
the successful check and exit status, not on an attempted repair.

## Reproduce

Requirements: Windows, PowerShell 7.2+, and Visual Studio C/C++ build tools.
No Python packages or external filesystem tools are required.

```powershell
./tests/run_windows_vhd_tests.ps1
```

This compiles the existing C interoperability workload, then requests a Windows UAC
elevation if necessary. Microsoft requires administrator privileges to
[mount a VHD](https://learn.microsoft.com/powershell/module/storage/mount-diskimage).
The elevated worker runs hidden and writes an incremental `report.json` under a new,
uniquely named `tests/build/windows-native/run-*` directory. The default invocation
waits for completion; `-NoWait` returns the worker PID and report path immediately.

Use `-Cases 4096`, `-Cases 65536` or `-Cases default` for a single case. Existing run
directories and VHDs are never overwritten. The runner identifies each disk from its
specific image path and refuses boot/system disks, unexpected sizes or bus types.
Formatting uses only the partition returned from that verified virtual disk.

Native file I/O uses the VHD's verified volume GUID. CHKDSK on this system required
an additional actual mount point; the runner creates one temporarily inside the run
directory and removes it after each check. It does not assign drive letters, open an
Explorer window, change automount settings, or install services. Cleanup only detaches
the explicitly created test images.

The initial attempt to run CHKDSK against a GUID-only volume path without a directory
mount point was rejected with exit code 3. That run was marked failed and cleaned up; the mount-point adapter
was corrected before the successful full run. Failed checks are never counted as passes.

This validates the native Windows exFAT driver on VHD-backed media. Physical SD-card
behavior, power interruption and native macOS driver interoperability remain outside
this test coverage.
