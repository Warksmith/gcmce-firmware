#!/bin/sh
# Builds and runs the host tests. Needs only a C compiler.
#   test/run.sh
# Runs everything under AddressSanitizer and UBSan.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-test
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined"

mkdir -p "$OUT"

cc $FLAGS -I"$ROOT/src" \
    "$ROOT/test/usb/test_usb_blocks.c" "$ROOT/src/usb_storage_blocks.c" -o "$OUT/test_usb_blocks"
"$OUT/test_usb_blocks"

cc $FLAGS -include stddef.h -Wno-unused-function -Wno-unused-variable -I"$ROOT/test/shim" -I"$ROOT/src" -I"$ROOT/src/gc" \
    -I"$ROOT/ext/ESP8266SdFatWrapper/include" \
    "$ROOT/test/card_emu/test_data_path.c" "$ROOT/src/gc/card_emu/gc_mc_data_interface.c" "$ROOT/src/gc/gc_dirty.c" \
    -o "$OUT/test_data_path"
"$OUT/test_data_path"

if [ -f "$ROOT/test/folder/run.sh" ]; then
    sh "$ROOT/test/folder/run.sh"
fi
