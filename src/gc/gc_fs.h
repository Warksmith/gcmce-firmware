#pragma once

/* The GameCube memory card filesystem, as the console's CARD library and Dolphin see it.
 * Nothing here depends on the RP2040: card contents are reached through gc_folder_port.h,
 * so the host tests run this same code. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GCFS_BLOCK_SIZE     0x2000U
#define GCFS_PAGE_SIZE      512U
#define GCFS_PAGES_PER_BLOCK (GCFS_BLOCK_SIZE / GCFS_PAGE_SIZE)
#define GCFS_SYS_BLOCKS     5U
#define GCFS_DIR_ENTRIES    127U
#define GCFS_ENTRY_SIZE     64U
#define GCFS_MAX_BLOCKS     1024U /* a 64 Mbit card, the largest this firmware emulates */

#define GCFS_BLOCK_HEADER   0U
#define GCFS_BLOCK_DIR      1U /* and 2 */
#define GCFS_BLOCK_BAT      3U /* and 4 */

/* header */
#define GCFS_HEADER_SIZE      512U
#define GCFS_HDR_SIZE_MBITS   0x22U
#define GCFS_HDR_ENCODING     0x24U
#define GCFS_HDR_CHECKSUM     0x1FCU

/* directory block */
#define GCFS_DIR_UPDATE       0x1FFAU
#define GCFS_DIR_CHECKSUM     0x1FFCU

/* block allocation table */
#define GCFS_BAT_CHECKSUM     0x0000U
#define GCFS_BAT_UPDATE       0x0004U
#define GCFS_BAT_FREE         0x0006U
#define GCFS_BAT_LAST_ALLOC   0x0008U
#define GCFS_BAT_MAP          0x000AU
#define GCFS_BAT_END          0xFFFFU

/* directory entry */
#define GCFS_ENT_GAMECODE     0x00U
#define GCFS_ENT_MAKER        0x04U
#define GCFS_ENT_FILENAME     0x08U
#define GCFS_ENT_FILENAME_LEN 32U
#define GCFS_ENT_PERMISSIONS  0x34U
#define GCFS_ENT_FIRST_BLOCK  0x36U
#define GCFS_ENT_BLOCK_COUNT  0x38U

typedef enum {
    GCFS_OK = 0,
    GCFS_ERR_NO_DIR,   /* neither directory copy has a valid checksum */
    GCFS_ERR_NO_BAT,   /* neither BAT copy is valid */
    GCFS_ERR_ENTRY,    /* an entry has a block count that cannot be right */
    GCFS_ERR_CHAIN,    /* a chain is broken, cyclic, too short, too long or out of range */
    GCFS_ERR_OVERLAP,  /* two saves own the same block */
    GCFS_ERR_DUPLICATE /* two entries are the same save to the CARD library */
} gcfs_result_t;

/* The filesystem the console would see right now. */
typedef struct {
    uint16_t blocks;      /* on the whole card, including the system blocks */
    uint16_t free_blocks;
    uint32_t dir_addr;    /* the active directory block */
    uint16_t bat[GCFS_MAX_BLOCKS - GCFS_SYS_BLOCKS]; /* in host byte order */
} gcfs_view_t;

static inline uint16_t gcfs_get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void gcfs_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint32_t gcfs_block_addr(uint32_t block) {
    return block * GCFS_BLOCK_SIZE;
}

/* The two running sums of the CARD library. Counts 16-bit big-endian words, so every
 * update but the last must cover an even number of bytes. */
typedef struct {
    uint16_t sum;
    uint16_t inv;
} gcfs_checksum_t;

void gcfs_checksum_update(gcfs_checksum_t *c, const uint8_t *data, size_t len);
void gcfs_checksum_store(const gcfs_checksum_t *c, uint8_t *dst);
bool gcfs_checksum_matches(const gcfs_checksum_t *c, const uint8_t *stored);

bool gcfs_size_valid(uint32_t mbits);
uint32_t gcfs_blocks_for_size(uint32_t mbits);

/* Same fields as a card this firmware creates in .raw mode. */
void gcfs_header_init(uint8_t header[GCFS_HEADER_SIZE], uint32_t mbits, uint8_t encoding);
bool gcfs_header_valid(const uint8_t header[GCFS_HEADER_SIZE]);
uint32_t gcfs_header_mbits(const uint8_t header[GCFS_HEADER_SIZE]);
/* Keeps the identity and updates the size. */
void gcfs_header_set_mbits(uint8_t header[GCFS_HEADER_SIZE], uint32_t mbits);

/* An entry is in use when its game code does not start with 0xFF. */
static inline bool gcfs_entry_used(const uint8_t *entry) {
    return entry[GCFS_ENT_GAMECODE] != 0xFF;
}

/* Picks the directory and BAT the CARD library would use and validates each copy. */
gcfs_result_t gcfs_view_load(gcfs_view_t *view, uint32_t blocks, uint8_t *page_buf);

/* Reads an entry of the active directory. */
void gcfs_view_entry(const gcfs_view_t *view, uint32_t slot, uint8_t *entry);

/* Follows a chain from the entry and claims its blocks in owned, a bitmap of block
 * numbers. Each call checks every link. */
gcfs_result_t gcfs_chain_check(const gcfs_view_t *view, const uint8_t *entry, uint8_t *owned);

/* Validates every entry and chain against the view. */
gcfs_result_t gcfs_view_validate(const gcfs_view_t *view, uint8_t *owned);

/* Next block of a chain, or GCFS_BAT_END. The caller has already validated the chain. */
static inline uint16_t gcfs_view_next(const gcfs_view_t *view, uint16_t block) {
    return view->bat[block - GCFS_SYS_BLOCKS];
}

/* Writes both directory and both BAT copies from the entries already in directory copy
 * 0 and from bat, with no pending state in either: a card as if freshly written. */
void gcfs_commit_tables(uint32_t blocks, const uint16_t *bat, uint16_t last_alloc, uint8_t *page_buf);
