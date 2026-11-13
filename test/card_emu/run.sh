#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-card-test
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -fno-sanitize-recover=undefined"
mkdir -p "$OUT"
cc $FLAGS -include stddef.h -Wno-unused-function -Wno-unused-variable -I"$ROOT/test/shim" -I"$ROOT/src" -I"$ROOT/src/gc" \
    -I"$ROOT/ext/ESP8266SdFatWrapper/include" \
    "$ROOT/test/card_emu/test_data_path.c" "$ROOT/src/gc/card_emu/gc_mc_data_interface.c" "$ROOT/src/gc/gc_dirty.c" \
    -o "$OUT/test_data_path"
"$OUT/test_data_path"
