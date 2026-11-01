#pragma once
/* settings.c stops core 1 around flash writes. There is no core 1 here. */
#include <stdbool.h>
static inline bool multicore_lockout_victim_is_initialized(unsigned core) { return false; }
static inline void multicore_lockout_start_blocking(void) {}
static inline void multicore_lockout_end_blocking(void) {}
