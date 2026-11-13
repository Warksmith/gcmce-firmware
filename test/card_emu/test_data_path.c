/* The console's write path, gc_mc_data_interface.c and gc_dirty.c as they are, on a host.
 * Regression tests for erases that were skipped or marked at the wrong place, and for a
 * flush that returned with writes still in the queue. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bigmem.h"
#include "card_emu/gc_mc_data_interface.h"
#include "gc_cardman.h"
#include "gc_dirty.h"
#include "psram/psram.h"

#define CARD_SIZE (8U * 1024U * 1024U)
#define SEGMENTS  (CARD_SIZE / 512U)

bigmem_t bigmem;
uint8_t cache[CACHE_SIZE];

static uint8_t psram[CARD_SIZE];
static uint8_t sd_image[CARD_SIZE];
static uint8_t available[SEGMENTS];
static uint32_t card_size = CARD_SIZE;
static unsigned sd_writes;
static uint64_t now_us;
static int lock_depth;
static sim_timer_t timer;

void sim_lock(void) { assert(lock_depth++ == 0); }
void sim_unlock(void) { assert(--lock_depth == 0); }

sim_timer_t *sim_timer_tick(void) {
    now_us += 500;
    timer.timerawh = (uint32_t)(now_us >> 32);
    timer.timerawl = (uint32_t)now_us;
    return &timer;
}

uint64_t time_us_64(void) {
    return now_us += 500;
}

void psram_read_dma(uint32_t addr, void *buf, size_t len, psram_cb_t cb) {
    assert(addr + len <= CARD_SIZE);
    memcpy(buf, &psram[addr], len);
    if (cb)
        cb();
}

void psram_write_dma(uint32_t addr, void *buf, size_t len, psram_cb_t cb) {
    assert(addr + len <= CARD_SIZE);
    memcpy(&psram[addr], buf, len);
    if (cb)
        cb();
}

void psram_wait_for_dma(void) {}
size_t psram_read_dma_remaining(void) { return 0; }

uint32_t gc_cardman_get_card_size(void) { return card_size; }

void gc_cardman_mark_segment_available(uint32_t segment) {
    assert(segment < SEGMENTS);
    available[segment] = 1;
}

int gc_cardman_write_segment(int segment, void *buf512) {
    memcpy(&sd_image[(uint32_t)segment * 512U], buf512, 512);
    sd_writes++;
    return 0;
}

void gc_cardman_flush(void) {}

static void reset(uint32_t size) {
    memset(psram, 0x11, sizeof(psram));
    memset(sd_image, 0x11, sizeof(sd_image));
    memset(available, 0, sizeof(available));
    card_size = size;
    sd_writes = 0;
    gc_mc_data_interface_init();
}

static void test_erase_high_address(void) {
    static uint8_t ff[8192];
    uint32_t addr = 0x500000;

    reset(CARD_SIZE);
    memset(ff, 0xFF, sizeof(ff));
    gc_mc_data_interface_erase(addr);

    assert(memcmp(&psram[addr], ff, sizeof(ff)) == 0);
    assert(psram[addr - 1] == 0x11 && psram[addr + sizeof(ff)] == 0x11);
    for (uint32_t i = 0; i < 16; i++)
        assert(available[addr / 512U + i]);

    gc_mc_data_interface_flush();
    assert(sd_writes == 16);
    assert(memcmp(&sd_image[addr], ff, sizeof(ff)) == 0);
}

static void test_erase_bounds(void) {
    reset(CARD_SIZE / 2U);
    gc_mc_data_interface_erase(CARD_SIZE / 2U - 0x2000U);
    assert(psram[CARD_SIZE / 2U - 1U] == 0xFF);
    gc_mc_data_interface_flush();

    reset(CARD_SIZE / 2U);
    gc_mc_data_interface_erase(CARD_SIZE / 2U);
    assert(psram[CARD_SIZE / 2U] == 0x11);
    gc_mc_data_interface_flush();
    assert(sd_writes == 0);
}

static void test_flush_after_idle_task(void) {
    uint8_t page[128];

    reset(CARD_SIZE);

    /* the background task has seen nothing to do, then the console writes */
    now_us += 1000000;
    gc_mc_data_interface_task();
    assert(gc_dirty_activity == 0);

    memset(page, 0x77, sizeof(page));
    gc_mc_data_interface_write_mc(0x300080, page, sizeof(page));

    gc_mc_data_interface_flush();
    assert(sd_writes == 1);
    assert(memcmp(&sd_image[0x300080], page, sizeof(page)) == 0);
    assert(gc_dirty_get_marked() == -1);
}

static void test_write_bounds(void) {
    uint8_t page[128] = {0};

    reset(CARD_SIZE / 2U);
    gc_mc_data_interface_write_mc(CARD_SIZE / 2U - 64U, page, sizeof(page));
    gc_mc_data_interface_flush();
    assert(sd_writes == 0);
}

int main(void) {
    test_erase_high_address();
    test_erase_bounds();
    test_flush_after_idle_task();
    test_write_bounds();
    puts("data path: all passed");
    return 0;
}
