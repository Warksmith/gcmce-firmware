# Folder card internals: synchronization and recovery

How a folder card works, for anyone changing it. The file formats are in
[gci-metadata-format.md](gci-metadata-format.md).

## Where things are

| File | Responsibility |
| --- | --- |
| `gc_fs.c` | The GameCube filesystem in PSRAM: header, directory, BAT, checksums, validation |
| `gc_gci.c` | `.gci` checks, save identity, host names, CRC-32 |
| `gc_folder_card.c` | The public API and the state machine around the other files |
| `gc_folder_import.c` | Mounting: scanning the folder and building the card |
| `gc_folder_sync.c` | Reconciliation, transactions, recovery |
| `gc_folder_meta.c` | `card.dat`, `saves.dat` and `journal.dat` |
| `gc_folder_port_pico.c` | The platform: PSRAM, clock, and the write generation |
| `gc_cardman.c` | Selects the backend. Calls the above and runs nothing of its own |
| `gc_dirty.c` | Hands the console's write marks to the folder card |

`gc_folder_port.h` is all the engine knows of the hardware, which is how the same code runs
in the host tests.

## The GameCube filesystem

Blocks are 8 KiB. Block 0 is the header, 1 and 2 the directory, 3 and 4 the BAT, and data
starts at block 5. The console keeps two copies of the directory and of the BAT, updates
whichever is older, and treats the one with the higher update counter as current, compared
as a signed 16-bit difference. A directory has 127 entries of 64 bytes.

Order of the console's updates, from libogc:

| Action | Order |
| --- | --- |
| Create | BAT, then directory |
| Delete | Directory, then BAT |
| Write | Data blocks in place, then directory (time stamp) |
| Format | Header, both directories, both BATs |

So a filesystem read at an arbitrary moment can be between two steps of one operation. That
is why the engine only reads it when the console has gone quiet, and checks that it stayed
quiet.

## Mounting

Runs on core 0 in slices of at most 20 ms. Core 1 waits on `gc_cardman_is_idle()`, so the
console sees nothing until the card is complete. A failed card simply never gets there.

1. Create `.flippermeta` and `stage` if needed. Nothing outside `.flippermeta` is written.
2. Recover: finish or discard a transaction left by a power cut.
3. Read `card.dat`, and `saves.dat` for slots and host names.
4. Scan for `.gci` files and check each: size, entry, identity, capacity, name. Any problem
   fails the mount after listing every problem found.
5. Write the header, then each save's blocks contiguously, into PSRAM.
6. Commit the directory and BAT copies and validate the result from PSRAM.
7. If what was found differs from `saves.dat`, commit a new one through the journal.

A save keeps the directory slot it had in `saves.dat`, so that a card looks the same from
mount to mount. Only metadata is written at mount, and a second mount of an unchanged folder
writes nothing at all. A test holds that to zero operations.

## Noticing changes

The console's writes already mark 512-byte segments in the dirty heap on core 1.

- `gc_dirty_mark()` also bumps `gc_dirty_generation`, with no lock on the time-critical path.
- `gc_dirty_task()` on core 0 hands a folder card's marks to
  `gc_folder_card_note_segment()` and does nothing else, so it never touches SD or PSRAM for
  a folder card.
- Marks become a bitmap of changed blocks, `dirty_now`. A pass moves it into `dirty_scan`,
  which only a committed pass clears, so an abandoned pass loses nothing.

## A synchronization pass

A pass needs all of: the generation moved since the last commit, no marks waiting, and
1.5 seconds since the last change with the console not busy. Card switching and shutdown force
a pass without the wait.

**Scan.** The header, directory and BAT are read and validated. A card that fails validation
is `GF_S_INVALID` and is not saved: it may be a moment the console is halfway through, or
junk. A later write gets it re-examined. Each entry is matched to the committed saves by
identity. A save needs staging if it is new, or its entry, block count or any block changed.

**Stage.** Each such save is written to `stage/<slot>.bin` from PSRAM in chain order, and read
back. A save whose bytes turn out to be the same is dropped. Before every page the generation
is checked, and any change abandons the pass, because the file could mix two versions.

**Commit.** `saves.dat`, and `card.dat` if the header changed, are written to `stage/` and read
back. The journal is written, the generation checked once more, and the trailer written. The
trailer is the commit point. Nothing before it has touched a save, so giving up costs nothing.

**Apply.** The operations run in order: removals first, then saves, then metadata. The journal
is deleted, then the stage files. PSRAM is not read during this phase, so writes that arrive
now simply belong to the next pass.

A pass commits the generation it scanned, not the current one, so a write during apply leaves
the card dirty and starts another pass.

## What a power cut leaves

| Cut during | On SD | Next mount |
| --- | --- | --- |
| Console writing, before a pass | The last committed state | Mounts it. The newer writes are lost |
| Scan or stage | Last committed state, stray stage files | Sweeps them |
| Commit, before the trailer | Same, plus a journal with no trailer | Removes it and sweeps |
| Apply | Some operations done, journal complete | Checks the stage files, redoes all of it |
| Deleting the journal | Everything done | Removes the rest |

There is one instant where the result changes from old to new: the trailer. A test cuts the
power at every operation of a transaction, and at every operation of the recovery that follows
such a cut. Every one of them comes back as exactly the old or exactly the new state.

**What can be lost** is the last pass's worth of writes that had not reached the trailer: the
game said it saved and the old version comes back. That window is at least the 1.5 seconds of
quiet plus the SD time, and it is the price of working from PSRAM. It is the same kind of
window a `.raw` card has, a little longer.

## Failures while running

| Failure | What happens |
| --- | --- |
| SD error before the trailer | Stage files swept, `GF_S_SAVE_FAILED`, retried every 5 s |
| SD error in apply | Journal stays, `GF_S_SAVE_FAILED`, retried from its first operation |
| Trailer written but reported failed | The journal cannot be told apart from a good one, so apply goes on |
| Read-back differs | As an SD error. A stage file is never committed unverified |
| Console writes during a pass | Pass abandoned, starts over after the next quiet time |
| SD card full | As an SD error. The saves on the card are unchanged |

Switching cards flushes first and refuses to switch while changes cannot be saved.
`gc_folder_card_close()` refuses the same way. Those changes exist only in PSRAM, so a refusal
is the safe outcome. A power cycle then gives up on them, which the display warns about.

## Recovery rules

Each of these exists because of a way to lose data.

- **No trailer means nothing happened.** The journal is removed. No operation of an uncommitted
  transaction ever ran.
- **A size with a trailer means it happened.** A read error, or a CRC mismatch, then says nothing
  about the journal, which might be good. The card stays hidden with `SD error` and the journal
  stays. A later mount tries again.
- **Check before touching.** Every stage file of a committed journal has its size and CRC
  checked before the first operation runs. If one is wrong the mount fails with `Recovery
  required` and everything is left as it is, because half a transaction is better examined than
  guessed at.
- **Operations repeat.** A replace copies over whatever is there. It does not use rename, since
  a stage file must survive until the journal is gone. A remove accepts a missing file. A trash
  accepts a missing source, which is safe because a transaction's new saves never use a name
  that any committed save has.
- **Metadata is verified like saves.** A journal carrying a wrong CRC for a good file could never
  complete, so the CRC comes from a checked read of `saves.dat` or a comparison with the header
  in memory. A bad read fails the pass, which is harmless, rather than committing it.
- **Each operation carries its own CRC.** The journal's CRC is seen once at the start, and a
  wrong path read later would touch the wrong file.

If `Recovery required` does appear, a stage file named by the journal is gone or damaged. Do not
delete `.flippermeta`. `journal.dat` lists what was being written: the `.gci` files present are
each complete or in the middle of being replaced. Copy out the folder, then deleting
`journal.dat` lets the card mount from what is there.

## Concurrency

Core 1 runs the console protocol and never waits for the SD card. It only ever touches PSRAM and
the dirty structures.

| Shared | Between | Protection |
| --- | --- | --- |
| PSRAM | Core 1 reads and writes, core 0 scan and stage | `gc_dirty_lock`, one page at a time |
| Dirty heap and map | Both | `gc_dirty_lock` |
| Generation | Written by core 1, read by core 0 | A single word, read with the pending count under the lock |

A pass that reads PSRAM takes the lock for one page at a time, the same as `gc_dirty_task()`
does for a `.raw` card. It only starts when the console is quiet, so contention needs the console
to come back in the middle of a page. That is the same class of risk `.raw` cards already run,
but it should be looked at on hardware: see [testing-folder.md](testing-folder.md).

## Memory

No second card image exists anywhere: SRAM holds the table of committed saves, the planned table
of the same size, bitmaps of 1024 blocks and one 512-byte page buffer. The folder backend adds
20,212 bytes of flash and 17,732 bytes of RAM to a release build. PSRAM use is the same as for a
`.raw` card of that size, and nothing else stays on the SD card but the `.gci` files and the
metadata above, which stay under a few KB per card. A test checks that.

The firmware ends up at 73.1% of RAM in a release build.
