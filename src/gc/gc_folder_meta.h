#pragma once

/* The files in a folder card's .flippermeta directory. See doc/folder-memory-cards.md for
 * the formats. Any change to a layout here must bump its version. */

#include <stdbool.h>
#include <stdint.h>

#include "gc_fs.h"
#include "gc_gci.h"

#define GF_META_DIR    ".flippermeta"
#define GF_STAGE_DIR   GF_META_DIR "/stage"
#define GF_TRASH_DIR   GF_META_DIR "/trash"
#define GF_CARD_FILE   GF_META_DIR "/card.dat"
#define GF_SAVES_FILE  GF_META_DIR "/saves.dat"
#define GF_JOURNAL     GF_META_DIR "/journal.dat"
#define GF_ERROR_LOG   GF_META_DIR "/import-errors.txt"

#define GF_STAGE_SAVES 200U
#define GF_STAGE_CARD  201U

/* One save of the committed state. All of it is cheap to keep in SRAM except the host
 * name, which is read back from saves.dat when it is needed. */
typedef struct {
    gci_id_t id;
    uint32_t entry_crc;
    uint32_t data_crc;
    uint32_t name_hash;
    uint16_t blocks;
    uint8_t rec;  /* record of saves.dat */
    bool used;
} gf_save_t;

typedef struct {
    uint8_t entry[GCFS_ENTRY_SIZE];
    uint32_t data_crc;
    uint32_t slot;
    char name[GCI_HOST_NAME_MAX];
} gf_record_t;

typedef enum {
    GF_OP_REPLACE = 1, /* copy the stage file over path */
    GF_OP_REMOVE = 2,
    GF_OP_TRASH = 3    /* copy path into the trash, then remove it */
} gf_op_type_t;

typedef struct {
    uint32_t type;
    uint32_t stage;    /* stage file number */
    uint32_t size;     /* of the stage file */
    uint32_t crc;      /* of the stage file */
    char path[GCI_HOST_NAME_MAX + 32U]; /* relative to the card folder */
} gf_op_t;

/* card.dat is the first 512 bytes of the card, exactly. */
typedef enum { GF_CARD_MISSING, GF_CARD_OK, GF_CARD_DAMAGED } gf_card_read_t;
gf_card_read_t gf_card_read(uint8_t header[GCFS_HEADER_SIZE]);

/* saves.dat. A file that fails its CRC reads as missing, since every save can be rebuilt
 * from the .gci files themselves. */
int gf_saves_open_read(uint32_t *seq, uint32_t *count);
bool gf_saves_read_record(int fd, uint32_t rec, gf_record_t *out);
int gf_saves_open_write(const char *path, uint32_t seq, uint32_t *crc);
bool gf_saves_write_record(int fd, const gf_record_t *rec, uint32_t *crc);
bool gf_saves_finish(int fd, uint32_t count, uint32_t crc);
/* For a file about to go into the journal: what must be a good saves.dat, or nothing is
 * committed. Returns its size and the CRC of the whole file. */
bool gf_saves_check(const char *path, uint32_t *size, uint32_t *crc);

/* The journal: a list of operations that takes effect when its trailer is written. */
int gf_journal_begin(uint32_t seq, uint32_t *crc);
bool gf_journal_add(int fd, const gf_op_t *op, uint32_t *crc, uint32_t *count);
bool gf_journal_commit(int fd, uint32_t crc, uint32_t count);

/* BROKEN: incomplete, so it never took effect. ERROR: it could not be read, which says
 * nothing about it. */
typedef enum { GF_JOURNAL_NONE, GF_JOURNAL_VALID, GF_JOURNAL_BROKEN, GF_JOURNAL_ERROR } gf_journal_state_t;
gf_journal_state_t gf_journal_check(uint32_t *seq, uint32_t *count);
bool gf_journal_read_op(int fd, uint32_t index, gf_op_t *op);

/* Reads a whole file and returns its CRC. False if it cannot, or its size differs. */
bool gf_file_crc(const char *path, uint32_t size, uint32_t *crc);
