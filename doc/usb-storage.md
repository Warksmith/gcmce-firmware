# USB storage mode

FlipperMCE can show its whole microSD card to a computer as a removable drive, so no card
reader is needed to manage memory cards, firmware files or settings.

## Using it

1. Disconnect the FlipperMCE from everything.
2. Hold **Button 1** while connecting USB, then release it.
3. The computer sees a removable drive. Every partition and file on the card is there.
4. Eject the drive on the computer before unplugging. The display shows `Safe to disconnect`
   a few seconds after the computer has released it.
5. Unplug. The next power-up boots normally.

Button 2 at power-up still starts the UF2 firmware update mode. If both buttons are held,
Button 2 wins, so firmware recovery cannot be blocked by this feature. A USB connection
with no button held does nothing new: a release build does not start USB at all.

| Display | Meaning |
| --- | --- |
| `Waiting for PC` | No computer has configured the device yet |
| `Connected` | In use by the computer. Eject before unplugging |
| `Safe to disconnect` | Released by the computer, or the computer has gone quiet |
| `SD Error` | No usable card. Check it and reconnect |

## What it does and does not do

Storage mode runs instead of everything else. The firmware reads its settings from flash
and enters this mode before anything mounts the SD card, so the computer is the only owner
of the card. PSRAM is not started, the console-facing pins stay idle, and the device does
not act as a memory card until it is restarted.

There is no write-back cache in the firmware: a write is on its way to the card's own
storage before the computer gets its acknowledgement, and SYNCHRONIZE CACHE and eject wait
for the card to finish. Reads are the only thing held ahead of time, and any write drops
them.

Unplugging without ejecting can damage the filesystem exactly as it can with any USB
drive. The computer may still be holding writes of its own.

Computers do not all announce an eject, so the device takes any of these as the release: a
SCSI stop, with or without unload, the host unconfiguring it, or the USB bus suspended for
2 seconds. A computer going to sleep suspends the bus too, so `Safe to disconnect` can also
appear then, and `Connected` returns when it wakes. Eject first even so.

## Speed, and the wait when connecting

The RP2040 only has USB full speed, so this will never be quick: expect well under
1 MB/s. For large transfers, use a card reader.

When a FAT32 drive is connected, Windows reads the whole FAT to count the free space, and
**File Explorer can hang until that finishes**. This depends on the size of the FAT, not on
how full the card is, so a nearly empty card waits just as long:

| Card | Cluster size | FAT size |
| --- | --- | --- |
| 32 GB | 16 KB | 7.8 MB |
| 32 GB | 32 KB | 3.9 MB |
| 32 GB | 64 KB | 1.9 MB |
| 8 GB | 16 KB | 1.9 MB |

A 32 GB card with 16 KB clusters, the usual format, took 13 seconds to appear when measured
at about 620 KiB/s. To shorten the wait, reformat with larger clusters, after copying off
anything you want to keep. This erases the card:

```
format F: /FS:FAT32 /A:64K /Q
```

The cost is that every file takes at least one cluster, so a small save occupies 64 KB. Use
the right drive letter: this is a command that will erase whatever it is pointed at.

exFAT avoids the scan altogether, since Windows only reads a small allocation bitmap:

```
format F: /FS:exFAT /A:128K /Q
```

The firmware reads and writes exFAT through the same library calls as FAT32. Its limits:

- Keep clusters at 128 KB or below. Any size mounts, but a new folder has its whole first
  cluster written with zeros, and every file and folder occupies at least one cluster, so
  sizes of 1 MB and up make folder creation slow and waste a lot of the card. The firmware
  creates folders when it starts a new card.
- The folder card power-cut tests run against a simulated file API, not a real filesystem,
  so they cover FAT32 and exFAT equally little. The two differ in what a power cut can do to
  the filesystem itself. exFAT has one FAT where FAT32 has two, and the firmware never marks
  an exFAT volume as in use, so Windows will not offer to check it after a power cut. Run
  `chkdsk F:` yourself if the device lost power during a save.
- Software on the console that reads the SD card directly through the MMCE block commands
  sees the raw sectors, and has to understand exFAT itself.

## Technical notes

- USB mass storage, bulk-only, one logical unit with 512-byte sectors. Vendor `BitFunX`,
  product `FlipperMCE SD`, ID `CAFE:4002`, which is TinyUSB's ID for an MSC device.
- The USB stack belongs to `pico_stdio_usb`. Storage mode keeps it and replaces only the
  device and configuration descriptors, with `--wrap` link options. Release builds now link
  this stack but only start it in this mode. Debug builds keep their CDC console in normal
  boots and have no console in storage mode.
- The SD card runs in `SHARED_SPI` mode here, which ends every transfer before returning.
- Transfers are in `src/usb_storage_blocks.c`, which is free of USB and SD code so that the
  range checks, partial sector handling and read-ahead can run in the host tests.
- Reads alternate between the card and the bus, because TinyUSB asks for the next 16 KB only
  once the last has gone out. USB sends from an interrupt, so when the host is reading in
  order the main loop fills a second 16 KB buffer while that happens. Writes have no
  equivalent: the last block of a command has to reach the card before the status goes out,
  and the `tud_msc_write10_cb` callback cannot tell which block is the last.
- An unreadable card is reported to the computer as "medium not present". Errors during a
  transfer are reported as medium errors, not as success.

## Verifying it on hardware

The host tests cover the transfer logic only. See [testing-usb.md](testing-usb.md) for the USB
checks that need a real device.
