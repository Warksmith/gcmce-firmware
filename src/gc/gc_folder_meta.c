#include "gc_folder_meta.h"

#include <stdio.h>
#include <string.h>

#include "gc_folder_internal.h"
#include "gc_folder_port.h"
#include "sd.h"

#define SAVES_MAGIC     "GCFSAVE1"
#define SAVES_END       "GCFSEND1"
#define SAVES_VERSION   1U
#define SAVES_HEAD      20U
#define SAVES_TRAIL     16U
#define RECORD_SIZE     (GCFS_ENTRY_SIZE + 8U + GCI_HOST_NAME_MAX)

#define JOURNAL_MAGIC   "GCFJRN01"
#define JOURNAL_END     "GCFJEND1"
#define JOURNAL_HEAD    16U
#define JOURNAL_TRAIL   16U
#define OP_PATH_SIZE    ((uint32_t)sizeof(((gf_op_t *)0)->path))
#define OP_SIZE         (OP_PATH_SIZE + 16U)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool write_all(int fd, const void *data, uint32_t len) {
    return sd_write(fd, (void *)data, len) == (int)len;
}

static bool read_all(int fd, void *data, uint32_t len) {
    return sd_read(fd, data, len) == (int)len;
}

gf_card_read_t gf_card_read(uint8_t header[GCFS_HEADER_SIZE]) {
    char path[GF_PATH_MAX];
    int fd;
    bool ok;

    gf_path(path, GF_CARD_FILE);
    if (!sd_exists(path))
        return GF_CARD_MISSING;

    fd = sd_open(path, O_RDONLY);
    if (fd < 0)
        return GF_CARD_DAMAGED;

    ok = sd_filesize(fd) == (int)GCFS_HEADER_SIZE && read_all(fd, header, GCFS_HEADER_SIZE);
    sd_close(fd);
    return ok && gcfs_header_valid(header) ? GF_CARD_OK : GF_CARD_DAMAGED;
}

static int saves_open_checked(const char *path, uint32_t *seq, uint32_t *count, uint32_t *file_crc) {
    uint8_t head[SAVES_HEAD];
    uint8_t trail[SAVES_TRAIL];
    uint32_t crc = 0;
    int fd;
    int size;

    fd = sd_open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size = sd_filesize(fd);
    if (size < (int)(SAVES_HEAD + SAVES_TRAIL) || (uint32_t)size > SAVES_HEAD + SAVES_TRAIL + GCFS_DIR_ENTRIES * RECORD_SIZE)
        goto bad;
    if (!read_all(fd, head, SAVES_HEAD) || memcmp(head, SAVES_MAGIC, 8) != 0 || get32(&head[8]) != SAVES_VERSION
        || get32(&head[12]) != RECORD_SIZE)
        goto bad;
    crc = gf_crc32(crc, head, SAVES_HEAD);

    uint32_t body = (uint32_t)size - SAVES_HEAD - SAVES_TRAIL;

    if (body % RECORD_SIZE != 0)
        goto bad;
    for (uint32_t left = body; left > 0;) {
        uint32_t chunk = left < GCFS_PAGE_SIZE ? left : GCFS_PAGE_SIZE;

        if (!read_all(fd, gf_page, chunk))
            goto bad;
        crc = gf_crc32(crc, gf_page, chunk);
        left -= chunk;
    }

    if (!read_all(fd, trail, SAVES_TRAIL) || memcmp(&trail[8], SAVES_END, 8) != 0
        || get32(trail) != body / RECORD_SIZE)
        goto bad;
    crc = gf_crc32(crc, trail, 4);
    if (crc != get32(&trail[4]))
        goto bad;

    *seq = get32(&head[16]);
    *count = body / RECORD_SIZE;
    *file_crc = gf_crc32(crc, &trail[4], SAVES_TRAIL - 4U);
    return fd;

bad:
    sd_close(fd);
    PF_LOG("saves.dat is damaged");
    return -1;
}

/* Checks the whole file, so that every record read later can be trusted. */
int gf_saves_open_read(uint32_t *seq, uint32_t *count) {
    char path[GF_PATH_MAX];
    uint32_t file_crc;

    gf_path(path, GF_SAVES_FILE);
    if (!sd_exists(path))
        return -1;
    return saves_open_checked(path, seq, count, &file_crc);
}

bool gf_saves_check(const char *path, uint32_t *size, uint32_t *crc) {
    uint32_t seq, count;
    int fd = saves_open_checked(path, &seq, &count, crc);

    if (fd < 0)
        return false;
    *size = SAVES_HEAD + SAVES_TRAIL + count * RECORD_SIZE;
    sd_close(fd);
    return true;
}

bool gf_saves_read_record(int fd, uint32_t rec, gf_record_t *out) {
    uint8_t tail[8];

    if (sd_seek(fd, (int32_t)(SAVES_HEAD + rec * RECORD_SIZE), SEEK_SET) != 0)
        return false;
    if (!read_all(fd, out->entry, GCFS_ENTRY_SIZE) || !read_all(fd, tail, sizeof(tail))
        || !read_all(fd, out->name, GCI_HOST_NAME_MAX))
        return false;

    out->data_crc = get32(tail);
    out->slot = get32(&tail[4]);
    out->name[GCI_HOST_NAME_MAX - 1U] = 0;
    return out->slot < GCFS_DIR_ENTRIES;
}

int gf_saves_open_write(const char *path, uint32_t seq, uint32_t *crc) {
    uint8_t head[SAVES_HEAD];
    int fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;

    memcpy(head, SAVES_MAGIC, 8);
    put32(&head[8], SAVES_VERSION);
    put32(&head[12], RECORD_SIZE);
    put32(&head[16], seq);
    *crc = gf_crc32(0, head, SAVES_HEAD);
    if (!write_all(fd, head, SAVES_HEAD)) {
        sd_close(fd);
        return -1;
    }
    return fd;
}

bool gf_saves_write_record(int fd, const gf_record_t *rec, uint32_t *crc) {
    char name[GCI_HOST_NAME_MAX];
    uint8_t tail[8];

    memset(name, 0, sizeof(name));
    strncpy(name, rec->name, sizeof(name) - 1U);
    put32(tail, rec->data_crc);
    put32(&tail[4], rec->slot);

    *crc = gf_crc32(*crc, rec->entry, GCFS_ENTRY_SIZE);
    *crc = gf_crc32(*crc, tail, sizeof(tail));
    *crc = gf_crc32(*crc, name, sizeof(name));

    return write_all(fd, rec->entry, GCFS_ENTRY_SIZE) && write_all(fd, tail, sizeof(tail))
        && write_all(fd, name, sizeof(name));
}

bool gf_saves_finish(int fd, uint32_t count, uint32_t crc) {
    uint8_t trail[SAVES_TRAIL];
    bool ok;

    put32(trail, count);
    crc = gf_crc32(crc, trail, 4);
    put32(&trail[4], crc);
    memcpy(&trail[8], SAVES_END, 8);

    ok = write_all(fd, trail, SAVES_TRAIL);
    return (sd_close(fd) == 0) && ok;
}

int gf_journal_begin(uint32_t seq, uint32_t *crc) {
    char path[GF_PATH_MAX];
    uint8_t head[JOURNAL_HEAD];
    int fd;

    gf_path(path, GF_JOURNAL);
    fd = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;

    memcpy(head, JOURNAL_MAGIC, 8);
    put32(&head[8], seq);
    put32(&head[12], 0);
    *crc = gf_crc32(0, head, JOURNAL_HEAD);
    if (!write_all(fd, head, JOURNAL_HEAD)) {
        sd_close(fd);
        return -1;
    }
    return fd;
}

/* The last bytes of an operation's path field hold the CRC of the operation: the journal's
 * own CRC is only seen once, and a wrong path here would write to the wrong file. */
static uint32_t op_crc(const uint8_t *head, const char *path) {
    return gf_crc32(gf_crc32(0, head, 16), path, OP_PATH_SIZE - 4U);
}

bool gf_journal_add(int fd, const gf_op_t *op, uint32_t *crc, uint32_t *count) {
    uint8_t head[16];
    char path[OP_PATH_SIZE];

    memset(path, 0, sizeof(path));
    strncpy(path, op->path, OP_PATH_SIZE - 5U);
    put32(head, op->type);
    put32(&head[4], op->stage);
    put32(&head[8], op->size);
    put32(&head[12], op->crc);
    put32((uint8_t *)&path[OP_PATH_SIZE - 4U], op_crc(head, path));

    *crc = gf_crc32(*crc, head, sizeof(head));
    *crc = gf_crc32(*crc, path, sizeof(path));
    (*count)++;
    return write_all(fd, head, sizeof(head)) && write_all(fd, path, sizeof(path));
}

/* The trailer is what makes the journal count, so nothing can be read into a journal that
 * lost power before this point. */
bool gf_journal_commit(int fd, uint32_t crc, uint32_t count) {
    uint8_t trail[JOURNAL_TRAIL];
    bool ok;

    put32(trail, count);
    crc = gf_crc32(crc, trail, 4);
    put32(&trail[4], crc);
    memcpy(&trail[8], JOURNAL_END, 8);

    ok = write_all(fd, trail, JOURNAL_TRAIL);
    return (sd_close(fd) == 0) && ok;
}

gf_journal_state_t gf_journal_check(uint32_t *seq, uint32_t *count) {
    char path[GF_PATH_MAX];
    uint8_t head[JOURNAL_HEAD];
    uint8_t trail[JOURNAL_TRAIL];
    gf_journal_state_t state = GF_JOURNAL_BROKEN;
    uint32_t crc;
    uint32_t body;
    int fd;
    int size;

    gf_path(path, GF_JOURNAL);
    if (!sd_exists(path))
        return GF_JOURNAL_NONE;

    fd = sd_open(path, O_RDONLY);
    if (fd < 0)
        return GF_JOURNAL_ERROR;

    /* The trailer is written last, and the size alone says whether it is there. Without it
     * the journal never took effect. With it the transaction did, and is not to be given up
     * on: from here on anything wrong says nothing about the journal, as a bad read could
     * make a good one look like this. */
    size = sd_filesize(fd);
    if (size < (int)(JOURNAL_HEAD + JOURNAL_TRAIL))
        goto done;
    body = (uint32_t)size - JOURNAL_HEAD - JOURNAL_TRAIL;
    if (body % OP_SIZE != 0)
        goto done;

    state = GF_JOURNAL_ERROR;
    if (sd_seek(fd, size - (int)JOURNAL_TRAIL, SEEK_SET) != 0 || !read_all(fd, trail, JOURNAL_TRAIL)
        || sd_seek(fd, 0, SEEK_SET) != 0 || !read_all(fd, head, JOURNAL_HEAD))
        goto done;
    if (memcmp(&trail[8], JOURNAL_END, 8) != 0 || memcmp(head, JOURNAL_MAGIC, 8) != 0)
        goto done;

    crc = gf_crc32(0, head, JOURNAL_HEAD);
    for (uint32_t left = body; left > 0;) {
        uint32_t chunk = left < GCFS_PAGE_SIZE ? left : GCFS_PAGE_SIZE;

        if (!read_all(fd, gf_page, chunk))
            goto done;
        crc = gf_crc32(crc, gf_page, chunk);
        left -= chunk;
    }

    crc = gf_crc32(crc, trail, 4);
    if (get32(trail) != body / OP_SIZE || crc != get32(&trail[4]))
        goto done;

    *seq = get32(&head[8]);
    *count = body / OP_SIZE;
    state = GF_JOURNAL_VALID;

done:
    sd_close(fd);
    return state;
}

bool gf_journal_read_op(int fd, uint32_t index, gf_op_t *op) {
    uint8_t head[16];

    if (sd_seek(fd, (int32_t)(JOURNAL_HEAD + index * OP_SIZE), SEEK_SET) != 0)
        return false;
    if (!read_all(fd, head, sizeof(head)) || !read_all(fd, op->path, sizeof(op->path)))
        return false;

    op->type = get32(head);
    op->stage = get32(&head[4]);
    op->size = get32(&head[8]);
    op->crc = get32(&head[12]);
    return op_crc(head, op->path) == get32((const uint8_t *)&op->path[OP_PATH_SIZE - 4U]);
}

bool gf_file_crc(const char *path, uint32_t size, uint32_t *crc) {
    int fd = sd_open(path, O_RDONLY);
    uint32_t c = 0;
    bool ok;

    if (fd < 0)
        return false;

    ok = sd_filesize(fd) == (int)size;
    for (uint32_t left = size; ok && left > 0;) {
        uint32_t chunk = left < GCFS_PAGE_SIZE ? left : GCFS_PAGE_SIZE;

        ok = read_all(fd, gf_page, chunk);
        if (ok)
            c = gf_crc32(c, gf_page, chunk);
        left -= chunk;
    }
    sd_close(fd);
    *crc = c;
    return ok;
}
