#!/bin/bash
# Builds the FlipperMCE firmware, with and without USB serial logging, into build/docker/.
# Keep /build in a volume so that later runs only recompile what changed.
# See doc/testing-usb.md.
set -eo pipefail

git config --global --add safe.directory '*'

for debug in OFF ON; do
    dir=/build/FlipperMCE-$debug
    echo "=== FlipperMCE debug=$debug"
    cmake -S /src -B "$dir" -DCMAKE_BUILD_TYPE=Release -DVARIANT=FlipperMCE -DDEBUG_USB_UART="$debug" -G 'Unix Makefiles' >/dev/null
    cmake --build "$dir" -j8 2>&1 | grep -E ' error |FLASH:|RAM:|gc_folder.*warning|usb_storage.*warning|\*\*\*' || true
done

mkdir -p /src/build/docker
cp /build/FlipperMCE-OFF/flippermce.uf2 /build/FlipperMCE-OFF/flippermce.elf \
   /build/FlipperMCE-ON/flippermce-debug.uf2 /build/FlipperMCE-ON/flippermce-debug.elf /src/build/docker/
sha256sum /src/build/docker/flippermce.uf2 /src/build/docker/flippermce-debug.uf2
