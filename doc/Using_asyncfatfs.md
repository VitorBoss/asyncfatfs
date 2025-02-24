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

#### MBR and Partitions

AsyncFatFS only knows how to read MBR-formatted disks. It only supports using partition 1, which must be
pre-formatted as FAT16 or FAT32. It is unable to handle GPT-formatted partition tables, nor unpartitioned
disks. This, along with the 512-byte sector requirement, limits the maximum disk size to 2 TiB.

#### Paths and the Current Working Directory

AsyncFatFS does not support opening files by absolute path (e.g. `/path/to/file.txt`). Instead, it uses the
concept of a Current Working Directory (CWD), within which all directory-specific operations occur. Note that
the CWD is global to the entire library(!). Managing the global CWD is a task left to the library user.

Once a file is opened, the file handle may be retained even if the CWD gets changed. Thus, it is possible to
have multiple files from different directories opened at the same time.

#### File Creation and Modified Datetimes

AsyncFatFS does not currently support setting or updating the creation or modified times of files within the
filesystem. It uses a hard-coded fallback datetime of 2015-12-01 00:00:00 for all files.

#### Freefile

The freefile optimization only works with one file at a time. Additionally, the special FREESPAC.E file must
be created and remain on the volume.

#### No Long Filename Support

AsyncFatFS does not support reading or setting long filenames. All files and directories must be referred to
by their "8.3" filenames.

#### No Error Detection / Handling

AsyncFatFS does not have any ability for the backend storage driver to report errors or failures. The safest
course of action if an error is produced in the backend driver is to not call the requested AsyncFatFS
callbacks. This results in the file deadlocking, and may deadlock the entire AsyncFatFS library, but will
prevent any further damage to the filesystem. Robust user code should include its own timeouts for any file
operations conducted through AsyncFatFS. Alternatively, user code may communicate with the backend storage
driver and treat any IO errors as fatal and discontinue any active file operations.

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

Choose the fileName that the freespace feature uses for its special reservation file. Note that this file
never contains user data, only reserved blocks to be used when writing other files. The fileName must respect
the FAT "8.3" name limitation.

### User API

This section describes the user-facing API of AsyncFatFS. The backend driver API that AsyncFatFS itself calls
is described [in the next section](#backend-driver-api).

#### General Notes about API Usage

All functions in the API are non-blocking. Because of this requirement, most operations do not succeed
immediately; instead, they merely queue up the operation for later processing. It is important to check the
return values for all API calls and handle them appropriately. Some calls will need to be called multiple
times until they report success. Other calls require you to register a callback that will be fired when the
operation completes.

> [!IMPORTANT]
> Callbacks will sometimes be directly called by the API call that initially
> registers them before the call returns. Be aware of this when altering
> state machine states from within callback functions.

File handles must be treated as opaque pointers. Once a file has been closed, any references to its handle
that were kept by the user application must be discarded. File handles are recycled internally, so no
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
written to the backend, or `false` if there are still pending writes. Only ensures data sent by operations
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

#### Directory Operations

`bool afatfs_mkdir(const char *fileName, afatfsFileCallback_t complete, void *user)`

Create or open a directory in the CWD with the fileName supplied. Note that the fileName must respect the
FAT 8.3 limitation. If the directory already exists, the operation will proceed as if it had created the
directory, but the files within the directory will not be affected. Returns `true` if the operation has
begun, or `false` if there are no free file descriptors available to start the operation. The user must supply
a callback function with the following signature:
`void mkdir_callback(afatfsFilePtr_t file, void *user)`
This callback will be fired with a file handle pointing to the new or existing directory if the operation
succeeded, or it will be fired with a `NULL` file handle if the mkdir failed. Failures could be due to the
filesystem being full or the CWD hitting a FAT limit. The `user` pointer may be used to identify the source of
the callback, or may be left `NULL` if not needed. If a valid directory handle is supplied to the callback
(`afatfsFilePtr_t file` is not `NULL`), the user application must close the directory once it is no longer
needed.

`bool afatfs_chdir(afatfsFilePtr_t dirHandle)`

Change the CWD to the directory supplied by the `dirHandle` argument. You must successfully call
`afatfs_fopen` on the directory before passing it to to this function, and you must wait for `afatfs_chdir` to
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

`bool afatfs_fopen(const char *fileName, const char *mode, afatfsFileCallback_t complete, void *user)`

Begin creating or opening a file or directory within the CWD. Note that path separator characters (`/` or `\`)
are NOT supported. Only files or directories immediately within the CWD may be opened. To access files within
a subdirectory, use the `afatfs_chdir` or `afatfs_mkdir` commands to change the CWD.

The `fileName` argument must be an ASCII string that follows the FAT 8.3 fileName convention. If the special
fileName string `"."` is passed, the CWD is opened. This is useful for the `afatfs_findFirst` directory
listing command.

The `mode` argument is a variation on the standard fopen mode strings. Only the following modes strings are
supported:
- `r` - open an existing file for read-only access
- `w` - create a file for write-only access, removing any existing file of the same name
- `a` - create or open a file for write access, setting the file cursor to the end of the existing file (append mode)
- `r+` - open an existing file for read and write access
- `w+` - create or open an existing file for read and write access
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

`bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback, void *user)`

This function truncates an existing, open file to zero length. `file` must be an open file handle with write
permissions. `callback` is a user-supplied callback function that will fire once the truncation operation has
completed. The callback uses the following signature:
`void fopen_callback(afatfsFilePtr_t file, void *user)`
The `file` argument to the callback always contains the file handle of the file that was truncated. The `user`
argument is an opaque pointer that the user can supply to differentiate the source of the callback. `user` may
be `NULL` if not needed. The file itself will remain in a busy state from the successful calling of ftruncate
until the firing of the callback; any reads or writes to the file will fail until the callback is fired.

The return value is `true` when the operation has been queued, or `false` when the operation could not be
queued due to the file being busy.

`bool afatfs_funlink(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)`

This function deletes the open file referred to by the `file` parameter. It behaves similarly to the `afatfs_ftruncate` command, but its callback differs in its signature:
`void funlink_callback(void *user)`
Note that the callback does not include a file handle, since after unlinking the file will be closed and
therefore have no valid file handle. The `user` pointer is still available to differentiate the source of the
callback if needed.

`bool afatfs_fclose(afatfsFilePtr_t file, afatfsCallback_t callback, void *user)`

This function closes an open file or directory handle `file`. It calls a callback with the following signature
once the close is complete:
`void funlink_callback(void *user)`
The fclose operation returns `true` if the close has been successfully queued, or `false` if the close could
not be queued due to the file being busy.

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

`void afatfs_fputc(afatfsFilePtr_t file, uint8_t c)`

> [!CAUTION]
> This function does not check or return errors. Characters will be silently
> dropped if the file is busy when this function is called!

Quick and dirty shortcut to write a single character into an open file. Will always succeed if the file cursor
is within a single 512-byte block. The first write to the next block when a block boundary will also succeed
if the file is not busy, but this operation may put the file into a busy state, causing subsequent fputc
operations to silently fail until the file is no longer busy. Use this function with caution.

`afatfsOperationStatus_e afatfs_fseek(afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence)`

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

This function returns a pessimistic estimate of the amount of free buffer space (in bytes) available for file
writes. The estimate is pessimistic because it does not account for any partially-filled sectors of
currently-open files. Note that it is not guaranteed that a single fwrite call will be able to consume this
many bytes. If the file is being appended to, or if the needed cluster maps are not currently in cache, writes
may be split up with block reads to find or allocate clusters to the target file.

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

`afatfsError_e afatfs_getLastError(void)`

If the filesystem state is `AFATFS_FILESYSTEM_STATE_FATAL`, this function may be called to get a more detailed
error code. The available errors are:
- `AFATFS_ERROR_NONE` - No error has occurred
- `AFATFS_ERROR_GENERIC` - An unknown error has occurred (most often caused by a full filesystem)
- `AFATFS_ERROR_BAD_MBR` - A valid MBR sector could not be detected
- `AFATFS_ERROR_BAD_FILESYSTEM_HEADER` - A valid FAT16 or FAT32 volume could not be detected

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
`AFATFS_MIN_MULTIPLE_BLOCK_WRITE_COUNT`, and will never be more than the value of `AFATFS_NUM_CACHE_SECTORS`.
Currently, the only way multi-block writes can trigger is when `AFATFS_USE_FREEFILE` is enabled. This function must return either `SDCARD_OPERATION_SUCCESS` or `SDCARD_OPERATION_BUSY`.

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
