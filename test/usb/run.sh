#!/bin/sh
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${TMPDIR:-/tmp}/gcmce-usb-test
mkdir -p "$OUT"
cc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$ROOT/src" \
    "$ROOT/test/usb/test_usb_blocks.c" "$ROOT/src/usb_storage_blocks.c" -o "$OUT/test_usb_blocks"
"$OUT/test_usb_blocks"
