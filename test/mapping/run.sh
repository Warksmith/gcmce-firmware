#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-mapping-test
mkdir -p "$OUT"
cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -include stddef.h -include string.h \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$ROOT/test/support" -I"$ROOT/test/shim" -I"$ROOT/src" \
    -I"$ROOT/ext/ESP8266SdFatWrapper/include" -I"$ROOT/ext/inih" \
    "$ROOT/test/mapping/test_mapping.c" "$ROOT/test/support/sim_sd.c" \
    "$ROOT/src/card_config.c" "$ROOT/ext/inih/ini.c" -o "$OUT/test_mapping"
"$OUT/test_mapping"
