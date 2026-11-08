## AsyncFatFS

AsyncFatFS is a minimalist, fully asynchronous FAT16 / FAT32 / exFAT file IO library. When paired with a simple
asynchronous read-block/write-block API for an SD card or other storage backend, this library provides file IO
primitives (e.g. fopen, fread, fseek) in a fully non-blocking manner. All API calls either provide a callback
to notify you of operation completion, or return a status code that indicates that the operation should be
retried later. This allows AsyncFatFS to run well on embedded systems that do not have an operating system or
threading capabilities.

A special optional feature of this filesystem is a high-speed contiguous append file mode, provided by the "freefile"
support. In this mode, the largest contiguous free block on the volume is pre-allocated during filesystem 
initialisation into a file called "freespac.e". One file can be created which slowly grows from the beginning 
of this contiguous region, stealing the first part of the freefile's space. Because the freefile is contiguous, 
the FAT entries for the file need never be read. This saves on buffer space and reduces latency during file
extend operations.

exFAT is enabled in the development configuration. It uses the existing asynchronous sector cache and file
operation state machines, including allocation bitmap updates, entry-set checksums, fragmented metadata,
and reading/extending existing `NoFatChain` files. New exFAT files use FAT chains. The public API remains
ASCII and 32-bit: exFAT names may contain up to 255 ASCII characters; FAT16/FAT32 retain 8.3 names.
exFAT supports MBR and GPT partition tables, including GPT CRC checks and backup-table fallback, within
the existing 32-bit sector address range. FAT16/FAT32 retain their MBR partition support.
See the [exFAT scope and limits](doc/Using_asyncfatfs.md#exfat-development-support) before integrating it.

The [extended API](doc/Extended_API.md) adds unified metadata and directory enumeration,
rename/attributes/timestamps, labels, exact free-space queries, contiguous preallocation,
text/stream helpers, formatting and partitioning within the existing limits. Its asynchronous
workspace belongs to the caller. Truncation now ends the file at the current cursor; seek to
zero to empty it. `afatfs_fputc` returns whether the byte was accepted.

Run the self-contained exFAT and FAT regression tests with `make test-exfat`, or on Windows with Visual
Studio C/C++ tools installed, `./tests/run_exfat_tests.ps1`. These tests generate media in memory and inject
delayed block I/O; they do not require the external images used by the older test suite.
Add `-CompileLegacy` to the PowerShell command to also compile the eight image-based tests against the current API.
External images are covered by a separate [interoperability suite](tests/EXFAT_INTEROP.md), using
mkfs.exfat and FATtools for formatting and fsck.exfat plus FATtools for independent verification.
The [native Windows suite](tests/WINDOWS_VHD_VALIDATION.md), `./tests/run_windows_vhd_tests.ps1`,
also covers Windows-formatted VHDs, native file I/O and read-only CHKDSK checks. It requests UAC elevation.
exFAT now reserves a hidden `AFATFS.JNL` metadata undo journal on clean volumes and replays interrupted
updates before accepting writes. The [power-cut suite](tests/EXFAT_POWERCUT.md),
`./tests/run_exfat_powercut.ps1`, checks write cuts, torn sectors and interrupted recovery.
The [removal suite](tests/MEDIA_REMOVAL_VALIDATION.md) also checks removal/reinsertion, replacement
cards and retired completions. Drivers must call `afatfs_notifyMediaRemoved()` on removal and implement
the `sdcard_reset()` cancellation contract before mounting another card.

### Implementing AsyncFatFS

The API for both AsyncFatFS itself, as well as the required driver functions for the storage backend, is
[documented here](doc/Using_asyncfatfs.md).

You can use the testcases in the `test/` directory as a guide to how to use the filesystem. AsyncFatFS is also used in
Cleanflight / Betaflight's "blackbox" logging system: [filesystem consumer code](https://github.com/betaflight/betaflight/blob/master/src/main/blackbox/blackbox_io.c), 
[SDCard SPI driver code](https://github.com/betaflight/betaflight/blob/master/src/main/drivers/sdcard_spi.c).

You'll notice that since most filesystem operations will fail and ask you to retry when the card is busy, it becomes 
natural to call it using a state-machine from your app's main loop - where you only advance to the next state once the 
current operation succeeds, calling afatfs_poll() in between so that the filesystem can complete its queued tasks.
