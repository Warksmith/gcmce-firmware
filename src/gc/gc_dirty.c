#include "gc_dirty.h"
#include "psram.h"
#include "gc_cardman.h"
#include "gc_folder_card.h"
#include "debug.h"

#include "bigmem.h"
#define dirty_heap bigmem.gc.dirty_heap
#define dirty_map bigmem.gc.dirty_map

#include <hardware/sync.h>
#include <pico/platform.h>
#include <stdio.h>

spin_lock_t *gc_dirty_spin_lock;
volatile uint32_t gc_dirty_lockout;
volatile uint32_t gc_dirty_generation;
int gc_dirty_activity = 0;

static int num_dirty;

#define SWAP(a, b) do { \
    uint16_t tmp = a; \
    a = b; \
    b = tmp; \
} while (0);

static inline bool dirty_map_is_marked(uint32_t sector) {
    return dirty_map[sector / 8] & (1 << (sector % 8));
}

static inline void dirty_map_mark_sector(uint32_t sector) {
    dirty_map[sector / 8] |= (1 << (sector % 8));
}

static inline void dirty_map_unmark_sector(uint32_t sector) {
    dirty_map[sector / 8] &= ~(1 << (sector % 8));
}

void gc_dirty_init(void) {
    gc_dirty_spin_lock = spin_lock_init(spin_lock_claim_unused(1));
}

void __time_critical_func(gc_dirty_mark)(uint32_t sector) {
    gc_dirty_generation++;

    if (sector < (sizeof(dirty_map) * 8) ) {
        /* already marked? */
        if (dirty_map_is_marked(sector))
            return;

        /* update map */
        dirty_map_mark_sector(sector);

        /* update heap */
        int cur = num_dirty++;
        dirty_heap[cur] = sector;
        while (dirty_heap[cur] < dirty_heap[(cur-1)/2]) {
            SWAP(dirty_heap[cur], dirty_heap[(cur-1)/2]);
            cur = (cur-1)/2;
        }
    }
}

static void heapify(int i) {
    int l = i * 2 + 1;
    int r = i * 2 + 2;
    int best = i;
    if (l < num_dirty && dirty_heap[l] < dirty_heap[best])
        best = l;
    if (r < num_dirty && dirty_heap[r] < dirty_heap[best])
        best = r;
    if (best != i) {
        SWAP(dirty_heap[i], dirty_heap[best]);
        heapify(best);
    }
}

int gc_dirty_pending(void) {
    return num_dirty;
}

int gc_dirty_get_marked(void) {
    if (num_dirty == 0)
        return -1;

    uint16_t ret = dirty_heap[0];

    /* update heap */
    dirty_heap[0] = dirty_heap[--num_dirty];
    heapify(0);

    /* update map */
    dirty_map_unmark_sector(ret);

    return ret;
}

/* a folder card reads PSRAM itself, as whole saves at consistent moments, so here its marks
   only move on to its own change tracking. nothing touches the SD card or PSRAM */
static void gc_dirty_drain_to_folder_card(void) {
    while (1) {
        gc_dirty_lock();
        int sector = gc_dirty_get_marked();
        gc_dirty_unlock();
        if (sector == -1)
            break;
        gc_folder_card_note_segment((uint32_t)sector);
    }
    gc_dirty_activity = gc_dirty_lockout_expired() ? 0 : 1;
}

/* this goes through blocks in psram marked as dirty and flushes them to sd */
void gc_dirty_task(void) {
    static uint8_t flushbuf[512];

    if (gc_cardman_is_folder()) {
        gc_dirty_drain_to_folder_card();
        return;
    }

    int num_after = 0;
    int hit = 0;
    uint64_t start = time_us_64();
    int ret = 0;
    while (1) {
        if (!gc_dirty_lockout_expired())
            break;
        /* do up to 100ms of work per call to dirty_taks */
        if ((time_us_64() - start) > 100 * 1000)
            break;

        gc_dirty_lock();
        int sector = gc_dirty_get_marked();
        num_after = num_dirty;
        if (sector == -1) {
            gc_dirty_unlock();
            break;
        }
        psram_read_dma(sector * 512, flushbuf, 512, NULL);
        psram_wait_for_dma();
        gc_dirty_unlock();

        ++hit;
        ret = gc_cardman_write_segment(sector, flushbuf);

        if (ret != 0) {
            // TODO: do something if we get too many errors?
            // for now lets push it back into the heap and try again later
            DPRINTF("!! writing sector 0x%x failed: %i\n", sector, ret);
            DPRINTF("Adress: 0x%08x\n", sector * 512);

            gc_dirty_lock();
            gc_dirty_mark(sector);
            gc_dirty_unlock();
        }
    }

    uint64_t end = time_us_64();

    if (hit) {
        /* to make sure writes hit the storage medium */
        gc_cardman_flush();
        DPRINTF("remain to flush - %d - this one flushed %d and took %d ms\n", num_after, hit, (int)((end - start) / 1000));
    }

    if (num_after || !gc_dirty_lockout_expired())
        gc_dirty_activity = 1;
    else
        gc_dirty_activity = 0;
}
