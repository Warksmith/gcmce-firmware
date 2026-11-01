#include "gc_folder_port.h"

#include "gc_dirty.h"
#include "hardware/timer.h"
#include "psram/psram.h"

void pf_psram_read(uint32_t addr, void *buf, size_t len) {
    gc_dirty_lock();
    psram_read_dma(addr, buf, len, NULL);
    psram_wait_for_dma();
    gc_dirty_unlock();
}

void pf_psram_write(uint32_t addr, const void *buf, size_t len) {
    gc_dirty_lock();
    psram_write_dma(addr, (void *)buf, len, NULL);
    psram_wait_for_dma();
    gc_dirty_unlock();
}

uint64_t pf_time_us(void) {
    return time_us_64();
}

void pf_write_snapshot(uint32_t *generation, bool *pending) {
    gc_dirty_lock();
    *generation = gc_dirty_generation;
    *pending = gc_dirty_pending() != 0;
    gc_dirty_unlock();
}

bool pf_console_quiet(void) {
    return gc_dirty_lockout_expired();
}
