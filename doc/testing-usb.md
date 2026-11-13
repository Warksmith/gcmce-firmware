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
4. Eject: the display shows `Safe to disconnect`.
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

USB read-ahead tests cover sequential reads, invalidation on writes, and transfer boundaries.
