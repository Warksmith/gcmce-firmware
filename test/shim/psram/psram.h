#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void (*psram_cb_t)(void);
void psram_read_dma(uint32_t addr, void *buf, size_t len, psram_cb_t cb);
void psram_write_dma(uint32_t addr, void *buf, size_t len, psram_cb_t cb);
void psram_wait_for_dma(void);
size_t psram_read_dma_remaining(void);
