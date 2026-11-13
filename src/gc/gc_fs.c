#include "gc_fs.h"

#include <string.h>

#include "gc_folder_port.h"
#include "gc_gci.h"

/* Both sums wrap at 16 bits. The CARD library treats a result of 0xFFFF as 0. */
void gcfs_checksum_update(gcfs_checksum_t *c, const uint8_t *data, size_t len) {
    uint16_t sum = c->sum;
    uint16_t inv = c->inv;

    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t word = gcfs_get16(&data[i]);

        sum = (uint16_t)(sum + word);
        inv = (uint16_t)(inv + (word ^ 0xFFFFU));
    }
    c->sum = sum;
    c->inv = inv;
}

static uint16_t checksum_final(uint16_t value) {
    return value == 0xFFFF ? 0 : value;
}

void gcfs_checksum_store(const gcfs_checksum_t *c, uint8_t *dst) {
    gcfs_put16(dst, checksum_final(c->sum));
    gcfs_put16(dst + 2, checksum_final(c->inv));
}

bool gcfs_checksum_matches(const gcfs_checksum_t *c, const uint8_t *stored) {
    return gcfs_get16(stored) == checksum_final(c->sum) && gcfs_get16(stored + 2) == checksum_final(c->inv);
}

bool gcfs_size_valid(uint32_t mbits) {
    return mbits == 4 || mbits == 8 || mbits == 16 || mbits == 32 || mbits == 64;
}

uint32_t gcfs_blocks_for_size(uint32_t mbits) {
    return mbits * 16U;
}

static void header_fix_checksum(uint8_t *header) {
    gcfs_checksum_t c = {0, 0};

    gcfs_checksum_update(&c, header, GCFS_HDR_CHECKSUM);
    gcfs_checksum_store(&c, &header[GCFS_HDR_CHECKSUM]);
}

void gcfs_header_init(uint8_t header[GCFS_HEADER_SIZE], uint32_t mbits, uint8_t encoding) {
    memset(header, 0xFF, GCFS_HEADER_SIZE);
    memset(header, 0x00, 0x26);
    header[0x1B] = (uint8_t)(encoding + 1U); /* language */
    gcfs_put16(&header[GCFS_HDR_SIZE_MBITS], (uint16_t)mbits);
    header[GCFS_HDR_ENCODING + 1] = encoding;
    header[0x1FA] = 0;
    header[0x1FB] = 0;
    header_fix_checksum(header);
}

bool gcfs_header_valid(const uint8_t header[GCFS_HEADER_SIZE]) {
    gcfs_checksum_t c = {0, 0};

    gcfs_checksum_update(&c, header, GCFS_HDR_CHECKSUM);
    return gcfs_checksum_matches(&c, &header[GCFS_HDR_CHECKSUM]) && gcfs_size_valid(gcfs_header_mbits(header));
}

uint32_t gcfs_header_mbits(const uint8_t header[GCFS_HEADER_SIZE]) {
    return gcfs_get16(&header[GCFS_HDR_SIZE_MBITS]);
}

void gcfs_header_set_mbits(uint8_t header[GCFS_HEADER_SIZE], uint32_t mbits) {
    gcfs_put16(&header[GCFS_HDR_SIZE_MBITS], (uint16_t)mbits);
    header_fix_checksum(header);
}

static uint16_t read_counter(uint32_t addr) {
    uint8_t raw[2];

    pf_psram_read(addr, raw, sizeof(raw));
    return gcfs_get16(raw);
}

/* The copy with the higher counter is the newer one, with the signed comparison the
 * console uses so that the counter can wrap. */
static uint32_t newest_copy(uint32_t first_block, uint32_t counter_offset) {
    uint16_t a = read_counter(gcfs_block_addr(first_block) + counter_offset);
    uint16_t b = read_counter(gcfs_block_addr(first_block + 1U) + counter_offset);

    return (int16_t)(uint16_t)(a - b) >= 0 ? first_block : first_block + 1U;
}

static bool dir_copy_valid(uint32_t block, uint8_t *page_buf) {
    gcfs_checksum_t c = {0, 0};
    uint32_t addr = gcfs_block_addr(block);

    for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++, addr += GCFS_PAGE_SIZE) {
        bool last = page == GCFS_PAGES_PER_BLOCK - 1U;

        pf_psram_read(addr, page_buf, GCFS_PAGE_SIZE);
        gcfs_checksum_update(&c, page_buf, last ? GCFS_PAGE_SIZE - 4U : GCFS_PAGE_SIZE);
        if (last)
            return gcfs_checksum_matches(&c, &page_buf[GCFS_PAGE_SIZE - 4U]);
    }
    return false;
}

/* Reads a BAT copy into the view and checks its checksum and free count, as the CARD
 * library does. A BAT whose free count is wrong is as bad as one with a wrong checksum. */
static bool bat_copy_load(gcfs_view_t *view, uint32_t block, uint8_t *page_buf) {
    gcfs_checksum_t c = {0, 0};
    uint32_t addr = gcfs_block_addr(block);
    uint32_t data_blocks = (uint32_t)view->blocks - GCFS_SYS_BLOCKS;
    uint32_t free_blocks = 0;
    uint32_t map_index = 0;
    uint8_t stored[4];
    uint16_t stored_free = 0;

    for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++, addr += GCFS_PAGE_SIZE) {
        uint32_t first = page == 0 ? GCFS_BAT_MAP : 0U;

        pf_psram_read(addr, page_buf, GCFS_PAGE_SIZE);
        if (page == 0) {
            memcpy(stored, page_buf, sizeof(stored));
            stored_free = gcfs_get16(&page_buf[GCFS_BAT_FREE]);
            gcfs_checksum_update(&c, &page_buf[GCFS_BAT_UPDATE], GCFS_PAGE_SIZE - GCFS_BAT_UPDATE);
        } else {
            gcfs_checksum_update(&c, page_buf, GCFS_PAGE_SIZE);
        }
        for (uint32_t off = first; off < GCFS_PAGE_SIZE && map_index < data_blocks; off += 2U, map_index++) {
            uint16_t next = gcfs_get16(&page_buf[off]);

            view->bat[map_index] = next;
            if (next == 0)
                free_blocks++;
        }
    }

    view->free_blocks = stored_free;
    return gcfs_checksum_matches(&c, stored) && stored_free == free_blocks;
}

gcfs_result_t gcfs_view_load(gcfs_view_t *view, uint32_t blocks, uint8_t *page_buf) {
    uint32_t dir = newest_copy(GCFS_BLOCK_DIR, GCFS_DIR_UPDATE);
    uint32_t bat = newest_copy(GCFS_BLOCK_BAT, GCFS_BAT_UPDATE);
    uint32_t other_dir = dir == GCFS_BLOCK_DIR ? GCFS_BLOCK_DIR + 1U : GCFS_BLOCK_DIR;
    uint32_t other_bat = bat == GCFS_BLOCK_BAT ? GCFS_BLOCK_BAT + 1U : GCFS_BLOCK_BAT;

    view->blocks = (uint16_t)blocks;

    if (dir_copy_valid(dir, page_buf))
        view->dir_addr = gcfs_block_addr(dir);
    else if (dir_copy_valid(other_dir, page_buf))
        view->dir_addr = gcfs_block_addr(other_dir);
    else
        return GCFS_ERR_NO_DIR;

    if (!bat_copy_load(view, bat, page_buf) && !bat_copy_load(view, other_bat, page_buf))
        return GCFS_ERR_NO_BAT;

    return GCFS_OK;
}

void gcfs_view_entry(const gcfs_view_t *view, uint32_t slot, uint8_t *entry) {
    pf_psram_read(view->dir_addr + slot * GCFS_ENTRY_SIZE, entry, GCFS_ENTRY_SIZE);
}

gcfs_result_t gcfs_chain_check(const gcfs_view_t *view, const uint8_t *entry, uint8_t *owned) {
    uint32_t count = gcfs_get16(&entry[GCFS_ENT_BLOCK_COUNT]);
    uint32_t block = gcfs_get16(&entry[GCFS_ENT_FIRST_BLOCK]);

    if (count == 0 || count > (uint32_t)view->blocks - GCFS_SYS_BLOCKS)
        return GCFS_ERR_ENTRY;

    for (uint32_t i = 0; i < count; i++) {
        if (block < GCFS_SYS_BLOCKS || block >= view->blocks)
            return GCFS_ERR_CHAIN;
        if (owned[block / 8U] & (1U << (block % 8U)))
            return GCFS_ERR_OVERLAP;
        owned[block / 8U] |= (uint8_t)(1U << (block % 8U));

        uint32_t next = view->bat[block - GCFS_SYS_BLOCKS];

        if (i + 1U == count)
            return next == GCFS_BAT_END ? GCFS_OK : GCFS_ERR_CHAIN;
        if (next == 0 || next == GCFS_BAT_END)
            return GCFS_ERR_CHAIN;
        block = next;
    }
    return GCFS_ERR_CHAIN;
}

gcfs_result_t gcfs_view_validate(const gcfs_view_t *view, uint8_t *owned) {
    static gci_id_t ids[GCFS_DIR_ENTRIES];
    uint8_t entry[GCFS_ENTRY_SIZE];
    uint32_t used = 0;

    memset(owned, 0, GCFS_MAX_BLOCKS / 8U);

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        gcfs_view_entry(view, slot, entry);
        if (!gcfs_entry_used(entry))
            continue;

        gcfs_result_t result = gcfs_chain_check(view, entry, owned);

        if (result != GCFS_OK)
            return result;

        gci_identity(entry, &ids[used]);
        for (uint32_t i = 0; i < used; i++) {
            if (gci_id_equal(&ids[i], &ids[used]))
                return GCFS_ERR_DUPLICATE;
        }
        used++;
    }
    return GCFS_OK;
}

static void write_dir_copy(uint32_t block, uint16_t update, const gcfs_checksum_t *entries_sum, uint8_t *page_buf) {
    uint32_t src = gcfs_block_addr(GCFS_BLOCK_DIR) + GCFS_BLOCK_SIZE - GCFS_PAGE_SIZE;
    gcfs_checksum_t c = *entries_sum;
    uint8_t tail[2];

    pf_psram_read(src, page_buf, GCFS_PAGE_SIZE);
    gcfs_put16(tail, update);
    memcpy(&page_buf[GCFS_DIR_UPDATE - (GCFS_BLOCK_SIZE - GCFS_PAGE_SIZE)], tail, sizeof(tail));
    gcfs_checksum_update(&c, tail, sizeof(tail));
    gcfs_checksum_store(&c, &page_buf[GCFS_PAGE_SIZE - 4U]);
    pf_psram_write(gcfs_block_addr(block) + GCFS_BLOCK_SIZE - GCFS_PAGE_SIZE, page_buf, GCFS_PAGE_SIZE);
}

static void write_bat_copy(uint32_t block, uint16_t update, uint32_t blocks, const uint16_t *bat,
                           uint16_t last_alloc, uint32_t free_blocks, uint8_t *page_buf) {
    gcfs_checksum_t c = {0, 0};
    uint32_t data_blocks = blocks - GCFS_SYS_BLOCKS;
    uint32_t map_index = 0;
    uint32_t addr = gcfs_block_addr(block);
    static uint8_t first_page[GCFS_PAGE_SIZE];

    /* The checksum covers what comes after it, so the first page goes out last. */
    for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++, addr += GCFS_PAGE_SIZE) {
        uint32_t off = page == 0 ? GCFS_BAT_MAP : 0U;
        uint8_t *dst = page == 0 ? first_page : page_buf;

        memset(dst, 0, GCFS_PAGE_SIZE);
        if (page == 0) {
            gcfs_put16(&dst[GCFS_BAT_UPDATE], update);
            gcfs_put16(&dst[GCFS_BAT_FREE], (uint16_t)free_blocks);
            gcfs_put16(&dst[GCFS_BAT_LAST_ALLOC], last_alloc);
        }
        for (; off < GCFS_PAGE_SIZE && map_index < data_blocks; off += 2U, map_index++)
            gcfs_put16(&dst[off], bat[map_index]);

        if (page == 0) {
            gcfs_checksum_update(&c, &dst[GCFS_BAT_UPDATE], GCFS_PAGE_SIZE - GCFS_BAT_UPDATE);
        } else {
            gcfs_checksum_update(&c, dst, GCFS_PAGE_SIZE);
            pf_psram_write(addr, dst, GCFS_PAGE_SIZE);
        }
    }
    gcfs_checksum_store(&c, &first_page[GCFS_BAT_CHECKSUM]);
    pf_psram_write(gcfs_block_addr(block), first_page, GCFS_PAGE_SIZE);
}

void gcfs_commit_tables(uint32_t blocks, const uint16_t *bat, uint16_t last_alloc, uint8_t *page_buf) {
    gcfs_checksum_t c = {0, 0};
    uint32_t data_blocks = blocks - GCFS_SYS_BLOCKS;
    uint32_t free_blocks = 0;
    uint32_t src = gcfs_block_addr(GCFS_BLOCK_DIR);
    uint32_t dst = gcfs_block_addr(GCFS_BLOCK_DIR + 1U);

    /* Everything but the last page is the same in both directory copies. */
    for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++, src += GCFS_PAGE_SIZE, dst += GCFS_PAGE_SIZE) {
        bool last = page == GCFS_PAGES_PER_BLOCK - 1U;

        pf_psram_read(src, page_buf, GCFS_PAGE_SIZE);
        gcfs_checksum_update(&c, page_buf, last ? GCFS_DIR_UPDATE - (GCFS_BLOCK_SIZE - GCFS_PAGE_SIZE) : GCFS_PAGE_SIZE);
        if (!last)
            pf_psram_write(dst, page_buf, GCFS_PAGE_SIZE);
    }

    /* Copy 1 is the newer one, the one the console takes as current. */
    write_dir_copy(GCFS_BLOCK_DIR, 0, &c, page_buf);
    write_dir_copy(GCFS_BLOCK_DIR + 1U, 1, &c, page_buf);

    for (uint32_t i = 0; i < data_blocks; i++)
        free_blocks += bat[i] == 0;

    write_bat_copy(GCFS_BLOCK_BAT, 0, blocks, bat, last_alloc, free_blocks, page_buf);
    write_bat_copy(GCFS_BLOCK_BAT + 1U, 1, blocks, bat, last_alloc, free_blocks, page_buf);
}
