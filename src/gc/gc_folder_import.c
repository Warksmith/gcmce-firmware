/* Builds a complete card in PSRAM from the .gci files in the folder. The console is kept
 * away by cardman until this finishes, so nothing here has to be visible in any order.
 *
 * Every save must fit: a failure leaves the card hidden and only .flippermeta changed. */

#include <stdio.h>
#include <string.h>

#include "gc_folder_internal.h"
#include "gc_folder_port.h"
#include "sd.h"

typedef enum {
    IM_START,
    IM_SCAN,
    IM_COPY,
    IM_ERASE,
    IM_TABLES,
    IM_META,
    IM_APPLY
} im_phase_t;

static struct {
    im_phase_t phase;
    int dir;
    int it;
    int report;
    uint32_t saves;
    uint32_t next_block;    /* next free block; the same as the blocks needed so far + 5 */
    uint32_t needed;        /* blocks all the saves in the folder need, failed or not */
    uint32_t seen;          /* .gci files, for the report */

    uint32_t slot;
    uint32_t page;          /* page of the save being copied */
    uint32_t pages;
    uint32_t crc;
    uint8_t entry[GCFS_ENTRY_SIZE];
    char name[GCI_HOST_NAME_MAX];
    bool have_old;

    uint32_t erase_block;
    uint32_t seq_old;
} im;

static void fail(gf_error_t error, const char *name, const char *why) {
    static char line[256 + 64];
    char path[GF_PATH_MAX];

    PF_LOG("%s: %s", name, why);
    if (gf.error == GF_E_NONE)
        gf.error = error;

    /* The display has room for one short line, so the details go where USB storage mode
     * can show them. Losing this report changes nothing about the failure itself. */
    if (im.report < 0) {
        gf_path(path, GF_ERROR_LOG);
        im.report = sd_open(path, O_RDWR | O_CREAT | O_TRUNC);
    }
    if (im.report >= 0) {
        int len = snprintf(line, sizeof(line), "%s: %s\r\n", name, why);

        sd_write(im.report, line, (size_t)len);
    }
}

static void close_report(void) {
    if (im.report >= 0) {
        sd_close(im.report);
        im.report = -1;
    }
}

static void close_dir(void) {
    if (im.it >= 0)
        sd_close(im.it);
    if (im.dir >= 0)
        sd_close(im.dir);
    im.it = im.dir = -1;
}

static void write_system_blocks(void) {
    /* header */
    memcpy(gf_page, gf.header, GCFS_HEADER_SIZE);
    pf_psram_write(0, gf_page, GCFS_PAGE_SIZE);

    memset(gf_page, 0xFF, GCFS_PAGE_SIZE);
    for (uint32_t page = 1; page < GCFS_PAGES_PER_BLOCK; page++)
        pf_psram_write(page * GCFS_PAGE_SIZE, gf_page, GCFS_PAGE_SIZE);

    /* directory copy 0, which collects the entries */
    for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++)
        pf_psram_write(gcfs_block_addr(GCFS_BLOCK_DIR) + page * GCFS_PAGE_SIZE, gf_page, GCFS_PAGE_SIZE);
}

/* What the last mount knew, only to keep saves in the directory slots they had. Everything
 * else comes from the files themselves. */
static void load_old_records(void) {
    static gf_record_t rec;
    uint32_t count = 0;
    int fd;

    memset(gf_next, 0, sizeof(gf_save_t) * GCFS_DIR_ENTRIES);
    im.have_old = false;
    im.seq_old = 0;

    fd = gf_saves_open_read(&im.seq_old, &count);
    if (fd < 0)
        return;

    for (uint32_t i = 0; i < count; i++) {
        if (!gf_saves_read_record(fd, i, &rec))
            break;

        gf_save_t *old = &gf_next[rec.slot];

        gci_identity(rec.entry, &old->id);
        old->entry_crc = gci_entry_crc(rec.entry);
        old->data_crc = rec.data_crc;
        old->name_hash = gci_name_hash(rec.name);
        old->blocks = gcfs_get16(&rec.entry[GCFS_ENT_BLOCK_COUNT]);
        old->rec = (uint8_t)i;
        old->used = true;
    }
    sd_close(fd);
    im.have_old = true;
}

/* The slot a save had before if it is still free, or the first slot no known save will
 * ask for, or failing that any free one. */
static uint32_t choose_slot(const gci_id_t *id) {
    uint32_t any = GF_NO_SLOT;

    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        if (gf_next[slot].used && gci_id_equal(&gf_next[slot].id, id) && !gf.base[slot].used)
            return slot;
    }
    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        if (gf.base[slot].used)
            continue;
        if (!gf_next[slot].used)
            return slot;
        if (any == GF_NO_SLOT)
            any = slot;
    }
    return any;
}

static void start(void) {
    char path[GF_PATH_MAX];
    gf_error_t recovered;

    sd_mkdir(gf.path);
    gf_path(path, GF_META_DIR);
    sd_mkdir(path);
    gf_path(path, GF_STAGE_DIR);
    sd_mkdir(path);

    gf_path(path, GF_META_DIR);
    if (!sd_exists(path)) {
        fail(GF_E_SD, gf.path, "cannot create the metadata directory");
        return;
    }

    /* An interrupted save is finished before anything reads the folder. */
    recovered = gf_recover();
    if (recovered != GF_E_NONE) {
        fail(recovered, "journal.dat", "cannot finish an interrupted save");
        return;
    }

    switch (gf_card_read(gf.header)) {
        case GF_CARD_OK:
            gf.header_known = true;
            gf.header_crc = gf_crc32(0, gf.header, GCFS_HEADER_SIZE);
            gf.mbits = gf.ini_mbits ? gf.ini_mbits : gcfs_header_mbits(gf.header);
            if (gcfs_header_mbits(gf.header) != gf.mbits)
                gcfs_header_set_mbits(gf.header, gf.mbits);
            break;
        case GF_CARD_MISSING:
            gf.header_known = false;
            gf.mbits = gf.ini_mbits ? gf.ini_mbits : gf.default_mbits;
            gcfs_header_init(gf.header, gf.mbits, gf.encoding);
            break;
        case GF_CARD_DAMAGED:
        default:
            /* A new header would be a new card identity. That is for the user to decide. */
            fail(GF_E_HEADER, "card.dat", "damaged; delete it to give this card a new identity");
            return;
    }

    if (!gcfs_size_valid(gf.mbits)) {
        fail(GF_E_HEADER, "card.dat", "unsupported card size");
        return;
    }

    gf.encoding = gf.header[GCFS_HDR_ENCODING + 1U];
    gf.blocks = gcfs_blocks_for_size(gf.mbits);

    load_old_records();
    memset(gf.base, 0, sizeof(gf.base));
    memset(gf_view.bat, 0, sizeof(gf_view.bat));
    write_system_blocks();

    im.dir = sd_open(gf.path, O_RDONLY);
    if (im.dir < 0) {
        fail(GF_E_SD, gf.path, "cannot open the folder");
        return;
    }
    im.phase = IM_SCAN;
}

/* Looks at the next directory entry. Returns false at the end of the folder. */
static bool scan_next(void) {
    static char long_name[256];
    gci_id_t id;
    int size;

    im.it = sd_iterate_dir(im.dir, im.it);
    if (im.it < 0)
        return false;

    if (sd_is_dir(im.it) || !sd_get_name(im.it, long_name, sizeof(long_name)) || !gci_is_save_name(long_name))
        return true;

    im.seen++;

    /* Every save must stay reachable by name, and these could be read but never replaced. */
    if (strlen(long_name) >= sizeof(im.name)) {
        fail(GF_E_NAME, long_name, "file name too long");
        return true;
    }
    if (strchr(long_name, '?')) {
        fail(GF_E_NAME, long_name, "file name is not plain ASCII");
        return true;
    }
    strcpy(im.name, long_name);

    size = sd_filesize(im.it);

    if (sd_read(im.it, im.entry, GCFS_ENTRY_SIZE) != (int)GCFS_ENTRY_SIZE) {
        fail(GF_E_BAD_GCI, im.name, "too short to be a GCI file");
        return true;
    }

    switch (gci_check(im.entry, (uint32_t)size)) {
        case GCI_BAD_ENTRY:
            fail(GF_E_BAD_GCI, im.name, "invalid directory entry");
            return true;
        case GCI_BAD_SIZE:
            fail(GF_E_BAD_GCI, im.name, "file size does not match its block count");
            return true;
        default:
            break;
    }

    uint32_t blocks = gcfs_get16(&im.entry[GCFS_ENT_BLOCK_COUNT]);

    im.needed += blocks;

    gci_identity(im.entry, &id);
    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        if (gf.base[slot].used && gci_id_equal(&gf.base[slot].id, &id)) {
            fail(GF_E_DUPLICATE, im.name, "same save as another file in this folder");
            return true;
        }
    }

    if (im.saves >= GCFS_DIR_ENTRIES) {
        im.saves++;
        fail(GF_E_TOO_MANY, im.name, "more than 127 saves");
        return true;
    }
    if (GCFS_SYS_BLOCKS + im.needed > gf.blocks) {
        fail(GF_E_TOO_BIG, im.name, "does not fit on this card");
        return true;
    }
    /* After a failure the rest of the folder is still looked at for the report. */
    if (gf.error != GF_E_NONE)
        return true;

    im.slot = choose_slot(&id);
    im.page = 0;
    im.pages = blocks * GCFS_PAGES_PER_BLOCK;
    im.crc = 0;
    im.phase = IM_COPY;
    return true;
}

static void finish_save(void) {
    uint32_t blocks = im.pages / GCFS_PAGES_PER_BLOCK;
    uint32_t first = im.next_block;
    gf_save_t *save = &gf.base[im.slot];

    for (uint32_t block = first; block < first + blocks; block++)
        gf_view.bat[block - GCFS_SYS_BLOCKS] = block + 1U == first + blocks ? GCFS_BAT_END : (uint16_t)(block + 1U);

    gcfs_put16(&im.entry[GCFS_ENT_FIRST_BLOCK], (uint16_t)first);
    pf_psram_write(gcfs_block_addr(GCFS_BLOCK_DIR) + im.slot * GCFS_ENTRY_SIZE, im.entry, GCFS_ENTRY_SIZE);

    gci_identity(im.entry, &save->id);
    save->entry_crc = gci_entry_crc(im.entry);
    save->data_crc = im.crc;
    save->name_hash = gci_name_hash(im.name);
    save->blocks = (uint16_t)blocks;
    save->used = true;

    im.next_block += blocks;
    im.saves++;
    im.phase = IM_SCAN;
}

static void copy_slice(uint64_t start_us) {
    uint32_t addr = gcfs_block_addr(im.next_block) + im.page * GCFS_PAGE_SIZE;

    while (im.page < im.pages) {
        if (sd_read(im.it, gf_page, GCFS_PAGE_SIZE) != (int)GCFS_PAGE_SIZE) {
            fail(GF_E_SD, im.name, "read error");
            im.phase = IM_SCAN;
            return;
        }
        im.crc = gf_crc32(im.crc, gf_page, GCFS_PAGE_SIZE);
        pf_psram_write(addr, gf_page, GCFS_PAGE_SIZE);
        addr += GCFS_PAGE_SIZE;
        im.page++;
        if (im.page < im.pages && !gf_time_left(start_us))
            return;
    }
    finish_save();
}

/* PSRAM still holds the previous card, and a real card's free blocks read as erased. */
static void erase_slice(uint64_t start_us) {
    memset(gf_page, 0xFF, GCFS_PAGE_SIZE);
    while (im.erase_block < gf.blocks) {
        for (uint32_t page = 0; page < GCFS_PAGES_PER_BLOCK; page++)
            pf_psram_write(gcfs_block_addr(im.erase_block) + page * GCFS_PAGE_SIZE, gf_page, GCFS_PAGE_SIZE);
        im.erase_block++;
        if (!gf_time_left(start_us))
            return;
    }
    im.phase = IM_TABLES;
}

static bool base_matches_old(void) {
    if (!im.have_old)
        return false;
    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++) {
        const gf_save_t *now = &gf.base[slot];
        const gf_save_t *old = &gf_next[slot];

        if (now->used != old->used)
            return false;
        if (now->used && (!gci_id_equal(&now->id, &old->id) || now->entry_crc != old->entry_crc
                || now->data_crc != old->data_crc || now->name_hash != old->name_hash))
            return false;
    }
    return true;
}

/* saves.dat does not describe the folder, so it is rewritten through the journal, along
 * with card.dat if that is new. This walks the folder again for the host names, which were
 * not worth keeping in SRAM for a case that only follows a change on a computer. */
static gf_error_t write_records(void) {
    static char path[GF_PATH_MAX];
    static gf_record_t rec;
    uint32_t crc;
    uint32_t count = 0;
    int dir;
    int it = -1;
    int fd;

    snprintf(path, sizeof(path), "%s/%s/%u.bin", gf.path, GF_STAGE_DIR, GF_STAGE_SAVES);
    im.seq_old++;
    fd = gf_saves_open_write(path, im.seq_old, &crc);
    if (fd < 0)
        return GF_E_SD;

    dir = sd_open(gf.path, O_RDONLY);
    if (dir < 0) {
        sd_close(fd);
        return GF_E_SD;
    }

    while ((it = sd_iterate_dir(dir, it)) >= 0) {
        gci_id_t id;

        if (sd_is_dir(it) || !sd_get_name(it, rec.name, sizeof(rec.name)) || !gci_is_save_name(rec.name))
            continue;
        if (sd_read(it, rec.entry, GCFS_ENTRY_SIZE) != (int)GCFS_ENTRY_SIZE)
            goto fail;

        gci_identity(rec.entry, &id);

        uint32_t slot = 0;

        while (slot < GCFS_DIR_ENTRIES && !(gf.base[slot].used && gci_id_equal(&gf.base[slot].id, &id)))
            slot++;
        if (slot == GCFS_DIR_ENTRIES)
            goto fail;

        rec.slot = slot;
        rec.data_crc = gf.base[slot].data_crc;
        gf.base[slot].rec = (uint8_t)count;
        if (!gf_saves_write_record(fd, &rec, &crc))
            goto fail;
        count++;
    }
    sd_close(dir);

    if (!gf_saves_finish(fd, count, crc))
        return GF_E_SD;

    return gf_commit_meta(im.seq_old) ? GF_E_NONE : GF_E_SD;

fail:
    if (it >= 0)
        sd_close(it);
    sd_close(dir);
    sd_close(fd);
    return GF_E_SD;
}

static void remove_stale_report(void) {
    char path[GF_PATH_MAX];

    gf_path(path, GF_ERROR_LOG);
    if (sd_exists(path))
        sd_remove(path);
}

static void finish_tables(void) {
    gcfs_commit_tables(gf.blocks, gf_view.bat, (uint16_t)(im.next_block - 1U), gf_page);

    /* Published only if the normal reader agrees with what was built. */
    gcfs_result_t result = gcfs_view_load(&gf_view, gf.blocks, gf_page);

    if (result == GCFS_OK)
        result = gcfs_view_validate(&gf_view, gf_owned);
    if (result != GCFS_OK) {
        fail(GF_E_CORRUPT, "card", "the built card failed validation");
        return;
    }
    PF_LOG("Virtual card validation complete, %lu saves", (unsigned long)im.saves);
    im.phase = IM_META;
}

void gf_import_abandon(void) {
    close_dir();
    close_report();
}

void gf_import_begin(void) {
    memset(&im, 0, sizeof(im));
    im.dir = im.it = im.report = -1;
    im.next_block = GCFS_SYS_BLOCKS;
}

gc_folder_load_t gf_import_step(int *percent) {
    uint64_t start_us = pf_time_us();

    do {
        switch (im.phase) {
            case IM_START:
                start();
                break;
            case IM_SCAN:
                if (!scan_next()) {
                    close_dir();
                    if (GCFS_SYS_BLOCKS + im.needed > gf.blocks)
                        PF_LOG("Card capacity exceeded: %lu blocks needed, %lu available",
                               (unsigned long)im.needed, (unsigned long)(gf.blocks - GCFS_SYS_BLOCKS));
                    im.erase_block = im.next_block;
                    im.phase = IM_ERASE;
                }
                break;
            case IM_COPY:
                copy_slice(start_us);
                break;
            case IM_ERASE:
                if (gf.error == GF_E_NONE)
                    erase_slice(start_us);
                break;
            case IM_TABLES:
                finish_tables();
                break;
            case IM_META: {
                uint32_t crc_now = gf_crc32(0, gf.header, GCFS_HEADER_SIZE);

                if (base_matches_old() && gf.header_known && crc_now == gf.header_crc) {
                    for (uint32_t slot = 0; slot < GCFS_DIR_ENTRIES; slot++)
                        gf.base[slot].rec = gf_next[slot].rec;
                    gf.seq = im.seq_old;
                    remove_stale_report();
                    *percent = 100;
                    return GC_FOLDER_LOAD_DONE;
                }
                gf_error_t result = write_records();

                if (result != GF_E_NONE) {
                    fail(result, "saves.dat", "cannot update the card metadata");
                    break;
                }
                gf.seq = im.seq_old;
                im.phase = IM_APPLY;
                break;
            }
            case IM_APPLY:
                if (gf_apply_step(start_us) == GF_SY_APPLY) {
                    if (gf.state == GF_S_SAVE_FAILED) {
                        gf.state = GF_S_LOADING;
                        fail(GF_E_SD, "saves.dat", "cannot update the card metadata");
                    }
                    break;
                }
                gf_header_committed();
                remove_stale_report();
                *percent = 100;
                return GC_FOLDER_LOAD_DONE;
        }

        if (gf.error != GF_E_NONE && (im.phase != IM_SCAN)) {
            close_dir();
            close_report();
            return GC_FOLDER_LOAD_FAILED;
        }
    } while (gf_time_left(start_us));

    *percent = gf.blocks ? (int)(100U * (im.next_block + (im.pages ? im.page / GCFS_PAGES_PER_BLOCK : 0)) / gf.blocks) : 0;
    if (*percent > 99)
        *percent = 99;
    return GC_FOLDER_LOAD_BUSY;
}
