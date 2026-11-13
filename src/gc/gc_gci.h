#pragma once

/* GCI save files: a 64-byte directory entry followed by the save's blocks in order, the
 * format Dolphin and GCMM read and write. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GCI_HEADER_SIZE 64U

/* Longest host name a folder card handles, with its terminator. */
#define GCI_HOST_NAME_MAX 128U

typedef enum {
    GCI_OK = 0,
    GCI_BAD_ENTRY, /* the entry cannot be a save */
    GCI_BAD_SIZE   /* the file size does not match the entry's block count */
} gci_result_t;

/* A save as the CARD library tells saves apart: game code, maker code and the file name
 * up to its first NUL. Two 32-bit hashes, because this is what is kept in memory. */
typedef struct {
    uint32_t a;
    uint32_t b;
} gci_id_t;

gci_result_t gci_check(const uint8_t *entry, uint32_t file_size);
void gci_identity(const uint8_t *entry, gci_id_t *id);

static inline bool gci_id_equal(const gci_id_t *x, const gci_id_t *y) {
    return x->a == y->a && x->b == y->b;
}

/* CRC of the entry without first_block, which the card decides and the save does not. */
uint32_t gci_entry_crc(const uint8_t *entry);

/* True for a name ending in .gci, in any case, that is not a macOS resource fork. */
bool gci_is_save_name(const char *name);

/* The name a new save gets on the SD card: maker-gamecode-filename.gci, as Dolphin names
 * them, with %XX for anything a host filesystem or the FAT case rules could change.
 * suffix adds a tie breaker for names that collide. */
void gci_default_name(const uint8_t *entry, uint32_t suffix, char *name, size_t size);

/* Case-insensitive hash of a host name, since FAT and exFAT are case-insensitive. */
uint32_t gci_name_hash(const char *name);

/* CRC-32, reflected, polynomial 0xEDB88320. Start with 0. */
void gf_crc_init(void);
uint32_t gf_crc32(uint32_t crc, const void *data, size_t len);
