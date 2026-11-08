# Extended asynchronous API

The declarations are in `lib/asyncfatfs.h`. The implementation is included from
`asyncfatfs_api.inc` and `asyncfatfs_format.inc` by the existing `asyncfatfs.c`;
applications still compile the same library translation units.

The existing limits remain: one mounted volume, ASCII names, 512-byte sectors,
32-bit sizes and sector addresses. FAT16/FAT32 names use strict 8.3 syntax;
exFAT accepts ASCII long names. Name arguments refer to entries in the current
directory, as in the original API. Rename works within that directory.
Full path resolution, cross-directory moves, Unicode, multiple volumes and
64-bit files are outside this extension.

## Requests and lifetime

```c
static afatfsRequest_t request;
static afatfsFileInfo_t info;

static void finished(afatfsError_e result, void *user)
{
    /* request.complete is true; inspect result and info. */
}

/* Once READY: */
bool accepted = afatfs_stat(&request, "DATA.BIN", &info, finished, NULL);
/* Continue calling afatfs_poll() from the application main loop. */
```

A request function returning true has accepted the operation. Its result is
delivered through `request.result` and the optional callback. False means the
request was not queued; no completion callback is promised. Keep the request,
all input strings/options/buffers and output objects alive and unchanged until
completion. Only `result`, `transferred` and `complete` are application-facing
request fields. Do not alias input/output buffers with the request's private
workspace. A completed request can be reused, including from its callback.

There is one active extended request at a time. Unrelated legacy file operations
can proceed when they do not conflict; namespace and allocation operations may
temporarily reject new work to preserve a consistent scan. All calls remain
serialized on the application's main thread. No heap or second sector cache is
added. `AFATFS_REQUEST_SCRATCH_SIZE` defaults to 256 bytes and can be reduced to
32 at compile time; this controls caller-owned workspace and buffered stream size.

Media removal invalidates handles and directory objects and marks an active
request complete with `AFATFS_ERROR_MEDIA_REMOVED`. A forced reset cancels it.
Cancellation does not call its callback; after reinitialization inspect the
request result and create fresh handles. Hardware failures retain pending work
for retry rather than authorizing shutdown.

## Operations

| Functions | Behavior |
| --- | --- |
| `open`, `close`, `sync`, `truncate` | Result-bearing alternatives to the legacy callbacks. Prefix every name with `afatfs_`. |
| `read`, `write` | Complete the requested transfer or report EOF/error; `request.transferred` counts bytes. A successful write means accepted into cache. Use sync/close for durability. |
| `ferror`, `clearerr` | Per-handle diagnostics; BUSY is retryable. The private file-busy helper remains private. |
| `fstat`, `stat` | Unified `afatfsFileInfo_t` for FAT and exFAT, independent of native entry layout. |
| `opendir`, `readdir`, `closedir`, `rewinddir` | Caller-owned directory cursor; no permanently occupied file slot. |
| `findfirst`, `findnext` | Unified enumeration filtered by case-insensitive ASCII `*` and `?`. The original camelCase raw iterators remain available. |
| `rename` | Preserves data, attributes, timestamps and supported exFAT benign secondary entries; refuses overwrite. |
| `unlink`, `rmdir` | Delete a file or an empty directory by name, reporting explicit errors. |
| `chmod`, `utime` | Update supported attribute bits or modification time. Packed dates are `(date << 16) \| time`, using FAT date/time fields. |
| `getfree` | Exact current free-cluster count by incremental FAT/bitmap scan, without an allocation map in RAM. |
| `getlabel`, `setlabel` | Read serial/label or create/update/remove the root volume label. Empty string removes it. Label limit: 11 ASCII characters. |
| `getcwd` | Reconstruct absolute current directory by incremental traversal. Depth is bounded by request scratch capacity; excessive depth reports UNSUPPORTED. |
| `chdrive`, `setcp` | Validate supported drive 0 and ASCII code page 20127; other values report UNSUPPORTED. |
| `expand` | Find a contiguous extent for an empty writable file. `allocate=false` sets an allocation hint without resizing; true allocates and publishes the requested size. Contents are undefined on FAT; exFAT VDL hides unwritten storage as zeros. |
| `gets`, `puts`, `printf` | Asynchronous line input/text output. Gets includes the newline and terminates the buffer; transferred excludes the terminator. Printf requires a separate caller-provided formatting buffer and uses C `vsnprintf`. |
| `forward` | Read through a consumer callback. Returning zero pauses, a short count retains the remainder, and a count greater than offered is an error. |
| `rewind` | Seek to offset zero using the original asynchronous seek mechanism. |
| `mkfs`, `fdisk` | Format explicit test/application media ranges while unmounted; details below. |

`readdir`/`findnext` return IN_PROGRESS when waiting for I/O. Retry with the
same output object after polling. SUCCESS with `info.name[0] == 0` means end of
directory. Keep a directory object alive until `closedir` or session teardown;
do not copy or move an opened object. Directory readers block deletion/rename
of that directory, and pending enumeration participates in `afatfs_isIdle()`.

`getfree` fills `clusterSize`, `totalClusters` and `freeClusters` in
`afatfsVolumeInfo_t`; `getlabel` fills label, serial and geometry. File info
reports live logical size and exFAT valid length. `allocatedSize` inherits the
original handle's cluster-rounded allocation estimate, not a full FAT-chain
accounting scan for externally preallocated files.

## Existing API corrections

`afatfs_ftruncate` now truncates **at the current cursor**, matching FatFs:
prefix and cursor are preserved, EOF is unchanged, trailing clusters are freed.
Seek to zero to empty a file. This covers fragmented FAT chains, exFAT
NoFatChain allocation and the existing freefile mode. Unlink still removes the
whole file independently of cursor position. Truncation waits for ordered data,
directory, allocation and journal writes before successful completion.

`afatfs_fputc` now returns 1 for acceptance and -1 for rejection. Retry BUSY;
do not advance the input on rejection. Open adds `x` and `x+` for exclusive
creation. Existing `w+` retains the library's documented create-or-open behavior
without truncating; `w`/`ws` truncate existing files. Malformed FAT 8.3 names are
rejected rather than silently shortened into aliases. Directory/file mismatches,
read-only attributes and active handles are checked before mutation. Writes and
truncation update archive/modified metadata; rename preserves existing metadata.

FAT32 allocation changes invalidate FSInfo free-space hints. Clean shutdown
writes the standard unknown value to valid primary/backup FSInfo records and
waits for acknowledgment, preventing stale counts from reaching the next driver.

## Formatting and partitioning

First complete `afatfs_destroy(false)` and check `afatfs_canPowerOff()`, or use
an initially unmounted instance. `mkfs`/`fdisk` reject a mounted volume. They use
the existing backend and cache buffer and participate in idle/power-off checks.
Keep options unchanged until completion; poll until the final write and backend
end-write acknowledgment complete. Call `afatfs_init()` to mount afterward.

`afatfsFormatOptions_t` selects FAT16, FAT32 or exFAT, start sector, sector count,
cluster size (0 selects automatically) and serial. FAT12 is not supported.
Formatting initializes filesystem metadata without erasing all free data.
`afatfsPartitionOptions_t` supplies disk size and up to four partition ranges;
MBR uses the supplied type bytes. GPT writes primary/backup headers, CRCs and
128 entry slots, with up to four configured Microsoft Basic Data partitions.
Supply a nonzero disk GUID unique to that disk. Converting GPT to MBR clears
old GPT signatures only where they do not overlap a new partition.

These operations intentionally overwrite the supplied ranges. Formatting and
partition-table replacement are not power-loss atomic; interrupted formatting
must be repeated. The tests use regular image files and simulated media only.
