#!/bin/sh
# Run each host-test suite present on this feature branch, with sanitizers.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for suite in card_emu usb folder mapping startup; do
    if [ -f "$ROOT/test/$suite/run.sh" ]; then
        sh "$ROOT/test/$suite/run.sh"
    fi
done
