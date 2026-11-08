# Card removal and reinsertion validation

`tests/test_exfat.c` extends the delayed synthetic backend with presence events and
synchronous cancellation. The suite runs the original cache/file/journal machines
through removal, reinsertion and clean teardown. It does not access physical cards.

## Integration

The existing boolean readiness API cannot identify a physical removal. The driver
or application must report the event with `afatfs_notifyMediaRemoved()`, serialized
with normal library calls and before any old request can reach an inserted card.
The notification enters `REMOVED`, invalidates old file handles/callbacks and
revokes any earlier power-off authorization, including one issued for a previous
cleanly stopped card. Reinsertion alone does not resume the old session. Initialize
the backend and call `afatfs_init()` to mount the inserted volume afresh.

The existing `sdcard_reset()` entry point now has a defined cancellation contract:
stop DMA and queued media/buffer accesses synchronously, do not start new I/O, and
preserve the original callback data for any late software-only completion. The
cache and journal tag requests with one 32-bit generation counter that survives
mount-state clearing. Stale success and failure notifications are ignored. Reset
also cancels work before reinitialization or a forced teardown, avoiding transfer
of old requests to new buffers/media. File handles and the journal format are
unchanged; there is no heap allocation or per-handle generation storage.

## Executed matrix

The three builds (default, FAT-only, exFAT/freefile) run **186 removal scenarios**:

- **168 exFAT cases:** 14 removal points, three write-interruption modes, two
  reinsertion choices, in each of the two exFAT-enabled builds.
- **18 FAT cases:** pending prefetch or an in-place write on an already allocated
  file, across FAT16 with one/four sectors per cluster and FAT32, in all three builds.

The 14 exFAT points are initialization read, prefetch, application-data write,
journal read, undo-image write, descriptor write, record-count publication, commit,
ownership release, clean boot marker, backend stop, already-stopped state,
recovery read and recovery write. The last two remove the card a second time
during replay of an already interrupted transaction.

Each pending write is either cancelled without persistence or torn after updating
its first or last 256 bytes. Each case reinserts either the same image immediately
or a second independently prepared image first. The replacement deliberately has
the same geometry and serial as the original, but different file data, so safety
does not depend on identifying a card from those values.

Assertions verify:

- No success/power-off authorization, no new block requests, and no writes while
  removed, even when old file APIs or delayed completions are attempted repeatedly.
- Retired success and failure callbacks do not alter new-session status, even
  when replayed during every step of a new mount.
- The replacement image stays byte-for-byte unchanged before explicit mounting;
  it contains its own expected data and none of the original transaction's files.
  New writes on it succeed normally after mounting.
- The original image recovers its journal after reinsertion, including a second
  interruption of replay. An untouched committed file always retains its exact
  length/data, and transactions whose close callback completed retain their full
  newly written contents. Cancelled application callbacks cannot revive.
- FAT reads after reinsertion match the bytes actually left on the medium by a
  torn in-place write; volatile old data is never resent. Unrelated data is intact.
- Power-off authorization is granted only after the newly mounted session finishes
  clean teardown with no remaining I/O.

## Automatic recovery of the eight shutdown handoffs

The original matrix had eight read-only outcomes: clearing the boot dirty bit was
interrupted after journal ownership had already been released. The latest valid
header was unowned/empty while the boot region was dirty. That state is also
possible after an interrupted external writer, so it cannot safely be accepted
based on the older owned header or an empty log alone.

The shutdown order is now: drain data and metadata, commit the empty journal while
still owned, clear the boot dirty marker and wait for its ACK, release ownership
and wait for that ACK, then complete backend shutdown. The existing owned-empty
mount recovery handles cuts through this new order; no new journal format, RAM
state or independent recovery engine is introduced. This is not a permission to
ignore an unowned dirty volume.

All eight formerly read-only interruption points now recover writable. Tests also
remove the card again during header reading of that recovery, create `RESUME.BIN`
with 73 known bytes, cleanly stop, remount and verify those bytes plus previously
committed file data. Replacement-card cases still complete a separate session on
the other card before recovering the original, so recovery does not depend on a
retained in-memory identity hint. All 186 removal regressions pass with zero
read-only outcomes for the new shutdown order.

Sixteen negative checks (eight in each exFAT build) retain read-only protection
with zero writes: legacy/external unowned-dirty volumes, an externally damaged file
entry, CRC-valid wrong geometry/serial, invalid header CRCs, active logs with a clean
boot or without ownership, nonempty inactive logs, and a newer released header
paired with an older owned active header. Legacy ambiguous images remain read-only.
Recovery must precede any external writer after unexpected removal, even if the
boot dirty bit has already cleared; an incomplete library session can still own
the journal. Completed normal shutdown releases ownership for external use.

Failure-injection cases additionally reject or partially write the clean boot
marker repeatedly and verify that its durable journal header stays owned and
empty until the boot write succeeds. `canPowerOff` stays false during those retries
and during ownership release. The backend-failure regression now contains 80 cases.

## Reproduce and limits

```powershell
./tests/run_exfat_tests.ps1 -CompileLegacy
```

Current runtime output is in `tests/build/handoff-recovery-validation-final.log`;
the summary and binary/source hashes are in `tests/build/handoff-recovery-report.json`.
The earlier `media-removal-report.json` is the historical eight-guard baseline. The eight
legacy image-based test programs also compile with the new backend reset function.
Existing power-cut, backend-failure and concurrent-shutdown regressions remain in
the same suite.

These are simulated presence, I/O and callback events. The actual driver must
implement detection and cancellation correctly; generation checks cannot stop a
DMA engine that continues writing into retired memory. Unacknowledged RAM data
can be lost on physical removal, in-place overwrites are not atomic, and FAT16/
FAT32 do not acquire exFAT journal guarantees. A card interrupted before initial
journal provisioning, in a mount-ownership gap, or under the older shutdown order
can require external repair. This change does not infer safety for such old images.
