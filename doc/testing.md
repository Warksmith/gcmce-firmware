# Testing

## Host tests

`test/run.sh` builds and runs everything below with AddressSanitizer and UBSan. It needs
only a C compiler. The firmware's build image has one:

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src" sd2psx-build sh /src/test/run.sh
```

| Test | Covers |
| --- | --- |
| `test/usb/test_usb_blocks.c` | USB transfers: bounds, partial sectors, card errors |
| `test/card_emu/test_data_path.c` | The real `gc_mc_data_interface.c` and `gc_dirty.c`: erases, flushes, bounds, folder card marks |
| `test/folder/test_folder.c` | The folder card engine and `card_config.c`, on a simulated SD card and console |
| `test/folder/test_settings.c` | `settings.c`: the global `StorageBackend` in `settings.ini` and flash |

### Folder card tests

`test/folder/run.sh [filter]` runs these on their own. `PF_VERBOSE=1` prints the engine's log
and `PF_LONG=1` runs longer randomized sessions.

They run the real engine with two simulators around it:

- `sim_sd.c`, an SD card in memory. Like FAT, it ignores case, and it only makes file growth
  durable on flush or close. It can cut the power, fail or corrupt any operation, and
  run out of space.
- `sim_console.c`, the CARD library's behaviour: erases and 128-byte page writes, in libogc's
  order, with both copies of the directory and BAT. It shares no code with `gc_fs.c`, and its
  mount applies the rules Dolphin and the CARD library do, so it checks the engine rather than
  agreeing with it.

The oracle in nearly every test is the same: the folder must hold exactly what the console
sees, entry and data.

| Area | Covers |
| --- | --- |
| Import | All sizes, refused files, saves kept in their slots, card exactly full, a second mount writing nothing |
| Identity | Header kept byte for byte, `card.dat` damaged, `saves.dat` damaged |
| Live changes | Create, write, delete, rename, data changed with no directory change, fragmented chains, awkward names |
| Coherence | Quiet time, marks not yet drained, a pass never started on a half-finished state |
| Power cuts | At every operation of a save and of a format, and again at every operation of the recovery |
| Faults | Every operation failing once, and for good, and the SD card filling up |
| Races | A console write at every operation, and racing writes followed by a power cut |
| Corruption | A wrong bit in every stage write and every read, in a pass and in recovery |
| Random | Sessions of random operations with power cuts at random points |
| Selection | `StorageBackend` parsing, including what must stay `RAW` |

What the simulators cannot show: real FAT behaviour, SD card timing, and whether a real console
and real games accept the card. Dolphin itself was not run against these files, so the
compatibility check is the console model and the format from Dolphin's source.

## Firmware builds

```bash
docker build -t sd2psx-build misc/docker
MSYS_NO_PATHCONV=1 docker run --rm -v "$PWD:/src" -v gcmce-build:/build sd2psx-build bash /src/misc/docker/build.sh
```

This builds the release and debug firmware and copies them to `build/docker`.

`FLIPPERMCE_WITH_GUI=OFF` does not link, because `gc.c` and `gc_mmceman.c` call GUI
functions without checking it. It did not link before this work either.

## Hardware checks: USB storage

Compiling and the host tests say nothing about these.

1. Hold Button 1 and connect USB: a removable drive appears, on Windows, Linux and macOS.
2. Check partitions and files against a card reader.
3. Copy a file of several hundred MB both ways and compare checksums.
4. Eject, from the tray on Windows, and with `Eject` in Explorer: within a few seconds the
   display shows `Safe to disconnect`. If it does not, flash the debug firmware and note the
   letters on the last line, which are the signals that arrived. `L` stop and unload, `S`
   stop, `A` allow removal, `Y` sync, `Z` suspend, `U` unconfigured.
5. Power up with no button held, with USB connected: the device boots normally and no
   drive appears on a release build.
6. Hold Button 2 while connecting USB: the RPI-RP2 drive appears.
7. Hold both buttons: the RPI-RP2 drive appears.
8. Hold Button 1 with no SD card: the display shows `SD Error`, and the computer shows an
   empty drive or none.
9. Confirm that Button 1 on the case is the one this firmware calls Button 1
   (`PIN_BTN_LEFT`, GPIO 21).
10. After using storage mode, boot the console and load a save: normal operation is
    unchanged.
11. Time an uncached read and write of a few MB, and the wait for the drive to appear. Before
    read-ahead, on a 32 GB FAT32 card with 16 KB clusters, reads were about 620 KiB/s, writes
    about 630 KiB/s, and the drive took about 13 seconds to appear.

## Hardware checks: folder cards

Start with a card you can lose and a copy of the SD card.

Done so far, on a BitFunX GCMCE by hand: save, reload and the display's unsaved and saving
messages, with PSO on 8, 16, 32 and 64 Mbit cards, and moving PSO's three files from a 32 to a
64 Mbit card without `card.dat`. The games ran through Nintendont. The rest is still to do.

Setup: set `StorageBackend=FOLDER` in one card's ini, leaving another as a `.raw` card.

1. Boot with an empty folder. The IPL's memory card screen shows an empty card of the size in
   the ini, and asks for nothing.
2. Repeat for 4, 8, 16, 32 and 64 Mbit.
3. Copy in `.gci` files exported by Dolphin, boot, and confirm the saves are listed and load in
   their games.
4. In a game, save over an existing save. Wait for the display to clear, then check on a computer
   that only that `.gci` changed.
5. Create a new save and delete another. Check the files on a computer.
6. Open the changed and new `.gci` files in Dolphin and load them in the game.
7. Format the card in the IPL. Confirm that `.flippermeta/trash` has the old saves.
8. Cut the power while `Saving card...` is showing, several times. Each boot must show a working
   card with each save complete, in its old or new version.
9. Write protect or remove the SD card while saving: the display shows `Save failed!`, and
   switching cards is refused.
10. Put a corrupt `.gci` in the folder: the card stays hidden, the display says why and
    `import-errors.txt` lists it.
11. Switch between a folder card and a `.raw` card, and between channels, with Game ID and with
    the buttons. Neither card is changed by the other.
12. Play a game that saves often, and a game with a large save on a 64 Mbit card, and watch for
    a console that reports a card error during a save. This is what would show PSRAM contention
    between the two cores.
13. Check the mount time of a full 64 Mbit card. The IPL and games must still detect the card
    at the end of it.
14. With MMCE games that switch cards, check that a switch while `Saving card...` is showing
    finishes the save first.
15. Test a save from F-Zero GX or Phantasy Star Online, made on this card, across a remount.
