/* Turns what the console left in PSRAM into .gci files.
 *
 * A pass reads the filesystem the way the console's CARD library would, validates it, and
 * compares it with the committed state. New versions go to .flippermeta/stage first. Once
 * they are all there the journal is written, and that write is the commit point: from then
 * on the change is applied, or redone by the next mount if the power goes. Before it,
 * nothing outside .flippermeta has changed.
 *
 * The console keeps running throughout. The write generation is compared before the
 * journal is committed, so that a commit always shows the card at a single moment. */

#include <stdio.h>
#include <string.h>

#include "gc_folder_internal.h"
#include "gc_folder_port.h"
#include "sd.h"

gf_save_t gf_next[GCFS_DIR_ENTRIES];

static struct {
    uint32_t gen;
    uint8_t match[GCFS_DIR_ENTRIES];  /* committed slot of the save now in this slot */
    uint8_t stage[(GCFS_DIR_ENTRIES + 7U) / 8U];
    uint32_t stage_crc[GCFS_DIR_ENTRIES];
    uint32_t suffix[GCFS_DIR_ENTRIES];

    uint8_t header[GCFS_HEADER_SIZE];
    uint32_t header_crc;
    bool header_changed;

    uint32_t slot;
    uint16_t block;
    uint32_t page;
    uint32_t blocks_left;
    uint32_t crc;
    int fd;
    uint8_t entry[GCFS_ENTRY_SIZE];

    bool verifying;
    uint32_t verify_left;
    uint32_t verify_file;
    uint32_t verify_data;
} sy;

static struct {
    int journal;
    int src;
    int dst;
    uint32_t index;
    uint32_t count;
    uint32_t seq;
    uint32_t crc;
    gf_op_t op;
    bool copying;
    bool from_sync;
} ap;

static char path_a[GF_PATH_MAX];
static char path_b[GF_PATH_MAX];

static bool slot_staged(uint32_t slot) {
    return sy.stage[slot / 8U] & (1U << (slot % 8U));
}

static void set_staged(uint32_t slot, bool staged) {
    if (staged)
        sy.stage[slot / 8U] |= (uint8_t)(1U << (slot % 8U));
    else
        sy.stage[slot / 8U] &= (uint8_t)~(1U << (slot % 8U));
}

static void stage_path(char *out, uint32_t id) {
    snprintf(out, GF_PATH_MAX, "%s/%s/%lu.bin", gf.path, GF_STAGE_DIR, (unsigned long)id);
}

static void trash_dir(char *out, uint32_t seq) {
    snprintf(out, GF_PATH_MAX, "%s/%s/%08lX", gf.path, GF_TRASH_DIR, (unsigned long)seq);
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        sd_close(*fd);
        *fd = -1;
    }
}

/* Empties the stage directory, which holds nothing of value once no journal needs it. */
static void stage_sweep(void) {
    static char name[GCI_HOST_NAME_MAX];

    gf_path(path_a, GF_STAGE_DIR);
    for (;;) {
        int dir = sd_open(path_a, O_RDONLY);
        int it;

        if (dir < 0)
            return;

        name[0] = 0;
        it = sd_iterate_dir(dir, -1);
        if (it >= 0) {
            sd_get_name(it, name, sizeof(name));
            sd_close(it);
        }
        sd_close(dir);

        if (!name[0])
            return;
        snprintf(path_b, sizeof(path_b), "%s/%s", path_a, name);
        if (sd_remove(path_b) != 0)
            return;
    }
}

static void apply_close(void) {
    close_fd(&ap.journal);
    close_fd(&ap.src);
    close_fd(&ap.dst);
    ap.copying = false;
}

static void apply_start(uint32_t count, uint32_t seq, bool from_sync) {
    ap.journal = ap.src = ap.dst = -1;
    ap.index = 0;
    ap.count = count;
    ap.seq = seq;
    ap.copying = false;
    ap.from_sync = from_sync;
    gf.phase = GF_SY_APPLY;
}

static void apply_failed(void) {
    apply_close();
    gf.state = GF_S_SAVE_FAILED;
    gf.retry_at_us = pf_time_us() + GF_RETRY_US;
    PF_LOG("Synchronization failed while applying");
}

static bool copy_begin(const char *src, const char *dst) {
    ap.src = sd_open(src, O_RDONLY);
    if (ap.src < 0)
        return false;
    ap.dst = sd_open(dst, O_RDWR | O_CREAT | O_TRUNC);
    if (ap.dst < 0) {
        close_fd(&ap.src);
        return false;
    }
    ap.crc = 0;
    ap.copying = true;
    return true;
}

/* False on an error. Clears ap.copying when the copy has finished. */
static bool copy_slice(uint64_t start_us) {
    do {
        int got = sd_read(ap.src, gf_page, GCFS_PAGE_SIZE);

        if (got < 0)
            return false;
        if (got == 0) {
            bool ok;

            close_fd(&ap.src);
            sd_flush(ap.dst);
            ok = sd_close(ap.dst) == 0;
            ap.dst = -1;
            ap.copying = false;
            return ok && (ap.op.type != GF_OP_REPLACE || ap.crc == ap.op.crc);
        }
        ap.crc = gf_crc32(ap.crc, gf_page, (size_t)got);
        if (sd_write(ap.dst, gf_page, (size_t)got) != got)
            return false;
    } while (gf_time_left(start_us));
    return true;
}

/* Operations are idempotent, because a power cut can repeat any of them. */
static bool op_start(void) {
    gf_path(path_a, ap.op.path);

    switch (ap.op.type) {
        case GF_OP_REPLACE:
            stage_path(path_b, ap.op.stage);
            return copy_begin(path_b, path_a);
        case GF_OP_REMOVE:
            return !sd_exists(path_a) || sd_remove(path_a) == 0;
        case GF_OP_TRASH:
            /* The source only goes away after a complete copy, so a source that is missing
             * means this operation already ran. Nothing else in the transaction can have
             * taken its name: see name_taken(). */
            if (!sd_exists(path_a))
                return true;
            gf_path(path_b, GF_TRASH_DIR);
            sd_mkdir(path_b);
            trash_dir(path_b, ap.seq);
            sd_mkdir(path_b);
            snprintf(path_b + strlen(path_b), GF_PATH_MAX - strlen(path_b), "/%s", ap.op.path);
            return copy_begin(path_a, path_b);
        default:
            return false;
    }
}

static bool op_finish(void) {
    if (ap.op.type != GF_OP_TRASH)
        return true;
    gf_path(path_a, ap.op.path);
    return !sd_exists(path_a) || sd_remove(path_a) == 0;
}

/* Runs the journal, from its start again after any failure. Returns GF_SY_IDLE when the
 * transaction is complete. */
gf_sync_phase_t gf_apply_step(uint64_t start_us) {
    if (ap.journal < 0) {
        gf_path(path_a, GF_JOURNAL);
        ap.journal = sd_open(path_a, O_RDONLY);
        ap.index = 0;
        if (ap.journal < 0) {
            apply_failed();
            return GF_SY_APPLY;
        }
    }

    do {
        if (ap.copying) {
            if (!copy_slice(start_us)) {
                apply_failed();
                return GF_SY_APPLY;
            }
            if (ap.copying)
                return GF_SY_APPLY;
            if (!op_finish()) {
                apply_failed();
                return GF_SY_APPLY;
            }
            ap.index++;
            continue;
        }

        if (ap.index >= ap.count) {
            apply_close();
            gf_path(path_a, GF_JOURNAL);
            if (sd_remove(path_a) != 0) {
                apply_failed();
                return GF_SY_APPLY;
            }
            stage_sweep();
            gf.phase = GF_SY_IDLE;
            return GF_SY_IDLE;
        }

        if (!gf_journal_read_op(ap.journal, ap.index, &ap.op) || !op_start()) {
            apply_failed();
            return GF_SY_APPLY;
        }
        if (!ap.copying)
            ap.index++;
    } while (gf_time_left(start_us));

    return GF_SY_APPLY;
}

gf_error_t gf_recover(void) {
    static gf_op_t op;
    uint32_t seq = 0;
    uint32_t count = 0;
    int fd;

    switch (gf_journal_check(&seq, &count)) {
        case GF_JOURNAL_NONE:
            stage_sweep();
            return GF_E_NONE;
        case GF_JOURNAL_BROKEN:
            /* Never committed, so nothing was applied. */
            PF_LOG("Discarding a transaction that was not committed");
            gf_path(path_a, GF_JOURNAL);
            if (sd_remove(path_a) != 0)
                return GF_E_SD;
            stage_sweep();
            return GF_E_NONE;
        case GF_JOURNAL_ERROR:
            return GF_E_SD;
        default:
            break;
    }

    /* Before touching anything, make sure the transaction can be completed. */
    gf_path(path_a, GF_JOURNAL);
    fd = sd_open(path_a, O_RDONLY);
    if (fd < 0)
        return GF_E_SD;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t crc = 0;

        if (!gf_journal_read_op(fd, i, &op)) {
            sd_close(fd);
            return GF_E_SD;
        }
        if (op.type != GF_OP_REPLACE)
            continue;
        stage_path(path_a, op.stage);
        if (!gf_file_crc(path_a, op.size, &crc) || crc != op.crc) {
            sd_close(fd);
            return GF_E_RECOVERY;
        }
    }
    sd_close(fd);

    PF_LOG("Recovery required: redoing %lu operations", (unsigned long)count);
    apply_start(count, seq, false);

    while (gf.phase == GF_SY_APPLY) {
        gf_apply_step(pf_time_us());
        if (gf.state == GF_S_SAVE_FAILED) {
            gf.state = GF_S_LOADING;
            gf.phase = GF_SY_IDLE;
            return GF_E_SD;
        }
    }
    return GF_E_NONE;
}

void gf_sync_reset(void) {
    memset(&sy, 0, sizeof(sy));
    memset(&ap, 0, sizeof(ap));
    sy.fd = ap.journal = ap.src = ap.dst = -1;
}

void gf_sync_abandon(void) {
    close_fd(&sy.fd);
    apply_close();
    gf.phase = GF_SY_IDLE;
}

static void scan_abandon(void) {
    close_fd(&sy.fd);
    gf.phase = GF_SY_IDLE;
    gf.state = GF_S_READY;
}

static bool any_block_dirty(const uint8_t *entry) {
    uint16_t block = gcfs_get16(&entry[GCFS_ENT_FIRST_BLOCK]);
    uint32_t count = gcfs_get16(&entry[GCFS_ENT_BLOCK_COUNT]);

    for (uint32_t i = 0; i < count; i++) {
        if (gf.dirty_scan[block / 8U] & (1U << (block % 8U)))
            return true;
        block = gcfs_view_next(&gf_view, block);
    }
    return false;
}

static void set_invalid(gcfs_result_t result) {
    PF_LOG("Invalid BAT chain or directory (%d), nothing saved", (int)result);
    gf.state = GF_S_INVALID;
    gf.phase = GF_SY_IDLE;
    gf.seen_gen = sy.gen;
}

/* Plans the pass from a validated view of the card. */
static void scan_begin(uint32_t gen) {
    gcfs_result_t result;

    sy.gen = gen;
    for (uint32_t i = 0; i < sizeof(gf.dirty_scan); i++) {
        gf.dirty_scan[i] |= gf.dirty_now[i];
        gf.dirty_now[i] = 0;
    }

    pf_psram_read(0, sy.header, GCFS_HEADER_SIZE);
    if (!gcfs_header_valid(sy.header) || gcfs_header_mbits(sy.header) != gf.mbits) {
        set_invalid(GCFS_ERR_NO_DIR);
        return;
    }
    sy.header_crc = gf_crc32(0, sy.header, GCFS_HEADER_SIZE);
    sy.header_changed = sy.header_crc != gf.header_crc;

    result = gcfs_view_load(&gf_view, gf.blocks, gf_page);
    if (result == GCFS_OK)
        result = gcfs_view_validate(&gf_view, gf_owned);
    if (result != GCFS_OK) {
        set_invalid(result);
        return;
    }

    memset(sy.stage, 0, sizeof(sy.stage));

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        gf_save_t *next = &gf_next[slot];

        memset(next, 0, sizeof(*next));
        sy.match[slot] = GF_NO_SLOT;

        gcfs_view_entry(&gf_view, slot, sy.entry);
        if (!gcfs_entry_used(sy.entry))
            continue;

        uint32_t blocks = gcfs_get16(&sy.entry[GCFS_ENT_BLOCK_COUNT]);

        /* A save that the next mount would refuse cannot be stored as a file. */
        if (gci_check(sy.entry, GCI_HEADER_SIZE + blocks * GCFS_BLOCK_SIZE) != GCI_OK) {
            set_invalid(GCFS_ERR_ENTRY);
            return;
        }

        gci_identity(sy.entry, &next->id);
        next->entry_crc = gci_entry_crc(sy.entry);
        next->blocks = (uint16_t)blocks;
        next->used = true;

        for (uint32_t b = 0; b < GCFS_DIR_ENTRIES; b++) {
            if (gf.base[b].used && gci_id_equal(&gf.base[b].id, &next->id)) {
                sy.match[slot] = (uint8_t)b;
                break;
            }
        }

        if (sy.match[slot] == GF_NO_SLOT) {
            set_staged(slot, true);
        } else {
            const gf_save_t *base = &gf.base[sy.match[slot]];

            next->data_crc = base->data_crc;
            next->name_hash = base->name_hash;
            if (base->entry_crc != next->entry_crc || base->blocks != next->blocks || any_block_dirty(sy.entry))
                set_staged(slot, true);
        }
    }

    sy.slot = 0;
    sy.verifying = false;
    gf.phase = GF_SY_STAGE;
    gf.state = GF_S_SYNCING;
    PF_LOG("Reconciliation started");
}

static bool stage_open(uint32_t slot) {
    gcfs_view_entry(&gf_view, slot, sy.entry);
    stage_path(path_a, slot);
    sy.fd = sd_open(path_a, O_RDWR | O_CREAT | O_TRUNC);
    if (sy.fd < 0)
        return false;

    sy.block = gcfs_get16(&sy.entry[GCFS_ENT_FIRST_BLOCK]);
    sy.blocks_left = gcfs_get16(&sy.entry[GCFS_ENT_BLOCK_COUNT]);
    sy.page = 0;
    sy.crc = 0;
    return sd_write(sy.fd, sy.entry, GCFS_ENTRY_SIZE) == (int)GCFS_ENTRY_SIZE;
}

/* The file is complete. Decides whether it is needed, and what is needed is read back:
 * what counts is what the SD card returns. */
static bool stage_written(uint32_t slot) {
    gf_save_t *next = &gf_next[slot];
    uint8_t entry[GCFS_ENTRY_SIZE];
    bool ok;

    sd_flush(sy.fd);
    ok = sd_close(sy.fd) == 0;
    sy.fd = -1;
    if (!ok)
        return false;

    next->data_crc = sy.crc;
    stage_path(path_a, slot);

    /* Saved again with the same bytes: there is nothing to replace. */
    if (sy.match[slot] != GF_NO_SLOT) {
        const gf_save_t *base = &gf.base[sy.match[slot]];

        if (base->entry_crc == next->entry_crc && base->data_crc == next->data_crc) {
            set_staged(slot, false);
            sy.slot++;
            return sd_remove(path_a) == 0;
        }
    }

    sy.fd = sd_open(path_a, O_RDONLY);
    if (sy.fd < 0)
        return false;
    if (sd_filesize(sy.fd) != (int)(GCI_HEADER_SIZE + (uint32_t)next->blocks * GCFS_BLOCK_SIZE))
        return false;
    if (sd_read(sy.fd, entry, GCFS_ENTRY_SIZE) != (int)GCFS_ENTRY_SIZE || memcmp(entry, sy.entry, GCFS_ENTRY_SIZE) != 0)
        return false;

    sy.verify_left = (uint32_t)next->blocks * GCFS_PAGES_PER_BLOCK;
    sy.verify_file = gf_crc32(0, entry, GCFS_ENTRY_SIZE);
    sy.verify_data = 0;
    sy.verifying = true;
    return true;
}

static bool verify_page(uint32_t slot) {
    if (sd_read(sy.fd, gf_page, GCFS_PAGE_SIZE) != (int)GCFS_PAGE_SIZE)
        return false;

    sy.verify_file = gf_crc32(sy.verify_file, gf_page, GCFS_PAGE_SIZE);
    sy.verify_data = gf_crc32(sy.verify_data, gf_page, GCFS_PAGE_SIZE);

    if (--sy.verify_left == 0) {
        close_fd(&sy.fd);
        sy.verifying = false;
        sy.stage_crc[slot] = sy.verify_file;
        sy.slot++;
        return sy.verify_data == sy.crc;
    }
    return true;
}

static void sync_failed(void) {
    close_fd(&sy.fd);
    gf.phase = GF_SY_IDLE;
    gf.state = GF_S_SAVE_FAILED;
    gf.retry_at_us = pf_time_us() + GF_RETRY_US;
    stage_sweep();
    PF_LOG("Synchronization failed");
}

/* True when the console wrote since the pass began. */
static bool generation_moved(void) {
    uint32_t gen;
    bool pending;

    pf_write_snapshot(&gen, &pending);
    return gen != sy.gen || pending;
}

static void stage_slice(bool force, uint64_t start_us) {
    do {
        if (!force && !pf_console_quiet())
            return;
        if (generation_moved()) {
            scan_abandon();
            return;
        }

        if (sy.verifying) {
            if (!verify_page(sy.slot)) {
                sync_failed();
                return;
            }
            continue;
        }

        if (sy.fd < 0) {
            while (sy.slot < GCFS_DIR_ENTRIES && !slot_staged(sy.slot))
                sy.slot++;
            if (sy.slot == GCFS_DIR_ENTRIES) {
                gf.phase = GF_SY_COMMIT;
                return;
            }
            if (!stage_open(sy.slot)) {
                sync_failed();
                return;
            }
        }

        pf_psram_read(gcfs_block_addr(sy.block) + sy.page * GCFS_PAGE_SIZE, gf_page, GCFS_PAGE_SIZE);
        sy.crc = gf_crc32(sy.crc, gf_page, GCFS_PAGE_SIZE);
        if (sd_write(sy.fd, gf_page, GCFS_PAGE_SIZE) != (int)GCFS_PAGE_SIZE) {
            sync_failed();
            return;
        }

        if (++sy.page == GCFS_PAGES_PER_BLOCK) {
            sy.page = 0;
            if (--sy.blocks_left == 0) {
                if (!stage_written(sy.slot)) {
                    sync_failed();
                    return;
                }
            } else {
                sy.block = gcfs_view_next(&gf_view, sy.block);
            }
        }
    } while (gf_time_left(start_us));
}

/* Names of saves leaving in this transaction count as taken: a repeated trash operation
 * must never find a new save under the name it is about to move. */
static bool name_taken(uint32_t hash, uint32_t skip_slot) {
    for (uint32_t i = 0; i < GCFS_DIR_ENTRIES; i++) {
        if (gf.base[i].used && gf.base[i].name_hash == hash)
            return true;
        if (i != skip_slot && gf_next[i].used && gf_next[i].name_hash == hash)
            return true;
    }
    return false;
}

static void choose_name(uint32_t slot, const uint8_t *entry, char *name) {
    uint32_t suffix = 0;

    gci_default_name(entry, 0, name, GCI_HOST_NAME_MAX);
    while (name_taken(gci_name_hash(name), slot)) {
        suffix = suffix ? suffix + 1U : gf_next[slot].id.a | 1U;
        gci_default_name(entry, suffix, name, GCI_HOST_NAME_MAX);
    }
    sy.suffix[slot] = suffix;
    gf_next[slot].name_hash = gci_name_hash(name);
}

/* Reads the host name a committed save has. */
static bool base_name(int fd, uint32_t base_slot, char *name) {
    static gf_record_t rec;

    if (fd < 0 || !gf_saves_read_record(fd, gf.base[base_slot].rec, &rec))
        return false;
    strcpy(name, rec.name);
    return gci_name_hash(name) == gf.base[base_slot].name_hash;
}

static bool record_changed(uint32_t slot) {
    const gf_save_t *base = &gf.base[slot];
    const gf_save_t *next = &gf_next[slot];

    if (base->used != next->used)
        return true;
    return next->used && (!gci_id_equal(&base->id, &next->id) || base->entry_crc != next->entry_crc
        || base->data_crc != next->data_crc);
}

static void commit_clean(void) {
    gf.phase = GF_SY_IDLE;
    gf.state = GF_S_READY;
    gf.committed_gen = sy.gen;
    memset(gf.dirty_scan, 0, sizeof(gf.dirty_scan));
}

static bool write_stage_card(void) {
    int fd;
    bool ok;

    stage_path(path_a, GF_STAGE_CARD);
    fd = sd_open(path_a, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    ok = sd_write(fd, sy.header, GCFS_HEADER_SIZE) == (int)GCFS_HEADER_SIZE;
    sd_flush(fd);
    return (sd_close(fd) == 0) && ok;
}

static bool add_replace(int fd, uint32_t stage, const char *rel, uint32_t size, uint32_t crc, uint32_t *jcrc, uint32_t *count) {
    static gf_op_t op;

    memset(&op, 0, sizeof(op));
    op.type = GF_OP_REPLACE;
    op.stage = stage;
    op.size = size;
    op.crc = crc;
    snprintf(op.path, sizeof(op.path), "%s", rel);
    return gf_journal_add(fd, &op, jcrc, count);
}

/* A wrong CRC in a committed journal could never be recovered from, so the metadata is
 * read back like a save before the journal vouches for it. */
static bool add_saves_file(int fd, uint32_t *jcrc, uint32_t *count) {
    uint32_t size = 0;
    uint32_t crc = 0;

    stage_path(path_a, GF_STAGE_SAVES);
    return gf_saves_check(path_a, &size, &crc) && add_replace(fd, GF_STAGE_SAVES, GF_SAVES_FILE, size, crc, jcrc, count);
}

static bool add_card_file(int fd, uint32_t *jcrc, uint32_t *count) {
    uint32_t crc = 0;

    stage_path(path_a, GF_STAGE_CARD);
    return gf_file_crc(path_a, GCFS_HEADER_SIZE, &crc) && crc == gf_crc32(0, sy.header, GCFS_HEADER_SIZE)
        && add_replace(fd, GF_STAGE_CARD, GF_CARD_FILE, GCFS_HEADER_SIZE, crc, jcrc, count);
}

static bool add_name_op(int fd, uint32_t type, const char *name, uint32_t *jcrc, uint32_t *count) {
    static gf_op_t op;

    memset(&op, 0, sizeof(op));
    op.type = type;
    snprintf(op.path, sizeof(op.path), "%s", name);
    return gf_journal_add(fd, &op, jcrc, count);
}

/* A format, or the last save going, is not trusted to mean the saves should be gone. */
static bool removal_goes_to_trash(uint32_t saves_left) {
    return saves_left == 0 || sy.header_changed;
}

/* The trash directory of a transaction must not exist yet, and saves.dat can be gone. */
static uint32_t next_seq(void) {
    uint32_t seq = gf.seq + 1U;

    for (;;) {
        trash_dir(path_a, seq);
        if (!sd_exists(path_a))
            return seq;
        seq++;
    }
}

/* Everything up to the journal can be undone by simply not committing it. */
static void commit_step(void) {
    static gf_record_t rec;
    static char name[GCI_HOST_NAME_MAX];
    uint32_t changed = 0;
    uint32_t saves_left = 0;
    uint32_t created = 0, modified = 0, deleted = 0;
    uint32_t saves_crc = 0;
    uint32_t journal_crc = 0;
    uint32_t count = 0;
    uint32_t records = 0;
    uint32_t file_seq = 0;
    uint32_t file_count = 0;
    uint32_t seq;
    int old_fd;
    int saves_fd;
    int journal_fd;

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        changed |= record_changed(slot) || slot_staged(slot);
        saves_left += gf_next[slot].used;
    }

    if (!changed && !sy.header_changed) {
        stage_sweep();
        commit_clean();
        return;
    }

    seq = next_seq();
    old_fd = gf_saves_open_read(&file_seq, &file_count);

    stage_path(path_a, GF_STAGE_SAVES);
    saves_fd = gf_saves_open_write(path_a, seq, &saves_crc);
    if (saves_fd < 0)
        goto fail;

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        gf_save_t *next = &gf_next[slot];

        if (!next->used)
            continue;

        gcfs_view_entry(&gf_view, slot, rec.entry);
        if (sy.match[slot] != GF_NO_SLOT) {
            if (!base_name(old_fd, sy.match[slot], rec.name))
                goto fail_saves;
        } else {
            choose_name(slot, rec.entry, rec.name);
        }
        rec.slot = slot;
        rec.data_crc = next->data_crc;
        next->rec = (uint8_t)records;
        if (!gf_saves_write_record(saves_fd, &rec, &saves_crc))
            goto fail_saves;
        records++;
    }
    if (!gf_saves_finish(saves_fd, records, saves_crc))
        goto fail;

    if (sy.header_changed && !write_stage_card())
        goto fail;

    if (generation_moved())
        goto stale;

    journal_fd = gf_journal_begin(seq, &journal_crc);
    if (journal_fd < 0)
        goto fail;

    for (uint32_t b = 0; b < GCFS_DIR_ENTRIES; b++) {
        bool kept = false;

        if (!gf.base[b].used)
            continue;
        for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++)
            kept |= sy.match[slot] == b;
        if (kept)
            continue;

        if (!base_name(old_fd, b, name)
            || !add_name_op(journal_fd, removal_goes_to_trash(saves_left) ? GF_OP_TRASH : GF_OP_REMOVE, name, &journal_crc, &count))
            goto fail_journal;
        deleted++;
    }

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        gf_save_t *next = &gf_next[slot];

        if (!next->used || !slot_staged(slot))
            continue;
        if (sy.match[slot] != GF_NO_SLOT) {
            if (!base_name(old_fd, sy.match[slot], name))
                goto fail_journal;
            modified++;
        } else {
            gcfs_view_entry(&gf_view, slot, rec.entry);
            gci_default_name(rec.entry, sy.suffix[slot], name, sizeof(name));
            created++;
        }
        if (!add_replace(journal_fd, slot, name, GCI_HEADER_SIZE + (uint32_t)next->blocks * GCFS_BLOCK_SIZE,
                         sy.stage_crc[slot], &journal_crc, &count))
            goto fail_journal;
    }

    close_fd(&old_fd);

    if (sy.header_changed && !add_card_file(journal_fd, &journal_crc, &count))
        goto fail_journal;
    if (!add_saves_file(journal_fd, &journal_crc, &count))
        goto fail_journal;

    /* The last look: a write after this belongs to the next pass. */
    if (generation_moved()) {
        sd_close(journal_fd);
        gf_path(path_a, GF_JOURNAL);
        sd_remove(path_a);
        goto stale;
    }

    if (!gf_journal_commit(journal_fd, journal_crc, count)) {
        gf_path(path_a, GF_JOURNAL);
        if (!sd_exists(path_a) || sd_remove(path_a) == 0)
            goto fail;

        /* It may well be complete on the card, and would then be redone at the next mount
         * from stage files that have to stay. Carrying on is the one consistent choice. */
        apply_start(count, seq, true);
        apply_failed();
        return;
    }

    PF_LOG("%lu created, %lu modified, %lu deleted", (unsigned long)created, (unsigned long)modified, (unsigned long)deleted);
    apply_start(count, seq, true);
    return;

stale:
    close_fd(&old_fd);
    scan_abandon();
    return;

fail_journal:
    sd_close(journal_fd);
    gf_path(path_a, GF_JOURNAL);
    sd_remove(path_a);
    goto fail;

fail_saves:
    sd_close(saves_fd);
fail:
    close_fd(&old_fd);
    sync_failed();
}

static void apply_slice(uint64_t start_us) {
    if (gf.state == GF_S_SAVE_FAILED && pf_time_us() < gf.retry_at_us)
        return;
    gf.state = GF_S_SYNCING;

    if (gf_apply_step(start_us) != GF_SY_IDLE)
        return;

    if (ap.from_sync) {
        ap.from_sync = false;
        memcpy(gf.base, gf_next, sizeof(gf.base));
        memcpy(gf.header, sy.header, sizeof(gf.header));
        gf.header_crc = sy.header_crc;
        gf.header_known = true;
        gf.seq = ap.seq;
        commit_clean();
        PF_LOG("Transaction committed");
    }
}

/* Used by a mount that rewrote saves.dat: the same journal, with no PSRAM involved. */
bool gf_commit_meta(uint32_t seq) {
    uint32_t journal_crc = 0;
    uint32_t ops = 0;
    int fd;
    bool write_card = !gf.header_known || gf_crc32(0, gf.header, GCFS_HEADER_SIZE) != gf.header_crc;

    if (write_card) {
        memcpy(sy.header, gf.header, GCFS_HEADER_SIZE);
        if (!write_stage_card())
            return false;
    }

    fd = gf_journal_begin(seq, &journal_crc);
    if (fd < 0)
        return false;

    if ((write_card && !add_card_file(fd, &journal_crc, &ops)) || !add_saves_file(fd, &journal_crc, &ops)) {
        sd_close(fd);
        gf_path(path_a, GF_JOURNAL);
        sd_remove(path_a);
        return false;
    }

    if (!gf_journal_commit(fd, journal_crc, ops)) {
        gf_path(path_a, GF_JOURNAL);
        sd_remove(path_a);
        return false;
    }

    apply_start(ops, seq, false);
    return true;
}

void gf_header_committed(void) {
    gf.header_crc = gf_crc32(0, gf.header, GCFS_HEADER_SIZE);
    gf.header_known = true;
}

bool gf_sync_clean(void) {
    uint32_t gen;
    bool pending;

    if (gf.phase != GF_SY_IDLE)
        return false;
    pf_write_snapshot(&gen, &pending);
    return gen == gf.committed_gen && !pending;
}

void gf_sync_task(bool force) {
    uint64_t start_us = pf_time_us();
    uint32_t gen;
    bool pending;

    pf_write_snapshot(&gen, &pending);
    if (gen != gf.seen_gen) {
        gf.seen_gen = gen;
        gf.last_change_us = start_us;
        if (gf.state == GF_S_INVALID)
            gf.state = GF_S_READY;
    }

    switch (gf.phase) {
        case GF_SY_IDLE:
            if (gen == gf.committed_gen) {
                if (gf.state == GF_S_SAVE_FAILED || gf.state == GF_S_INVALID)
                    gf.state = GF_S_READY;
                return;
            }
            if (pending || (gf.state == GF_S_INVALID && !force))
                return;
            if (!force) {
                if (!pf_console_quiet() || start_us - gf.last_change_us < GF_QUIET_US)
                    return;
                if (gf.state == GF_S_SAVE_FAILED && start_us < gf.retry_at_us)
                    return;
            }
            scan_begin(gen);
            break;
        case GF_SY_STAGE:
            stage_slice(force, start_us);
            break;
        case GF_SY_COMMIT:
            commit_step();
            break;
        case GF_SY_APPLY:
            apply_slice(start_us);
            break;
    }
}
