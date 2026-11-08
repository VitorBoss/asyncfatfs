# External exFAT interoperability

Validated on 2026-11-06 using MSVC 14.51.36231, FATtools 1.1.23 and exfatprogs 1.3.2
(the unpins Windows port, release `v1.3.2-1`). Images are regular files, not mounted
devices. The library's existing cache, state machines, static allocation and backend API
are used without substituting any filesystem operation.

## Matrix

| Formatter | Cluster size | Layout | x64 | x86 | x64 AddressSanitizer |
| --- | ---: | --- | --- | --- | --- |
| mkfs.exfat | 512 B | Default | Passed | Passed | Passed |
| mkfs.exfat | 4 KiB | Default | Passed | Passed | Passed |
| mkfs.exfat | 32 KiB | Default | Passed | Passed | Passed |
| mkfs.exfat | 64 KiB | Default | Passed | Passed | Passed |
| mkfs.exfat | 4 KiB | `--pack-bitmap` | Passed | Passed | Passed |
| FATtools exfat_mkfs | 4 KiB | FATtools layout and compressed up-case table | Passed | Passed | Passed |
| FATtools exfat_mkfs | 64 KiB | FATtools layout and compressed up-case table | Passed | Passed | Passed |

Each configuration runs all seven layouts. For each layout, `fsck.exfat -n` must return
zero and explicitly report a clean volume at four checkpoints: after external seeding,
after library writes, after external writes, and after the final library read/remount.
No repairs are requested. This produces 21 completed cases and 84 clean checks across
the three configurations. All three C binaries compile with `/W3 /WX`.

The formatters create a raw volume. The test copies its bytes, unchanged, into an MBR
container at LBA 2048 for the original MBR matrix. It extracts the partition bytes
again for fsck. Both unpartitioned and partition-offset-aware boot headers are exercised;
no filesystem header or checksum is patched to make the library accept an image.

## Workload and independent checks

FATtools seeds files before the library mounts: a `NoFatChain` file, a genuinely
fragmented FAT-chain file, an empty file, files for truncation/deletion, and a contiguous
subdirectory containing data. The C harness then reads the external data, appends beyond
the original allocation, truncates and deletes external files, and creates its own empty
file, 255-character name, patterned data and subdirectory. Both the existing external
directory and the new directory grow beyond their original cluster allocation.

All data is checked after an AsyncFatFS unmount/remount, then independently parsed and
read byte for byte by FATtools. FATtools subsequently appends to a library-created file
and creates another file; AsyncFatFS remounts and verifies both. The 64-KiB cases use
128-MiB volumes so the directory-growth workload fits; other cases use 64-MiB volumes.

No new filesystem incompatibility was found in this matrix, so it required no change
to the library core. Changes are confined to the external regression harness, build
integration and documentation. [Native Windows driver checks](WINDOWS_VHD_VALIDATION.md)
are covered separately, as is [power-failure recovery](EXFAT_POWERCUT.md).
Native macOS checks, physical hardware and unsupported features such as Unicode API names and 64-bit addressing remain
outside this validation.

## GPT validation (2026-11-07)

The same workload also passed on GPT disks partitioned by FATtools 1.1.23, with exFAT
formatted independently by mkfs.exfat (512-byte clusters) and FATtools (4-KiB clusters).
Both cases passed four clean `fsck.exfat -n` checkpoints, independent byte-for-byte
verification and an external-write/library-remount roundtrip. The source and binary
hashes and eight fsck results are recorded in `tests/build/external-gpt/report.json`.
This run used the x64 MSVC binary; it does not extend the historical x86/ASan matrix to GPT.

`tests/test_exfat.c` additionally covers 12 GPT layouts/recovery cases, 13 malformed
layouts, read-only attributes (clean and dirty), failed reads and removal/reinsertion
during discovery. It checks that library writes leave the primary/backup GPT unchanged.
GPT scratch state shares the existing up-case-table memory; no extra sector buffer is used.
MSVC layout probes confirmed unchanged filesystem-state size: 9,896 bytes on x64 and
9,780 bytes on x86 in the default configuration (before/after delta: zero).

## Reproduce on Windows

Install Python and Visual Studio C/C++ build tools. From the repository root:

```powershell
python -m pip install --target tests/build/vendor -r tests/requirements-exfat.txt
```

Download the pinned [Windows exfatprogs release](https://github.com/unpins/exfatprogs/releases/tag/v1.3.2-1)
asset `exfatprogs-1.3.2-1-x86_64-windows.exe.zst` into `tests/build`, verify its SHA-256,
then extract it with 7-Zip into `tests/build/external-tools` and name the executable
`exfatprogs-1.3.2-1.exe`. The compressed asset SHA-256 is:

```text
3b816f2ee58c89b30e07dea420c8305de8747048a99f06a84928a2c68edb325d
```

```powershell
Get-FileHash tests/build/exfatprogs-1.3.2-1.exe.zst -Algorithm SHA256
7z x tests/build/exfatprogs-1.3.2-1.exe.zst -otests/build/external-tools
./tests/run_exfat_interop.ps1 -Python python
./tests/run_exfat_interop.ps1 -Python python -Architecture x86
./tests/run_exfat_interop.ps1 -Python python -AddressSanitizer
./tests/run_exfat_interop.ps1 -Python python -PartitionTable gpt
```

`-Python` may be an absolute interpreter path; `-Exfatprogs` may specify a different
location for the same multicall binary. The suite never downloads dependencies or
opens physical disks. It overwrites only its named test images under `tests/build`.
Images, binaries, logs and `report.json` files are ignored by Git. Reports record command
exit codes/output, tool versions and binary/source hashes. The runner defaults to
separate `external-x64`, `external-x86` and `external-x64_asan` output directories.

The first validation runs invoked `tests/exfat_interop.py` directly; their reports are
in `tests/build/external/report.json`, `tests/build/external-x86/report.json` and
`tests/build/external-asan/report.json`. The lower-level command also accepts `--case`
to select a layout and `--binary` to select a precompiled harness.

This Windows port of exfatprogs crashes on exit in the Codex restricted process sandbox,
even on a clean, unmodified volume. Outside that sandbox it returns the expected status
codes; all results above require successful process exits. A printed `clean` message
followed by a crash is a failure, not an accepted check.

FATtools 1.1.23 also requires care in the adapter: close its underlying disk cache
explicitly, seek to the logical data length before append (its `SEEK_END` uses allocation
length), and use its `Chain` reader for 255-character names. The adapter uses the
published `exfat_mkfs` API because that version's standalone `mkfat.py` parser has an
argument-default error. No third-party source is modified, and the independent fsck
checks still validate all fixture and library output.
