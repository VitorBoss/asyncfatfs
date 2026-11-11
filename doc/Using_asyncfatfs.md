## Using AsyncFatFS

AsyncFatFS is meant to be simple to integrate into C codebases. It is suitable for use in embedded systems
without an operating system.

### Limitations

This library is not meant to cover all possible use cases for FAT filesystems. Be aware of these limitations
before using AsyncFatFS:

#### Storage Backend Assumptions

This library was created with SD cards in mind as its primary storage backend. Accordingly, AsyncFatFS
requires that the storage backend operates on 512-byte blocks. If implementing AsyncFatFS on a backend that
does not use 512-byte blocks, this block size will need to be emulated by your backend driver. Additionally,
backend drivers must support reads or writes to arbitrary block indexes in any order. AsyncFatFS is not
suitable for directly interacting with raw flash chips; a flash translation layer of some sort is required.

#### MBR, GPT and Partitions

AsyncFatFS selects the first supported MBR partition, pre-formatted as FAT16, FAT32 or (when enabled) exFAT.
With exFAT enabled, a protective MBR also enables GPT discovery. GPT selects the first Microsoft Basic Data
partition containing an exFAT boot sector, skipping reserved partitions and other Basic Data filesystems.
A protective MBR takes precedence over legacy entries in hybrid tables. GPT support does not enable
FAT16/FAT32 on GPT or unpartitioned media, and does not widen the 32-bit backend address range (2 TiB).

GPT discovery checks header and complete entry-array CRC32 values, geometry, partition bounds and overlap
with the selected partition. Entry sizes of 128 times a power of two are supported, including entries larger
than a sector. Invalid primary headers/arrays fall back to the backup copy without repairing the GPT.
The backup location comes from the protective MBR; if its size is saturated (`0xFFFFFFFF`), a valid primary
header is needed to establish the backup location because the backend has no capacity-query API.
Both GPT metadata and the selected volume must fit the current 32-bit address range.

The GPT Basic Data read-only attribute is respected: no journal provisioning/replay or boot-marker writes
are performed on such a partition. Dirty volumes remain read-only without automatic recovery writes.
Discovery reuses the existing cache and temporarily overlaps its state with the exFAT ASCII up-case table;
there is no additional sector buffer or persistent RAM allocation.

#### Paths and the Current Working Directory

AsyncFatFS does not support opening files by absolute path (e.g. `/path/to/file.txt`). Instead, it uses the
concept of a Current Working Directory (CWD), within which all directory-specific operations occur. Note that
the CWD is global to the entire library(!). Managing the global CWD is a task left to the library user.

Once a file is opened, the file handle may be retained even if the CWD gets changed. Thus, it is possible to
have multiple files from different directories opened at the same time.

#### File Creation and Modified Datetimes

New files and metadata updates use `afatfs_fatTime()`, whose default implementation returns the build date
and time. GCC-compatible builds may override this weak function to provide a clock.

#### Freefile

The freefile optimization only applies to FAT16/FAT32 and works with one file at a time. Additionally, the special FREESPAC.E file must
be created and remain on the volume.

#### Filenames

The default configuration enables `ASYNCFS_ENABLE_LFN` for FAT16/FAT32 and disables exFAT.
With `AFATFS_EXTERNAL_CONFIG`, define `ASYNCFS_ENABLE_LFN` in your own `afatfs_config.h` to enable it;
omitting it retains the smaller 8.3-only implementation.

LFN supports leaf names up to 255 printable ASCII characters, with case-preserving enumeration and
case-insensitive lookup by either the long name or its 8.3 alias. Spaces and multiple dots are accepted;
`" * / : < > ? \\ |`, trailing dots/spaces and non-ASCII API names are rejected. Paths in filenames remain
unsupported: use `afatfs_chdir` as before. Native UTF-16LE entries are streamed through the existing cache,
including across fragmented directory chains, with ordinal, checksum, type, cluster and padding checks.
Malformed chains fall back to their short entry. Non-ASCII on-disk names remain accessible by their short
alias; Unicode API encoding is outside the current scope.

Creation generates collision-checked numeric aliases when required, flushes LFN entries before publishing
the short entry, and preserves the following directory end marker. Deletion makes the short entry durable
before releasing its attached LFN slots. Rename uses the existing asynchronous copy/publish/delete flow.
FAT32 with LFN now enables the metadata journal described below. FAT16 and builds with the FAT32
journal disabled retain ordering barriers without atomic multi-sector recovery. Existing 8.3 names and the raw `afatfs_findNext` interface remain supported;
use `afatfs_readdir`/`afatfs_findnext` for assembled long names.

`afatfs_fstat` may yield while reading a long name. Retain the same output object until it completes;
the caller's output supplies the assembly buffer, avoiding another 256-byte buffer per handle. The
operation union retains the requested name only during creation/search. See
[LFN validation and memory results](../tests/LFN_VALIDATION.md) for the measured alignment and RAM costs.
The [native Windows FAT32 validation](../tests/WINDOWS_FAT32_LFN_VALIDATION.md) covers bidirectional VHD
round trips, Windows-generated aliases and read-only CHKDSK checks. Request-based close/sync also accept
directory handles returned by open, including root directories.
exFAT names support 1 to 255 printable ASCII characters, excluding `" * / : < > ? \\ |`. The exFAT
implementation uses the volume's up-case table for case-insensitive comparison and name hashes.

#### FAT32 Metadata Journal

`ASYNCFS_ENABLE_LFN` enables the FAT32 journal by default; define `AFATFS_DISABLE_FAT_JOURNAL` to
retain the smaller implementation without persistent recovery. FAT16 remains unjournaled. exFAT
remains disabled by default and retains its journal when explicitly enabled.

The first clean FAT32 mount reserves a contiguous hidden/system/read-only `AFATFS.JNL` file. Its
capacity is `2 * fatSectors + 64` distinct metadata sectors, stored as two alternating header sectors
plus one descriptor and one before-image sector per record, rounded up to a cluster. This is about
four times one FAT's size plus 65 KiB of disk space. Provisioning needs that much contiguous free
space and zeroes it asynchronously before application operations are accepted. The journal name and
allocation are reserved. Do not edit, delete, move or defragment it using another writer.

FAT32 and exFAT share the same capture, CRC, I/O, checkpoint and recovery engine. It uses one aligned
512-byte buffer and the retained original cache sector, with no heap or extra per-file state. The
filesystem adapters handle discovery, extent allocation and dirty markers; their existing disk
signatures remain distinct and the exFAT journal format remains compatible. FAT32 validates the
entire contiguous journal chain in both FAT copies before reading or replaying its records.

An interrupted metadata transaction is validated completely, then undone before the volume becomes
writable. Recovery can itself be interrupted and restarted. A rename is recovered as its old or new
name, without a duplicated entry or cross-linked chain in the tested fault model. `fsync`, close,
rename and unlink wait for their metadata checkpoint. Open callbacks alone are not durability
points. Concurrent handles may share a transaction, so keep polling all active work.
Both FAT copies, cache writes, the empty checkpoint, clean markers and ownership release must be
acknowledged before clean shutdown can authorize `afatfs_canPowerOff()`.

Protection starts after the first successful journal provisioning. A cut during bootstrap can leave
unclaimed allocations or divergent FATs; dirty volumes without a trustworthy owned log are mounted
read-only with `AFATFS_ERROR_RECOVERY_REQUIRED`. Invalid headers, record/image CRCs or journal chains
also block recovery writes. Preserve such media for external repair. After an interrupted library
session, let this library recover before another driver writes to the volume. Use clean shutdown
before handing it to Windows; Windows does not replay this private journal. Interrupted external
writers are outside its recovery contract.

This protects metadata, not in-place payload overwrites or unsynced data still in RAM. Backend write
acknowledgments must represent durable, ordered writes. See the [FAT32 power-cut and shared-journal
validation](../tests/FAT32_LFN_POWERCUT.md) for tested cuts, limits and measured RAM costs.

#### Memory and Metadata I/O

The journal shares provisioning, capture and recovery workspaces whose lifetimes do not overlap.
Its initialization and shutdown selectors share one byte, reset to the initial shutdown step before
`READY`. The runtime phase remains separate so pending commits and mount errors retain their state.
exFAT temporarily borrows the first public handle during initialization and releases it before `READY`,
including protected read-only mounts. There is no permanent journal handle, extra sector buffer or heap.
The public handle limit and file/directory/request sizes remain unchanged.

During the first journal reservation, FAT-chain and bitmap allocation updates stay in the existing
cache until cache pressure requires I/O or the allocation reaches its final durability barrier. That
barrier still acknowledges every allocation sector before publishing the journal file's extent.
Zeroing of the reserved extent and all undo-record ordering remain unchanged.

On writable journaled FAT32, regular-file allocation also combines changes to the new and previous
FAT entries where the cache permits. Background flushing pauses only during those allocation phases;
a cache miss that cannot find a reusable buffer starts a flush so fragmented chains, other writers
and retained readers can make progress. The directory update, sync/close and transaction checkpoint
continue to wait for the required writes. Directory initialization and unjournaled allocations retain
their ordering barriers. exFAT's normal file-allocation barriers are retained.

This reduces repeated media writes without promising that every workload needs fewer reads. See
[memory and I/O measurements](../tests/MEMORY_IO_OPTIMIZATION.md) for before/after ABI sizes, transfer
counts, interrupted-write checks and the measured tradeoff for a single FAT32 writer. The subsequent
[journal lifetime layout report](../tests/JOURNAL_LIFETIME_MEMORY.md) describes the workspace unions,
remaining simultaneous fields and the additional RAM savings.
The [operation and mount lifetime report](../tests/OPERATION_LIFETIME_MEMORY.md) covers further
workspace sharing during file creation, truncation, directory growth and exFAT initialization,
with measured sizes for 16-, 32- and 64-bit targets.

#### exFAT Development Support

exFAT is disabled by default. With `AFATFS_EXTERNAL_CONFIG`, define `ASYNCFS_ENABLE_EXFAT` in
your own `afatfs_config.h` to opt in. This feature is independent of FAT LFN support and can coexist
with it. The exFAT regression/VHD/power-cut runners explicitly select their exFAT test configuration.

Mounting verifies the boot checksum, header geometry, mandatory bitmap and up-case entries, and the
up-case table checksum. Root directories, allocation bitmaps, and up-case tables can have fragmented FAT
chains. Allocation and release use the bitmap. File creation, append, truncation, deletion and subdirectory
growth update native entry sets and their checksums, including sets crossing sector/cluster boundaries.
Existing contiguous `NoFatChain` streams are read directly; extending their allocation converts them to
FAT chains. Bytes past `ValidDataLength` read as zero, and gaps are initialized before exposing later writes.
`mkdir` allocates and zeroes the first directory cluster before its completion callback.

The current scope is MBR or GPT media with 512-byte sectors, one FAT, and cluster sizes up to 32 MiB. TexFAT
(two FATs), Unicode API names, and 64-bit sector/file interfaces are not implemented. File lengths,
allocated lengths and cursor offsets remain 32-bit; files whose rounded allocation exceeds `UINT32_MAX`
are rejected. Volumes exceeding the 32-bit backend address range are rejected. `ASYNCFS_ENABLE_LBA64`
produces a compile-time error until the backend and public finder API are widened together.

Entry sets may contain at most 19 entries; supported benign trailing entries are preserved. Walking a
directory with a larger file entry set fails the open operation. Matching unsupported files (including
oversized files) fail without creating a duplicate. Non-ASCII on-disk names are outside the supported API
scope; a matching hash requiring Unicode comparison fails safely.

The FAT reservation-file optimization is bypassed on exFAT, including when `AFATFS_USE_FREEFILE` is
enabled for FAT16/FAT32. Each file handle (including the CWD) retains 608 bytes of entry data, 19 entry
positions and a small amount of state when exFAT is enabled. No heap allocation or synchronous disk I/O
is introduced. Enabling exFAT therefore increases static RAM use even while a FAT volume is mounted.

Mount marks `VolumeDirty` and sets `PercentInUse` to unknown; `afatfs_destroy(false)` clears the dirty bit
after file data and metadata have been flushed. `afatfs_destroy(true)` intentionally leaves it dirty.
Clean shutdown retains ownership of the committed empty journal until the clean boot marker has been
acknowledged, then releases the journal and waits for that acknowledgment before authorizing power-off.
A removal during either write can therefore mount writable from a valid owned empty checkpoint, without
replaying committed data. The journal header format is unchanged; the shared engine now uses one
sector buffer, reducing static RAM by 1,024 bytes compared with the previous exFAT journal.
exFAT reserves a contiguous hidden/system/read-only `AFATFS.JNL` file on the first clean mount. Metadata
before-images reach this undo log before FAT, bitmap or directory sectors can be modified. An interrupted
transaction is validated completely and replayed before the mount becomes writable. Recovery can itself
be interrupted and restarted. This uses the existing polling machines and static buffers, without heap
allocation. Reserve approximately twice the FAT and bitmap size plus 65 KiB, rounded to a cluster, in one
contiguous free extent. Initialization can take longer while this extent is zeroed. The journal name is
reserved; do not alter its contents or allocation through another driver.

`afatfs_fsync(file, callback, user)` releases this file's data buffers, flushes them, publishes its valid
length, and waits for the metadata transaction to commit before invoking the callback. exFAT close and
truncate also wait for their metadata commit. `afatfs_flush()` waits for flushable cache pages and the
journal; it does not unlock a file's partial data sector or publish its logical EOF. Use `fsync` or close
for an application durability point. Transactions can cover concurrent handles; keep polling them all.

The journal protects filesystem metadata, not overwritten user data. In-place data writes are not atomic.
Before the first successful journal provisioning, power loss can still require external repair. A dirty
volume with no usable owned journal mounts read-only (`afatfs_isReadOnly()` and error
`AFATFS_ERROR_RECOVERY_REQUIRED`). This includes mount-ownership gaps and volumes left by the older
shutdown ordering, where an unowned dirty volume cannot be distinguished from an interrupted external
writer. Such existing images are not retroactively made trustworthy by the new ordering. Preserve the image and repair
it externally before writing again. After a crash during library writes, let this library recover the
volume before mounting it with another writer: Windows and exfatprogs do not replay this private journal.
An interrupted external-driver session is not covered by the journal. Backend write completions must mean
durable, ordered media writes; volatile device caches and unrelated media corruption are not covered.

Backup boot-region recovery is not implemented. The [power-cut suite](../tests/EXFAT_POWERCUT.md)
documents the fault model and recovery checks. The [external interoperability suite](../tests/EXFAT_INTEROP.md)
validates exfatprogs/FATtools images and independent filesystem checks. The separate
[native Windows VHD suite](../tests/WINDOWS_VHD_VALIDATION.md) validates Windows formatting, driver I/O
and read-only CHKDSK checks. Physical hardware and native macOS driver validation remain outside the
current test coverage.

`afatfs_findNext` returns raw 32-byte entries: on exFAT inspect the first byte for the native entry type.
`afatfs_fGetFileInfo` is FAT-only. `afatfs_fGetExFatFileInfo(file, &primary, &stream)` returns pointers to
the handle's cached native metadata, or NULL pointers for the root directory; it returns false if busy
or if the mounted filesystem is not exFAT. These pointers remain owned by the library and must not be modified.

#### Errors and retries

`afatfs_getLastError()` reports session diagnostics; `afatfs_ferror(file)` reports the last result for a
handle, independently of other files. `afatfs_clearerr(file)` clears that handle's diagnostic. A busy
nonblocking read/write is retryable. The request API below reports an explicit result in its callback.

The backend must report a failed completion with a NULL buffer, not with a successful acknowledgment.
Rejected submissions and failed completions retain pending work and retry through `afatfs_poll()`.
Permanent hardware failures require an application timeout or media-removal handling; they never make
pending data safe to discard. `afatfs_canPowerOff()` remains false until clean shutdown finishes.

#### File Seeking Behavior

AsyncFatFS does very little caching of file structure metadata. Thus, seeking to arbitrary points in a file
may require a large amount of IO. Forward seeks that stay within a 512-byte block are IO-free. Relative
forward seeks scale linearly with the distance from the current write cursor, but absolute or reverse seeks
scale linearly with the distance from the start of the file.

#### No Reentrancy

The AsyncFatFS API calls are not reentrant. They must all be called from the same thread in a multi-threaded
system, and they must not be called from within an interrupt service routine or signal handler function.

### Compiling and Linking

Simply copy the 5 files from the lib/ directory into your project. Make sure that the .h files are in a path
that is used by your build system for header files, or that they are in the same directory as the .c files.
Compile and link against both `fat_standard.c` and `asyncfatfs.c`.

AsyncFatFS requires a C99-compatible compiler. In addition, the compiler must support the
`__attribute__((packed))` struct attribute. The code is tested with GCC.

### Compile-Time Settings

There are a number of `#define` compile-time settings that affect how AsyncFatFS operates. They can either be
directly adjusted in `asyncfatfs.c`, or redefined in a user-supplied `afatfs_config.h` file by defining
`AFATFS_EXTERNAL_CONFIG` in your build system. This is useful if you want to keep AsyncFatFS as a git
submodule without modifying it. Available settings:

```
#define AFATFS_DEBUG
#define AFATFS_DEBUG_VERBOSE
```

Turns on various asserts and debug features. Note that these features require `signal.h` and `stdio.h`, and
thus may not be suitable for use in embedded systems. The \_VERBOSE flag includes some additional printfs to
stderr.

`#define AFATFS_USE_INTROSPECTIVE_LOGGING`

Logs performance information about individual backend read and write operations to the filesystem itself. This
is independent of the above DEBUG settings. See the documentation for `sdcard_setProfilerCallback` below for
more information about this setting.

`#define AFATFS_NUM_CACHE_SECTORS 8`

AsyncFatFS maintains a RAM cache of the SD card blocks it is currently using. The more blocks that can be kept
in cache, the more efficient the library can be. Each cache block uses 512 + ~16 bytes (~528 total) of RAM.
Avoid setting this below `2 * AFATFS_MAX_OPEN_FILES`.

`#define AFATFS_MAX_OPEN_FILES 3`

AsyncFatFS uses statically allocated memory, so the number of file descriptors is set at compile time. Set
this to the maximum number of files you expect to have open at once. Note that closing a file does not
immediately free its descriptor, so include extra descriptors if your application will be closing and opening
files in rapid succession. Be aware that listing directories also requires a file handle to be held for the
duration of the listing operation. On 32-bit platforms, each file descriptor consumes ~68 bytes. Note that
certain elements of the block cache have a counter limited to 6 bits (max value of 63). Increasing the file
descriptor limit beyond 62 runs the risk of overflowing this counter and causing unexpected behavior.

`#define AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT 4`

Sets the minimum number of SD card blocks to use a multi-block write command for. All other writes will use
the single-block write command. This define may be omitted if your backend driver does not support multi-block
writes, in which case all writes will be single-block.

`#define AFATFS_USE_FREEFILE`

AsyncFatFS includes a special optimization called the freefile. If enabled, AsyncFatFS will allocate as many
contiguous clusters as it can and place them all into a special placeholder file. This can be used to do
continuous writes on a single file without needing to read any filesystem metadata, as all the write locations
are known in advance. When this feature is used, a special file is created to reserve these clusters (see
`AFATFS_FREEFILE_LEAVE_CLUSTERS` below). If this feature is not wanted, this directive may be commented out.

`#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100`

If `AFATFS_USE_FREEFILE` is enabled, the freefile will consume all of the available free space on the storage
device. This would prevent new files from being created, and would also prevent other files that do not use
the freefile from expanding. This setting leaves a given number of FAT clusters available when allocating
the freefile. The default value of 100 ensures a minimum of 50 kB of available extra space (typically more,
depending on the cluster size of the FAT).

`#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"`

Choose the filename that the freespace feature uses for its special reservation file. Note that this file
never contains user data, only reserved blocks to be used when writing other files. The filename must respect
the FAT "8.3" name limitation.

### User API

The [extended API reference](Extended_API.md) documents caller-owned requests, unified metadata,
directory iteration, rename/attributes, labels/free space, preallocation, text/streams and formatting.
The original entry points below remain available, with the corrected truncation and putc behavior.

This section describes the user-facing API of AsyncFatFS. The backend driver API that AsyncFatFS itself calls
is described [in the next section](#backend-driver-api).

#### General Notes about API Usage

All functions in the API are non-blocking. Because of this requirement, most operations do not succeed
immediately; instead, they merely queue up the operation for later processing. It is important to check the
return values for all API calls and handle them appropriately. Some calls will need to be called multiple
times until they report success. Other calls require you to register a callback that will be fired when the
operation completes.

> [!IMPORTANT]
> Callbacks will sometimes be directly called by API call that initially
> registers them before the call returns. Be aware of this when altering
> state machine states from within callback functions.

File handles must be treated as opaque pointers. Once a file has been closed, any references to its handle
that were kept bt the user application must be discarded. File handles are recycled internally, so no
guarantees are made about the state of handles after a close.

AsyncFatFS allows directories to be opened and referred to by file handles. This is primarily to enable the
directory listing and chdir functionality; directory pseudo-files may not be used to open files outside of the
CWD. Attempting to perform file operations (e.g. fread, fwrite) on a directory file handle may result in
undefined behavior.

Throughout the documentation, the term `CWD` is used to refer to the Current Working Directory.  See the notes
in the [Paths and the Current Working Directory](#paths-and-the-current-working-directory) section for more
information.

#### Initialization and Mainloop

`void afatfs_init(void)`

This function initializes the global data structures for the AsyncFatFS library and starts the process of
mounting the FAT16/FAT32 volume. This function must be called once, and must be called before any other
AsyncFatFS API functions are called. It should be called after any required initialization routines are
complete for the backend storage driver. If the storage media is removable, wait until media has been detected
to call `afatfs_init`. Note that AsyncFatFS will require a number of calls to `afatfs_poll` to complete
initialization. The user can call `afatfs_getFilesystemState` to check the status of the initialization. Once
initialization is complete, the CWD will be set to the root directory of the filesystem.

`void afatfs_poll(void)`

This function must be called periodically by the user application in order to make progress on any AsyncFatFS
operations. It will never block and only performs work if there are pending filesystem operations, so it
should be called frequently from the user application's mainloop.

`bool afatfs_flush(void)`

Attempt to flush all dirty blocks in RAM to the backend. Returns `true` if all data has been successfully
written to the backend, or `false` if there is still pending writes. Only ensures data sent by operations
before the flush have been successfully passed to the backend driver. Any subsequent file or directory
operations can produce newly-dirty blocks.

`bool afatfs_destroy(bool dirty)`

With the `dirty` parameter set to `false`, this function closes all open files, completes all writes, flushes
all cache to storage, and de-initializes AsyncFatFS. If the user wishes to safely eject the backend storage
media, this function must be called periodically until it returns `true`. After any additional
backend-specific media flushes are complete, the media may be safely ejected. After this function returns
`true`, a call to `afatfs_init()` must be made to begin using AsyncFatFS again.

With the `dirty` parameter set to `true`, AsyncFatFS will skip cleanly closing files and flushing dirty cache
blocks to storage. THIS WILL CAUSE DATA LOSS!

Clean shutdown enters `CLOSING` immediately and stays there while file/cache/journal writes are pending.
Backend I/O failures preserve their buffers and operation phase for retry; they never grant power-off
authorization. If shutdown is requested during initialization it advances mounting without discarding its
pending work. A fatal state is preserved and cannot complete clean shutdown. An unrecoverable runtime
journal validation error likewise blocks clean shutdown instead of discarding accepted writes.

`bool afatfs_isIdle(void)`

Observes the global library state without polling or performing I/O. Returns `false` during initialization,
errors, shutdown, file/CWD/internal-file operations, cache reads (including prefetch), writes, dirty or
locked cache pages, and pending FAT32/exFAT journal work. Clean retained read sectors alone do not make the
system busy. A successfully stopped filesystem is idle. This uses existing state without additional fields.

Idle is an activity snapshot, not permission to cut power: a mounted volume can still have open handles,
deferred EOF metadata or an exFAT dirty marker. Do not submit new operations once shutdown is requested.

`bool afatfs_canPowerOff(void)`

Returns `true` only after `afatfs_destroy(false)` has successfully closed the files, drained cache/prefetch,
committed the journal, completed clean volume marking, and confirmed backend readiness. Shutdown ends
any multi-block write through `sdcard_endWriteBlocks()` when `AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT` is
enabled; `BUSY` keeps the filesystem in `STOPPING` and the query remains false. Backends enabling that
feature must implement the already-declared end-write function.

The query is observational: keep calling `afatfs_destroy(false)` to progress shutdown. It returns `false`
before initialization, during mount/shutdown, after a forced `afatfs_destroy(true)`, or after another
`afatfs_init()`. Successful shutdown records `STOPPED` in the existing filesystem-state field, without
adding RAM. Repeating `afatfs_destroy(false)` after success preserves that state.

```c
if (shutdownRequested) {
    afatfs_destroy(false); // Non-blocking: repeat from the application's main loop.
    if (afatfs_canPowerOff()) {
        // Complete any platform-specific backend shutdown, then disable card power.
    }
}
```

This confirms library-owned work only; other users of the storage driver must be coordinated by the
application. Backend write completion must mean durable storage, as required by the driver contract.

`void afatfs_notifyMediaRemoved(void)`

The application/driver must call this on a detected removal or replacement, serialized with other library
calls and before permitting I/O to the inserted card. The boolean `sdcard_poll()` interface cannot
distinguish absence from `BUSY`; removal detection is therefore the backend/application's responsibility.
The notification cancels the old session through `sdcard_reset()`, invalidates all handles and queued
application callbacks, clears volatile cached work and reports `REMOVED` / `AFATFS_ERROR_MEDIA_REMOVED`.
Both `isIdle` and `canPowerOff` remain false. Merely reinserting a card does not resume old requests.

After reinsertion, initialize the backend for that card, call `afatfs_init()`, and wait for `READY` before
reopening handles. A new mount revalidates the volume and recovers a trustworthy owned FAT32/exFAT
journal when enabled. Unsynced data lost at removal cannot be reconstructed; in-place overwrites and
unjournaled FAT16/FAT32 metadata do not gain transactional recovery. Legacy unowned-dirty volumes and
mount-ownership gaps can still require repair.
After any unexpected removal, recover and cleanly unmount with this library before any external writer,
even if the boot dirty bit was already cleared: journal ownership may still belong to the interrupted
session. Normal completed clean shutdown leaves both a clean boot marker and an unowned empty journal.

`sdcard_reset()` must synchronously cancel DMA/queued transfers and stop all access to the old buffers
and medium before returning, without starting new I/O. This applies to init and forced/clean teardown
as well as removal. Old software completion notifications may arrive later, but must preserve their
original `callbackData` and must not touch retired buffers. A single 32-bit session counter rejects those
notifications for both the cache and journal; no handle layout, heap allocation or on-disk format changes
are introduced. This counter is separate from the mount state so resetting that state cannot revive old
callbacks. The [removal validation](../tests/MEDIA_REMOVAL_VALIDATION.md) documents the tested cases.

#### Directory Operations

`bool afatfs_mkdir(const char *filename, afatfsFileCallback_t complete, void *user)`

Create or open a directory in the CWD with the filename supplied. Note that the filename must respect the
FAT 8.3 limitation. If the directory already exists, the operation will proceed as if it had created the
directory, but the files within the directory will not be affected. Returns `true` if the operation has
begun, or `false` if there are no free file descriptors available to start the operation. The user must supply
a callback function with the following signature:
`void mkdir_callback(afatfsFilePtr_t file, void *user)`
This callback will be fired with a file handle pointing to the new or existing directory if the operation
succeeded, or it will be fired with a `NULL` file handle if the mkdir failed. Failures could be due to the
filesystem being full or the CWD hitting a FAT limit. The `user` pointer may be used to identify the source of
the callback, or may be left `NULL` if not needed. It is up to the user application to close the directory
handle supplied to the callback (if not `NULL`) once it is no longer needed.

`bool afatfs_chdir(afatfsFilePtr_t dirHandle)`

Change the CWD to the directory supplied by the `dirHandle` argument. You must successfully call
`afatfs_fopen` on the directory before passing it to to this function, and you must wait for this function to
return successfully before calling `afatfs_fclose` on the dirHandle. Returns `true` if the chdir operation has
been completed successfully, otherwise returns `false` if the filesystem is waiting on other operations to
complete. If `false` is returned, you must call `afatfs_chdir` again with the same parameter at a later time.
This operation (unlike `afatfs_mkdir`) does not queue; the CWD will not be changed until a `true` is returned.

As a special case, calling chdir with `NULL` as the dirHandle argument will change the CWD to the root
directory of the filesystem. This operation always succeeds, but may put the filesystem into a busy state.

`void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder)`

This function is used to begin a directory contents listing operation. As with chdir, the `directory` argument
must be a file handle to a directory that has been successfully opened by `afatfs_fopen`. The `finder`
argument is a pointer to an `afatfsFinder_t` object that the user application must allocate. Both the `finder`
object and the `directory` file handle must be kept for the duration of the directory listing operation. Note
that only one directory listing operation may be running on a given directory file handle at any given time.

`afatfsOperationStatus_e afatfs_findNext(afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry)`

This function is the iteration step for listing a directory. The `directory` and `finder` arguments must be
the ones used in the `afatfs_findFirst` call. The user application must allocate a `fatDirectoryEntry_t *`
pointer and pass the pointer to this pointer to the `dirEntry` argument. Each call to findNext returns either
one or zero directory entries. If the return value of `afatfs_findNext` is `AFATFS_OPERATION_IN_PROGRESS`,
then no directory entry was returned because the disk is busy and the operation will need to be retried later.
If the return value is `AFATFS_OPERATION_SUCCESS`, then the `dirEntry` pointer will either be a pointer to a
valid directory entry object, or `NULL` if the operation has reached the end of the directory.

Directory entries are returned in the order they are found on the disk; alphabetical ordering is _not_
guaranteed.

In addition to real files and directories, a number of other entries may be returned:
- The `.` and `..` directory entries (for self and parent directory, respectively)
- The volume ID pseudo-entry (check the attributes field for the `FAT_FILE_ATTRIBUTE_VOLUME_ID` flag)
- Empty/deleted directory entities (use `fat_isDirectoryEntryEmpty` to check)
- The special terminator directory entry (use `fat_isDirectoryEntryTerminator` to check)

The user application may also want to check for the presence of the `FAT_FILE_ATTRIBUTE_SYSTEM` and/or
`FAT_FILE_ATTRIBUTE_HIDDEN` flags and skip or ignore these files. For example, the `System Volume Information`
directory (typically given the short name `SYSTEM~1`) created by Windows has both of these flags set.

Once the terminator directory entry has been encountered, the user application must stop calling
`afatfs_findNext` and call `afatfs_findLast` to clean up the directory iterator.

`void afatfs_findLast(afatfsFilePtr_t directory)`

This function cleans up the resources used during a directory listing operation. The `directory` argument is
the file handle to the directory that was being listed. If no previous listing operation was in progress, this
function is a no-op. It is safe to call it more than once. After `afatfs_findLast` has been called, the
`finder` object may be freed and the `directory` object may be closed with `afatfs_fclose` if it is no longer
needed.

`bool fat_isDirectoryEntryEmpty(fatDirectoryEntry_t *entry)`

Helper function to determine if a given directory entry pointer is an empty (previously deleted) entry.
Returns `true` if the entry is empty, returns `false` otherwise. This should be checked before any other
checks on the entry are made; empty entries may contain stale or invalid data in other fields.

`bool fat_isDirectoryEntryTerminator(fatDirectoryEntry_t *entry)`

Helper function to determine if a given directory entry pointer is the terminator entry in a directory.
Returns `true` if the entry is the terminator, returns `false` otherwise. The terminator entry does not
contain any information itself; it merely acts as a sentinel to inform the directory listing operation that
there are no further valid entities after this one.

#### File Operations

`void afatfs_fprefetch(afatfsFilePtr_t file)`

Requests a best-effort asynchronous read of the cursor's sector when aligned, or the following sector
when the cursor is partway through a sector. The cursor and file operation state remain unchanged.
The hint does nothing for a handle without read access, a busy file, EOF, or a target outside the current
cluster. `afatfs_fread()` invokes it automatically after consuming and releasing a complete sector.
Continue calling `afatfs_poll()` to complete the speculative read.

`bool afatfs_fopen(const char *filename, const char *mode, afatfsFileCallback_t complete, void *user)`

Begin creating or opening a file or directory within the CWD. Note that path separator characters (`/` or `\`)
are NOT supported. Only files or directories immediately within the CWD may be opened. To access files within
a subdirectory, use the `afatfs_chdir` or `afatfs_mkdir` commands to change the CWD.

The `filename` argument must be an ASCII string. FAT16/FAT32 accept up to 255 characters with LFN
enabled and otherwise use 8.3 names; exFAT accepts up to 255 characters. If the special
filename string `"."` is passed, the CWD is opened. This is useful for the `afatfs_findFirst` directory
listing command.

The `mode` argument is a variation on the standard fopen mode strings. Only the following modes strings are
supported:
- `r` - open an existing file for read-only access
- `w` - create a file for write-only access, removing any existing file of the same name
- `a` - create or open a file for write access, setting the file cursor to the end of the existing file (append mode)
- `r+` - open an existing file for read and write access
- `w+` - create or open an existing file for read and write access
- `x` / `x+` - create exclusively, failing if the name already exists; `x+` also permits reads
- `a+` - create or open an existing file for read and write access, setting the file cursor to the end of the existing file (append mode)
- `as` - same as `a`, but uses the freefile optimization if available
- `ws` - same as `w`, but uses the freefile optimization if available

Note that all other mode strings are invalid. In particular, the `b` character is not allowed in AsyncFatFS,
as there is no distinction between text and binary modes. All file data is handled as `uint8_t` bytes and no
special handling or conversion is ever applied to line ending characters.

The `complete` argument requires a user-supplied callback function that will be fired when the fopen is
complete. It uses the following signature:
`void fopen_callback(afatfsFilePtr_t file, void *user)`
If the fopen succeeds, the callback will be called with an open file handle to the requested file. If the
fopen fails, the callback will be called with a `NULL` file handle. The `user` argument can be used to
differentiate the source of the callback, or may be left `NULL` if not needed.

The fopen command itself returns a `false` if the fopen could not be queued, usually due to no available file
handles. The fopen command returns a `true` if the command has been successfully queued for processing. At
this point, the user application must wait for the callback in order to proceed.

Multiple read-only handles may refer to the same file. If either handle requests write or append access,
another open of that file fails through the callback with `NULL`, including while the first handle is
closing. Files with the on-disk read-only attribute cannot be opened for writing or appending. These
checks use the existing open-file table and add no persistent RAM. They do not provide thread synchronization.

`bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)`

This function truncates an existing, open file at its current cursor, preserving the prefix and cursor
position. At EOF it leaves the length unchanged. Seek to zero first to empty the file. Clusters beyond
the new end are released; a partially used final cluster is retained. `file` must have write
permissions. `callback` is a user-supplied callback function that will fire once the truncation operation has
completed. The callback uses the following signature:
`void fopen_callback(afatfsFilePtr_t file, void *user)`
The `file` argument contains the file handle on success, or NULL on a structural filesystem error. The `user`
argument is an opaque pointer that the user can supply to differentiate the source of the callback. `user` may
be `NULL` if not needed. The file itself will remain in a busy state from the successful calling of ftruncate
until the firing of the callback; any reads or writes to the file will fail until the callback is fired.

The return value is `true` when the operation has been queued, or `false` when the operation could not be
queued due to the file being busy.

Truncation and unlink also return `false` without invoking the callback when the handle lacks write/append
permission or the file has the read-only attribute.

`bool afatfs_funlink(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)`

This function deletes the open file referred to by the `file` parameter. It behaves similarly to the `afatfs_ftruncate` command, but its callback differs in its signature:
`void funlink_callback(void *user)`
Note that the callback does not include a file handle, since after unlinking the file will be closed and
therefore have no valid file handle. The `user` pointer is still available to differentiate the source of the
callback if needed.

Deletion removes the entire file regardless of cursor position. Directories must be empty; the root,
current directory and directories with active directory readers cannot be deleted. Use the request-based
`afatfs_unlink`/`afatfs_rmdir` alternatives when an explicit completion result is needed.

`bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)`

This function closes an open file or directory handle `file`. It calls a callback with the following signature
once the close is complete:
`void funlink_callback(void *user)`
The fclose operation returns `true` if the close has been successfully queued, or `false` if the close could
not be queued due to the file being busy.

For FAT16, FAT32 and exFAT, the close callback waits for the file's buffered data and directory metadata
to be written and acknowledged by the backend. FAT32/exFAT also wait for their enabled journal checkpoint. Failed writes
remain pending and are retried; the callback is not issued until synchronization succeeds. Other open
writers may still have partially filled buffers, so closing one file does not authorize powering off the
card. Use clean destruction and `afatfs_canPowerOff()` for that decision.

`uint32_t afatfs_fread(afatfsFilePtr_t file, uint8_t *buffer, uint32_t len)`

This function attempts to read up to `len` bytes of data from an open file. A valid file handle must be passed
to `file`, and a pointer to a byte buffer of at least `len` bytes must be passed to `buffer`. The fread
function does not make use of callbacks; instead, as much data as is available in the cache is copied to the
buffer immediately, then a background read operation is queued to pull the next block of data into cache. As
such, reads may return less than the requested number of bytes. It is up to the user application to check the
return value, which contains the number of bytes actually returned by the read operation. If a zero is
returned, either the file is busy or the cursor has reached the end of the file. To check which is the case,
the user application may call `afatfs_feof`. If the file is not at EOF, the read may be periodically retried
until it succeeds.

Due to the strong coupling between storage blocks and the AsyncFatFS cache system, the most optimal read
pattern is one that always reads in exactly 512-byte increments. If the file pointer is always kept at a
multiple of 512 `(offset % 512 == 0)`, reads of 512 bytes will always read either the full 512 bytes or 0 bytes
(retry later).

`uint32_t afatfs_fwrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t len)`

This function attempts to write up to `len` bytes of data to an open file. A valid file handle must be passed
to `file`, and a pointer to a byte buffer containing at least `len` bytes must be passed to `buffer`. The
fwrite function does not make use of callbacks; instead, data is copied into cached or newly-allocated blocks
until an operation triggers a backend storage read or write operation. The number of bytes successfully
written is returned, however note that this data has not yet been persisted to the backend storage. How much
data can be accepted in a single write is hard to predict; the user application should expect and handle
writes that do not accept the full buffer in one operation. The fwrite function does not require that the
passed buffer remain valid after it has returned. Any bytes that were accepted by the fwrite have been copied
into AsyncFatFS internal buffers during the fwrite call. If a zero is returned, either the file is busy, not
opened in a writable mode, or the filesystem has run out of space. To check if the filesystem is full, use the
`afatfs_isFull` function. If the filesystem is not full, writes may be periodically retried until they
succeed.

`int afatfs_fputc(afatfsFilePtr_t file, uint8_t character)`

Returns 1 if the byte was accepted, or -1 if rejected. On BUSY, poll and retry the same byte. Consult
`afatfs_ferror(file)` for denial or lack of space. Existing code that ignored the former void return
continues to compile, but must inspect the result to avoid losing bytes under backpressure.

`afatfsOperationStatus_e afatfs_fseek(afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence, afatfsFileCallback_t callback, void *user)`

Pass NULL for `callback` and `user` when no completion notification is needed. If supplied, the callback
receives `(file, user)` after a successful seek and may run immediately when no I/O is required.

Seek the file cursor to a given location. The `file` handle must be an open file. `offset` is a signed integer
value whose meaning is determined by the setting of `whence`:
- AFATFS_SEEK_SET - the value of `offset` is an absolute offset from the start of the file
- AFATFS_SEEK_CUR - the value of `offset` is a relative offset from the current location of the file cursor
- AFATFS_SEEK_END - the value of `offset` is a relative offset from _one byte past_ the end of the current file

For all relative offsets, negative values move the cursor towards the beginning of the file. The specific byte
position of the SEEK_END is to allow a seek of `offset = 0` to place the cursor in the correct place to begin
appending data to the end of the file.

Returns a status enum of one of the following values:
- AFATFS_OPERATION_SUCCESS - The seek completed immediately
- AFATFS_OPERATION_IN_PROGRESS - The seek has been queued and the file will be busy until it completes
- AFATFS_OPERATION_FAILURE - The seek could not be queued because the file is busy; retry the seek later

Seeks are always clamped to the current size of the file. It is not possible to use fseek to enlarge a file;
only seeking to the end of the file and performing fwrite or fputc commands can enlarge a file.

Seeks to the beginning of the file (`whence = AFATFS_SEEK_SET`, `offset = 0`) will always complete
immediately. Relative forward seeks that do not cross a 512-byte block boundary will also always complete
immediately. Absolute seeks and backwards relative seeks will generally have to queue operations in order to
complete. The file will remain busy until the seek completes, so fread or fwrite operations can be attempted
and will succeed once the seek is complete. See the [seeking behavior](#file-seeking-behavior) limitation note
for the performance implications of the different seek types.

`bool afatfs_ftell(afatfsFilePtr_t file, uint32_t *position)`

Request the current position of the file cursor of an open file. `file` must be an open file handle.
`position` is a variable passed by pointer, which the ftell call will fill with the current cursor offset.
Returns `true` if the operation succeeded, or `false` if the file is busy (in which case the `position`
variable will be left untouched).

`bool afatfs_fGetSize(afatfsFilePtr_t file, uint32_t *size)`

Get the logical file size of an open file. If the call returns `true`, the logical size (in bytes) has been
placed in the `size` variable. If the call returns `false`, the file was busy and no size was reported; retry
the call later. Note that the logical size is based on cached information held in the AsyncFatFS file
descriptor, and is not guaranteed to have been persisted to storage.

`bool afatfs_feof(afatfsFilePtr_t file)`

Check whether the file's cursor is currently at the end-of-file point (one byte past the last byte of the
file). Returns `true` if the file is at EOF, `false` otherwise. This function does not check the busy state of
the file before returning. Calling this function when a file is busy is not guaranteed to produce a useful
result, as the file cursor may move during the operation.

#### Filesystem Operations

`bool afatfs_isFull(void)`

Returns `true` if the filesystem has no free clusters available to allocate. Will also return `true` if the
`AFATFS_USE_FREEFILE` optimization is enabled and the freefile has been completely exhausted. Returns `false`
otherwise. Note that just because the filesystem reports as full does not mean all write operations will fail.
Writes that overwrite existing portions of files, or writes that do not require allocating an additional
cluster to a file may still succeed.

`uint32_t afatfs_getFreeBufferSpace(void)`

Returns reusable cache capacity in bytes plus the remaining bytes from the cursor to the end of each
sector locked for writing by an idle public file handle. Clean sectors retained by readers or metadata
operations, dirty sectors awaiting flush, and buffers involved in I/O are excluded. Each cache sector is
counted once. Returns zero unless the filesystem is ready and writable. This scans existing descriptors
and handles; it adds no persistent RAM, allocation or I/O.

The total covers all open files: a partially used sector belongs to its current writer and cannot be
transferred to another file. It is not a guarantee that one `fwrite` can consume the whole total. Allocation,
metadata journaling and reads needed to locate or merge sectors may still make the write yield.

`uint32_t afatfs_getFileFreeBufferSpace(afatfsFilePtr_t file)`

Returns the reusable shared cache capacity plus the remaining bytes in this file's locked write sector.
Partially filled sectors belonging to other files are excluded. Returns zero for `NULL`, a closed handle,
a directory, a handle without write access, a busy file, or a filesystem that is not ready and writable.
Pass a valid library handle or `NULL`; arbitrary or dangling pointers are not supported.

This query uses the existing cache descriptors and file state, without additional persistent memory,
allocation, I/O or cursor changes. The shared capacity is not reserved by the query, and allocation,
journaling or required reads may still cause `fwrite` to accept fewer bytes. Results for different files
overlap in their shared capacity and must not be summed.

`uint32_t afatfs_getContiguousFreeSpace(void)`

> [!NOTE]
> This function is only available if `AFATFS_USE_FREEFILE` is enabled

Returns the size of the freefile (in bytes). This corresponds to the amount of data that can be written to
files via the fast-path optimization provided by the freefile mode.

`afatfsFilesystemState_e afatfs_getFilesystemState(void)`

Returns the global state of the filesystem:
- `AFATFS_FILESYSTEM_STATE_UNKNOWN` - AsyncFatFS has not been initialized; call `afatfs_init`
- `AFATFS_FILESYSTEM_STATE_INITIALIZATION` - AsyncFatFS is still performing the initial filesystem opening and
sanity checks
- `AFATFS_FILESYSTEM_STATE_READY` - Initialization is complete and the filesystem may be used
- `AFATFS_FILESYSTEM_STATE_FATAL` - Initialization failed; see the `afatfs_getLastError` function for details
- `AFATFS_FILESYSTEM_STATE_STOPPING` - Clean filesystem writes are complete; backend shutdown is pending
- `AFATFS_FILESYSTEM_STATE_STOPPED` - `afatfs_destroy(false)` completed; `afatfs_canPowerOff()` is true
- `AFATFS_FILESYSTEM_STATE_CLOSING` - File, cache, journal or clean-volume writes are still being drained
- `AFATFS_FILESYSTEM_STATE_REMOVED` - Media was removed; reinitialize after insertion and reopen handles

`afatfsError_e afatfs_getLastError(void)`

Returns the last reported error, including retryable I/O errors while mounted or closing. It does not
clear the error. A later successful retry can leave the diagnostic recorded until another error or a
mount/reset/shutdown clears it; the pending-operation state determines whether work is complete.
The available errors are:
- `AFATFS_ERROR_NONE` - No error has occurred
- `AFATFS_ERROR_GENERIC` - An unknown error has occurred (most often caused by a full filesystem)
- `AFATFS_ERROR_BAD_MBR` - No supported MBR partition or usable GPT layout could be detected
- `AFATFS_ERROR_BAD_FILESYSTEM_HEADER` - A valid FAT16 or FAT32 volume could not be detected
- `AFATFS_ERROR_RECOVERY_REQUIRED` - The journal/volume cannot be trusted for normal writes
- `AFATFS_ERROR_BACKEND_IO` - A backend read/write callback failed, a write was rejected with
  `SDCARD_OPERATION_FAILURE`, or backend stop failed. The pending operation is retained for retry.
- `AFATFS_ERROR_MEDIA_REMOVED` - The old media session was cancelled after a removal/replacement event

The boolean read-submission API cannot distinguish a rejected read from `BUSY`; both are retried without
advancing the operation. Normal `BUSY` does not report an I/O error. A journal I/O failure retries the
same prepared header, descriptor, before-image or recovery sector until acknowledged successfully.
A journal CRC/structure failure is different: it reports recovery required, freezes runtime journal work,
and does not authorize shutdown. `afatfs_destroy(true)` is an explicit lossy discard, not confirmation
that power can be removed safely.

### Backend Driver API

These are the functions that AsyncFatFS itself calls to interact with the storage backend. AsyncFatFS uses the
LBA storage model, where blocks are addressed by sequential 32-bit index numbers starting at index zero.
Blocks are always 512 bytes long. This matches the standard behavior of SD cards. To implement a backend
driver, include the `lib/sdcard.h` file and implement all of the mandatory functions described below.

Note that there is no periodic processing call made to the backend driver. If your driver requires such a
call, it will need to be implemented and called outside of AsyncFatFS. A good time to do such a call would be
directly before the `afatfs_poll` call.

#### Mandatory Functions

The user application or hardware abstraction layer must implement the following three functions for AsyncFatFS
to call:

`bool sdcard_readBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)`

Read a single 512-byte block of data at index `blockIndex` from the backend storage and transfer it to the
buffer supplied in `buffer`. A callback function pointer is supplied by AsyncFatFS in the `callback` argument
along with an opaque `callbackData` identifier; this pointer and identifier pair must be stored by the backend
driver and called once the full 512-byte block has been successfully transferred to the buffer. The callback
function signature is the following:
`void sdcard_operationCompleteCallback_c(sdcardBlockOperation_e operation, uint32_t blockIndex, uint8_t *buffer, uint32_t callbackData)`

When calling the callback, supply `SDCARD_BLOCK_OPERATION_READ` to the `operation` argument, the requested
block index to the `blockIndex` argument, the pointer to the buffer that was originally supplied to the
readBlock call (or a `NULL` if the read operation failed), and the opaque `callbackData` value that was also
supplied to the original readBlock call.

The readBlock function must return a boolean value based on whether or not it has accepted the requested read
operation. If it returns `true`, it is committing to perform the read operation and to call the callback at a
later time. If it returns `false`, it is not accepting the read operation at this time. The backend should not
call the callback it it returned `false`. Returning `false` is interpreted as a busy state, not an error.
AsyncFatFS may retry the same readBlock call again at a later time.

The readBlock function itself must return quickly and therefore should not block; if the data is not
immediately available, the function should queue the read and perform it asynchronously. Once a read operation
is accepted, the contents of `buffer` are owned by the backend driver and may be written to at any time and
in any order until the callback function is called, after which `buffer` may not be altered further. The
callback function passed to the backend driver may safely be called at any time.

The backend driver may queue multiple reads if it is capable of doing so, however each read operation that is
accepted must individually fire the callback it was given with the appropriate `buffer` and `callbackData`
arguments (see above). It is acceptable to only be able to queue a single read or write operation at a time.

`sdcardOperationStatus_e sdcard_writeBlock(uint32_t blockIndex, uint8_t *buffer, sdcard_operationCompleteCallback_c callback, uint32_t callbackData)`

Write a single 512-byte block at index `blockIndex` from the memory `buffer` to the backend storage. A
callback function `callback` and opaque identifier `callbackData` are supplied for the backend to call once
the write operation has completed. As with the readBlock call, the writeBlock call must not block. Return
`true` to indicate that the operation has been queued for later processing, or `false` to indicate that the
backend cannot accept the write operation at this time. The callback signature is identical to the signature
in the readBlock documentation above, except that the backend should supply `SDCARD_BLOCK_OPERATION_WRITE` to
the `operation` argument. The other arguments behave identically to the callback in readBlock.

The contents of `buffer` are guaranteed to be stable until the callback is fired, after which they may not be
accessed anymore. AsyncFatFS expects that the block has been fully persisted to storage once the callback has
been fired; there is the risk of filesystem corruption otherwise.

`bool sdcard_poll(void)`

This should be a very lightweight function that returns the busy status of the storage backend. If the backend
is able to accept new read or write operations, return `true`. If the backend is busy and new operations
cannot be accepted right now, return `false`.

#### Optional Functions

`sdcardOperationStatus_e sdcard_beginWriteBlocks(uint32_t blockIndex, uint32_t blockCount)`

This function is used by AsyncFatFS to indicate the beginning of a multi-block write operation. The size of
potential multi-block writes (in number of blocks) will never be less than the value of
`AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT`.
The cache also uses this feature without `AFATFS_USE_FREEFILE` when dirty, unlocked sectors are physically
consecutive in their existing write order and meet `AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT`. These runs
contain at most `AFATFS_NUM_CACHE_SECTORS` blocks. Contiguous append through freefile may announce a
longer range of future data sectors, up to 65535 blocks. Metadata never announces uncached future sectors:
pre-erasing them could destroy contents before their undo records are durable.

The library retries `SDCARD_OPERATION_BUSY` and `SDCARD_OPERATION_FAILURE` without submitting the data
block, retaining the pending buffers. Failure also records `AFATFS_ERROR_BACKEND_IO`. Sync, close and
shutdown continue to wait for completion and the end of the multi-block operation.

Background flushing is deferred only during selected journal-protected metadata production phases:
LFN/exFAT entry publication, FAT32 allocation, chain conversion and chain release. Cache pressure and
explicit durability barriers override this deferral; allocation blocked on a journal checkpoint allows
background flushing to continue. An open but inactive file does not defer flushing.
During LFN free-slot search/publication, fewer than two reusable cache pages also resumes flushing:
the FAT walk and target entry must not continually evict each other's scratch page.

See the [I/O batching measurements](../tests/IO_BATCHING.md) for operation counts, multi-block coverage,
unchanged RAM measurements and interrupted-write validation. Multi-block writes reduce the number
of write-sequence starts, not the number of payload sectors. Actual time saved depends on the card
and backend; the simulated backend does not measure physical SD timing.
The [native Windows FAT32 validation](../tests/WINDOWS_FAT32_IO_BATCHING.md) covers concurrent LFN
operations, native parallel writes, multi-block instrumentation and CHKDSK on isolated test VHDs.

`void sdcard_setProfilerCallback(sdcard_profilerCallback_c callback)`

If `AFATFS_USE_INTROSPECTIVE_LOGGING` is enabled, this function will be called during `afatfs_init` to provide
the backend driver with the function pointer for the profiling function. The backend driver should call this
function once for every read or write operation it successfully executes. The callback signature is as
follows:
`void afatfs_sdcardProfilerCallback(sdcardBlockOperation_e operation, uint32_t blockIndex, uint32_t duration)`
The `operation` argument should be one of `SDCARD_BLOCK_OPERATION_READ` or `SDCARD_BLOCK_OPERATION_WRITE`. The
`blockIndex` argument should be the index of the given operation, and the `duration` argument is a
backend-calculated value of how long the operation took to complete. The units of `duration` are not
specified, however microseconds may be a good choice.

Note that because the logging writes to a file on the storage backend itself, having logging enabled creates a
sort of "write amplification". Every 32 operations logged will cause at least one write operation and possibly
multiple read operations (around a 3-10% overhead). If this is a concern, it may be necessary to do
proportional logging, where only a certain percentage of operations are logged.
