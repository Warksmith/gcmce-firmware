#pragma once
/* Just enough of the Pico SDK to compile the card emulation's data path on a host. */
#include <stdbool.h>
#include <stdint.h>
typedef unsigned int uint;
#define __time_critical_func(f) f
#define __no_inline_not_in_flash_func(f) f
static inline uint get_core_num(void) { return 1; }
