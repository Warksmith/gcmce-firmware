#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-startup-test
INCLUDES="-I$ROOT/test/support -I$ROOT/test/shim -I$ROOT/src/gc -I$ROOT/src -I$ROOT/ext/ESP8266SdFatWrapper/include -I$ROOT/ext/inih"
mkdir -p "$OUT"
cc -std=gnu11 -O1 -g -w -fsanitize=address -include stddef.h $INCLUDES -c "$ROOT/ext/inih/ini.c" -o "$OUT/ini.o"
cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-sign-compare \
    -Wno-pedantic -include stddef.h -include string.h -include stdlib.h \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -include ctype.h -include pico.h \
    $INCLUDES \
    "$ROOT/test/startup/test_startup.c" "$ROOT/test/support/sim_sd.c" "$OUT/ini.o" \
    "$ROOT/src/gc/gc_cardman.c" "$ROOT/src/util.c" "$ROOT/src/card_config.c" \
    -o "$OUT/test_startup"

"$OUT/test_startup"
