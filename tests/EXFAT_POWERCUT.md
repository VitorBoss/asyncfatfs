# exFAT metadata journal and power-cut validation

The implementation uses the original asynchronous cache and operation machines.
`lib/asyncfatfs_journal.inc` is included by `asyncfatfs.c`; consumers still compile
the same library translation units. FAT16/FAT32 do not use the journal.

## Storage and recovery

The first clean mount preallocates a contiguous hidden/system/read-only file named
`AFATFS.JNL`. Its reserved name cannot be opened for writing or deleted through
the library. Provisioning zeroes the extent before publishing its allocation and
length. It does not consume a public file handle. The extent must remain in place
when other drivers use the volume.

The file contains two alternating CRC32-protected headers, then pairs of sectors:
a record descriptor and a complete metadata before-image. The header identifies
the volume geometry, journal extent, sequence, transaction epoch, ownership and
published record count. A before-image and descriptor become durable before their
record is published, and publication becomes durable before the cache may modify
the target. Cache epoch tags and an on-media descriptor scan suppress duplicate
records without allocating a volume-sized RAM index.

Capacity is `fatSectors + ceil(bitmapLength / 512) + 64` records. File size is
`(2 + 2 * capacity) * 512`, rounded up to a cluster. RAM consists of three journal
sector buffers, a private file handle, and state counters. No heap allocation or
blocking backend I/O is introduced.

Transactions commit after flushable data and metadata are durable and all metadata
operations reach a consistent boundary. `fsync`, exFAT close and truncate wait for
the commit. Freed clusters cannot be reused before their truncation commits.
On a dirty owned mount, recovery validates every record and before-image before
writing anything, restores sectors in reverse order, and only then clears the log.
An interruption during replay leaves the original log available for another replay.

The journal is private to this library, not TexFAT or a Windows journal. After a
crash, recover with this library before another driver writes to the volume.
Never delete, truncate, defragment or modify `AFATFS.JNL` externally. Clean external
roundtrips are supported; a dirty external session is not automatically repaired.

## Executed checks

### Request APIs: truncate, rename and expand (2026-11-09)

The new `tests/exfat_api_powercut.py` matrix passed **1,119 operation cuts and
224 additional cuts during undo replay**, across nine scenarios at cluster sizes
512 and 4096 bytes. Every recovered image passed `fsck.exfat -n` with exit code 0
and an explicit clean result. No repair command was used.

| Scenario | Operation cuts, both cluster sizes | Replay cuts |
| --- | ---: | ---: |
| Cursor truncation, contiguous NoFatChain file | 158 | 32 |
| Cursor truncation, fragmented FAT chain | 164 | 32 |
| Cursor truncation, partly valid preallocated file | 158 | 32 |
| Truncation to zero | 182 | 32 |
| Truncation at EOF | 26 | 0 |
| Rename to a 255-character filename | 149 | 32 |
| Rename a nonempty directory | 92 | 32 |
| Contiguous preallocation (`allocate=true`) | 164 | 32 |
| Allocation hint (`allocate=false`) | 26 | 0 |

The fixtures are externally formatted by FATtools, with the journal already
provisioned, patterned unrelated files and a target entry at a sector boundary.
An unused data area contains nonzero bytes. The partly valid fixture has a valid
entry-set checksum and VDL smaller than its allocated/logical length; the
fragmented fixture is independently checked to have a FAT chain.

For every completed 512-byte write prefix, the next write is also cut after its
first or last 256 bytes. A fresh process mounts the image and replays the journal.
FATtools then compares the complete namespace, file contents, VDL, first cluster,
contiguous flag, attributes, creation time, unaffected modification times and
free-cluster count. Rename must expose exactly the original or renamed entry,
including directory children; partial namespace changes are rejected. Expand
must leave either the original empty file or the entire requested zero-valid-data
extent, without bitmap leaks. Truncate must preserve the exact retained prefix.

All recovered states matched the complete old or new state. **126 cuts at or
after the request-completion checkpoint retained the new state**. In every one
of the **224 interrupted undo replays**, recovery restored the old state and
returned to writable mode. Cuts include log records, before-images, publication,
metadata writes, commit and shutdown; repeated recovery is idempotent.

There were **72 protected startup handoffs**, all before the filesystem completed
mounting and before any API operation started. At these points the dirty marker
may have persisted without a confirmed journal-ownership header. They correctly
mounted read-only with `AFATFS_ERROR_RECOVERY_REQUIRED` and **zero writes**;
fsck still found clean metadata and every original file remained intact. There
were no read-only outcomes during the API operations or interrupted undo replay.
An unowned dirty session is not guessed to belong to this library.

No library incompatibility was found, so no cache, operation-machine, journal
format or embedded RAM allocation changed. The C test probe now preserves the
mount error when reporting a protected read-only result, so a later missing-name
lookup cannot obscure the ownership diagnostic. The default, FAT-only and
exFAT/freefile self-contained regressions also passed.

Evidence:

```text
tests/build/api-powercut-current/report.json
tests/build/api-powercut-current/audit.json
tests/build/api-powercut-current.log
tests/build/api-powercut-regression.log
```

The full matrix was executed through the Python command below. Afterward, the
verifier was tightened to explicitly require the old state during undo replay;
the supplementary audit applies that stricter condition to all 224 recorded
replays, checks all fsck codes and completion checkpoints, and verifies unchanged
library/binary hashes. An optional repeat through the updated PowerShell runner
was not authorized, so it is not counted as another executed matrix.

```powershell
# Compiles and selects the new API matrix (also accepts -AddressSanitizer):
./tests/run_exfat_powercut.ps1 -Api -Python python

# Direct command used for the completed run, with the compiled harness:
python tests/exfat_api_powercut.py --binary tests/build/test_exfat_api_powercut.exe --exfatprogs tests/build/external-tools/exfatprogs-1.3.2-1.exe --output tests/build/api-powercut-current
```

The Python command accepts `--cluster`, `--scenario` and `--no-replay` for focused
checks. All images and trace files stay under `tests/build`; physical cards are
not accessed. As in the external-format suite, this Windows exfatprogs build must
run outside the restricted process sandbox for successful exits.

The file-backed suite records completed 512-byte writes, then starts a fresh process
on every durable prefix and on both 256-byte prefix/suffix tears of the next write.
Fixtures are externally formatted by FATtools, include an untouched patterned file,
and deliberately position a target primary entry at the end of a directory sector.
Provisioning completes before application-operation fault injection begins.

`tests/build/journal-final/report.json` records **1,808 cuts** across seven scenarios
at cluster sizes 512 and 4096: create, 255-character create, append, append plus
`fsync`, truncate/rewrite, mkdir and abandoned empty allocation. The journal matrix
found 422 inconsistent intermediate images before recovery. After remount there
were **zero fsck errors, lost original append prefixes, unrelated-file failures,
completed-close failures or completed-fsync failures**. No `fsck -y` repairs were
used. There were 56 protected mount-handoff cuts and 28 protected shutdown-handoff
cuts; these stayed read-only with zero writes and structurally clean metadata.
Those figures describe the earlier shutdown order. The current
[handoff recovery validation](MEDIA_REMOVAL_VALIDATION.md#automatic-recovery-of-the-eight-shutdown-handoffs)
keeps journal ownership through the clean-marker ACK and recovers its eight
formerly read-only removal scenarios to writable mode. Legacy unowned-dirty images
remain protected; the old 1,808-cut report is retained as historical evidence.

The final self-contained suite additionally executes **22 cuts during recovery**,
including both half-sector tears, and three damaged-log guards (both headers,
descriptor and before-image). Repeated replay preserves the previously committed
700-byte file; damaged logs mount read-only without any physical write. These
checks use synthetic delayed I/O rather than an external fsck process. They run in
the default and exFAT/freefile configurations. FAT-only builds also pass.

The seven external-format layouts were rerun with the journal, with successful
bytewise FATtools roundtrips and 28 clean external fsck checks in
`tests/build/external-journal/report.json`. The existing native Windows VHD result
predates this journal change; it is not counted as journal-specific validation.

The final prefetch regression verifies automatic reads on sector release, repeated
hints, unchanged cursor, EOF suppression, and clean shutdown while a speculative
read remains in flight. This change was validated with the self-contained suite;
the 1,808-cut report above precedes the prefetch addition.

## Reproduce

### Global idle and power-off status

The self-contained suite also runs 15 end-to-end concurrent shutdown scenarios
across the default, FAT-only and exFAT/freefile builds. Each scenario has two
writers (1,700 and 2,300 bytes) and a third file handle issuing prefetch. FAT16 is
checked with one and four sectors per cluster; FAT32 uses one. exFAT uses backend
delays of 1, 7 and 31 polls in both exFAT-enabled builds.

The delayed backend snapshots each submitted write and can withhold its completion
indefinitely. Tests hold prefetch with both allocations queued, active-journal I/O,
and prefetch during shutdown. During shutdown they additionally hold a data write,
the journal commit header, ownership-release header and clean boot marker. Every
held transfer is retried for eight cycles: `isIdle` and `canPowerOff` must remain
false and queries must not advance the backend. During shutdown, `destroy(false)`
must also remain false.
The backend then reports four busy end-write attempts, which must keep `STOPPING`
and prevent power-off authorization.

At the first authorized power-off, all submitted writes must have completed, no
transfer or backend stop may remain pending, and exFAT's dirty marker must be
cleared. Sixteen later polls must produce no additional writes. Finally both
files are remounted and checked for exact sizes and every byte of their contents.
All 15 scenarios passed without a library-core change. These are deterministic
storage-model tests, not physical electrical power cuts on an SD card.

### Backend failure validation

The suite also executes 76 injected-failure cases across the three configurations:
28 exFAT shutdown cases, 27 FAT shutdown cases, 11 prefetch/read retry cases,
8 recovery-I/O retry cases, and 2 runtime journal-CRC guards. The shutdown cases
keep two writers open, retaining their final partial sectors. Both files must
retain their exact lengths and all bytes after a failed backend is restored and
the volume is cleanly shut down and remounted.

Coverage includes failed read/write callbacks, rejected write submissions,
read submissions returning false, and failed backend stop requests. Targeted
failures affect data sectors, boot reads, journal record reads, undo images,
record descriptors, published record counts, commit headers and ownership-release
headers. Failed writes persist only the first 256 bytes before reporting failure;
failed reads spoil half of the destination buffer before reporting failure.
No failed completion is treated as a durable full-sector acknowledgment.

Each shutdown fault remains active through at least three failures plus 32 further
shutdown attempts. `isIdle` and `canPowerOff` must remain false. Once faults are
removed, all accepted writes must be accounted for by successful or explicitly
failed completions, and no transfer may remain pending at authorization. Recovery
faults must not let `destroy(false)` discard initialization state. A runtime CRC
failure must freeze buffered work and block clean shutdown even after the injected
corruption is removed; only an explicit forced discard is used in that test, and
it must never report safe power-off.

The regression initially reproduced premature authorization when the journal's
ownership-release write failed after all files had closed. Journal I/O now retries
the same phase and prepared buffers instead of switching to a read-only path that
could bypass shutdown work. `CLOSING` records shutdown progress in the existing
state field, so a rejected required read cannot masquerade as global idle.
Transient I/O diagnostics use `AFATFS_ERROR_BACKEND_IO`; journal validation errors
retain `AFATFS_ERROR_RECOVERY_REQUIRED`. No persistent RAM fields were added.

Executed output is saved in `tests/build/backend-failure-validation-final.log`.
The three runtime configurations and eight legacy C test builds pass. These checks
run with `./tests/run_exfat_tests.ps1 -CompileLegacy`; external filesystem tools
are not needed for this failure-injection matrix.

Use the dependencies and pinned exfatprogs executable described in
[EXFAT_INTEROP.md](EXFAT_INTEROP.md), then run:

```powershell
./tests/run_exfat_tests.ps1 -CompileLegacy
./tests/run_exfat_powercut.ps1 -Python python
```

The power-cut runner compiles the delayed file-backed harness, runs all operation
cuts, then invokes `exfat_journal_recovery.py` for a separate file-backed replay/
corruption matrix with independent fsck checks. That additional replay runner is
provided for reproduction; the executed replay coverage reported above is the
self-contained 22-cut matrix. `-AddressSanitizer` enables MSVC ASan. All images,
traces, executables and JSON reports are stored under `tests/build`, ignored by Git.
The lower-level Python harness supports `--cluster`, `--scenario`, `--journal`,
`--torn` and a frozen `--baseline` binary for comparisons with earlier behavior.

## Limits

Only metadata is logged. In-place overwrites of previously valid file data are
not atomic, and data accepted by `fwrite` without a completed durability point may
be lost. The initial journal bootstrap is not protected by a journal that does not
yet exist. A dirty volume without a usable owned journal is protected read-only,
with `AFATFS_ERROR_RECOVERY_REQUIRED`, pending external repair. This also applies
to legacy shutdown/mount ownership gaps described above, and insufficient space for
provisioning. A journal with incompatible geometry is never overwritten.

Backend completion must mean durable, ordered media writes. Device firmware that
acknowledges volatile writes, spontaneous corruption of already durable sectors,
and damage to boot regions are outside this model. Prefix/suffix tears model only
the write in progress, not physical SD-card firmware behavior. Check
`afatfs_isReadOnly()` after mount. Use `afatfs_isIdle()` for global activity and
complete `afatfs_destroy(false)` before cutting card power; `afatfs_canPowerOff()`
confirms successful clean shutdown.
