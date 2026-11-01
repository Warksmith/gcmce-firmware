# USB storage mode

FlipperMCE can show its whole microSD card to a computer as a removable drive, so no card
reader is needed to manage memory cards, firmware files or settings.

## Using it

1. Disconnect the FlipperMCE from everything.
2. Hold **Button 1** while connecting USB, then release it.
3. The computer sees a removable drive. Every partition and file on the card is there.
4. Eject the drive on the computer before unplugging. The display shows `Safe to disconnect`
   once the computer has released it.
5. Unplug. The next power-up boots normally.

Button 2 at power-up still starts the UF2 firmware update mode. If both buttons are held,
Button 2 wins, so firmware recovery cannot be blocked by this feature. A USB connection
with no button held does nothing new: a release build does not start USB at all.

| Display | Meaning |
| --- | --- |
| `Waiting for PC` | No computer has configured the device yet |
| `Connected` | In use by the computer. Eject before unplugging |
| `Safe to disconnect` | Released by the computer |
| `SD Error` | No usable card. Check it and reconnect |

## What it does and does not do

Storage mode runs instead of everything else. The firmware reads its settings from flash
and enters this mode before anything mounts the SD card, so the computer is the only owner
of the card. PSRAM is not started, the console-facing pins stay idle, and the device does
not act as a memory card until it is restarted.

There is no write-back cache in the firmware: a write is on its way to the card's own
storage before the computer gets its acknowledgement, and SYNCHRONIZE CACHE and eject wait
for the card to finish.

Unplugging without ejecting can damage the filesystem exactly as it can with any USB
drive. The computer may still be holding writes of its own.

## Technical notes

- USB mass storage, bulk-only, one logical unit with 512-byte sectors. Vendor `BitFunX`,
  product `FlipperMCE SD`, ID `CAFE:4002`, which is TinyUSB's ID for an MSC device.
- The USB stack belongs to `pico_stdio_usb`. Storage mode keeps it and replaces only the
  device and configuration descriptors, with `--wrap` link options. Release builds now link
  this stack but only start it in this mode. Debug builds keep their CDC console in normal
  boots and have no console in storage mode.
- The SD card runs in `SHARED_SPI` mode here, which ends every transfer before returning.
- Transfers are in `src/usb_storage_blocks.c`, which is free of USB and SD code so that the
  range checks and partial sector handling can run in the host tests.
- An unreadable card is reported to the computer as "medium not present". Errors during a
  transfer are reported as medium errors, not as success.

## Verifying it on hardware

The host tests cover the transfer logic only. See [testing.md](testing.md) for the USB
checks that need a real device.
