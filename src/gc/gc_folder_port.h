#pragma once

/* Everything the folder-card engine needs from the platform. Firmware builds use
 * gc_folder_port_pico.c; the host test harness provides a simulated PSRAM and console. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded PSRAM transfers (at most one page) that are safe against console DMA. */
void pf_psram_read(uint32_t addr, void *buf, size_t len);
void pf_psram_write(uint32_t addr, const void *buf, size_t len);

uint64_t pf_time_us(void);

/* Consistent view of the console write counter and of writes not yet drained into the
 * folder-card change tracking. */
void pf_write_snapshot(uint32_t *generation, bool *pending);

/* False while the console is actively using the card. */
bool pf_console_quiet(void);

#ifdef GC_FOLDER_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#define PF_LOG(fmt, ...) do { if (getenv("PF_VERBOSE")) printf("[GCI] " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#include "debug.h"
#define PF_LOG(fmt, x...) DPRINTF("[GCI] " fmt "\n", ##x)
#endif
