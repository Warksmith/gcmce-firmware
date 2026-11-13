# Startup-card validation

Run `sh test/startup/run.sh` on Linux with a C compiler. The existing regression cases exercise the real settings parser, card-manager startup selection and directory iterator under AddressSanitizer and UBSan: restore precedence, numbered/named/new cards, channel 1, invalid names, missing configuration and settings rewrite/reload.

On hardware, set `[GC] CardRestore=OFF` and `StartupCard=Shared`, then power-cycle and confirm channel 1 opens. Repeat with Card12 and a new card. Enable CardRestore, select another card/channel and confirm restore wins. Remove StartupCard with restore off to check Card1. Confirm subsequent GameID switching and menu settings changes. Hardware validation remains pending.
