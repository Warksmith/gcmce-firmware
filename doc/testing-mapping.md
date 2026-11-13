# Game2Folder validation

Run `sh test/mapping/run.sh` on Linux with a C compiler. The existing mapping regression cases run against the real card configuration parser with an in-memory SD card, under AddressSanitizer and UBSan. They cover exact/prefix precedence, repeated stars, duplicate rules, section isolation, opt-in fallback, empty mappings, stale output and buffer bounds.

On hardware, enable GameID and put full loader IDs under `[GC]` in `.flippermce/Game2Folder.ini`. Check exact rules, overlapping prefixes and unmatched IDs with fallback off and on; switch from mapped to unmapped games and verify save/reload. Hardware validation of these behaviors remains pending.
