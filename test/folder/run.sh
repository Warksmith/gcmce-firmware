#!/bin/sh
# Builds and runs the folder-card host tests. Needs only a C compiler.
#   test/folder/run.sh [test name filter]
# PF_VERBOSE=1 prints the engine log, PF_LONG=1 runs longer randomized sessions.
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-folder-test
INCLUDES="-I$ROOT/test/folder -I$ROOT/test/shim -I$ROOT/src/gc -I$ROOT/src -I$ROOT/ext/ESP8266SdFatWrapper/include -I$ROOT/ext/inih"

mkdir -p "$OUT"

cc -std=gnu11 -O1 -g -w -fsanitize=address -include stddef.h $INCLUDES -c "$ROOT/ext/inih/ini.c" -o "$OUT/ini.o"

cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-sign-compare \
    -Wno-pedantic -include stddef.h -include string.h \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -DGC_FOLDER_HOST_TEST $INCLUDES \
    "$ROOT/test/folder/test_folder.c" "$ROOT/test/folder/sim_sd.c" "$ROOT/test/folder/sim_console.c" \
    "$ROOT/src/gc/gc_fs.c" "$ROOT/src/gc/gc_gci.c" "$ROOT/src/gc/gc_folder_card.c" \
    "$ROOT/src/gc/gc_folder_import.c" "$ROOT/src/gc/gc_folder_meta.c" "$ROOT/src/gc/gc_folder_sync.c" \
    "$ROOT/src/card_config.c" "$OUT/ini.o" \
    -o "$OUT/test_folder"

cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-sign-compare \
    -Wno-pedantic -include stddef.h -include string.h -include stdlib.h \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    $INCLUDES \
    "$ROOT/test/folder/test_settings.c" "$ROOT/test/folder/sim_sd.c" "$OUT/ini.o" \
    -o "$OUT/test_settings"

"$OUT/test_folder" "$@"
"$OUT/test_settings"
