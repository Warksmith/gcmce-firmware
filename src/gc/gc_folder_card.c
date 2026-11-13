#include "gc_folder_card.h"

#include <stdio.h>
#include <string.h>

#include "gc_folder_internal.h"
#include "gc_folder_port.h"

gf_ctx_t gf;
gcfs_view_t gf_view;
uint8_t gf_owned[GCFS_MAX_BLOCKS / 8U];
uint8_t gf_page[GCFS_PAGE_SIZE];

void gf_path(char *out, const char *sub) {
    snprintf(out, GF_PATH_MAX, "%s/%s", gf.path, sub);
}

bool gf_time_left(uint64_t start) {
    return pf_time_us() - start < GF_SLICE_US;
}

void gc_folder_card_open(const char *path, uint32_t ini_mbits, uint32_t default_mbits, uint8_t encoding) {
    uint32_t gen;
    bool pending;

    gf_crc_init();
    gf_sync_reset();

    memset(&gf, 0, sizeof(gf));
    snprintf(gf.path, sizeof(gf.path), "%s", path);
    gf.ini_mbits = ini_mbits;
    gf.default_mbits = default_mbits;
    gf.encoding = encoding;
    gf.state = GF_S_LOADING;

    pf_write_snapshot(&gen, &pending);
    gf.committed_gen = gf.seen_gen = gen;

    PF_LOG("Backend selected: FOLDER, %s", path);
    gf_import_begin();
}

gc_folder_load_t gc_folder_card_load_task(int *percent) {
    gc_folder_load_t result = gf_import_step(percent);

    if (result == GC_FOLDER_LOAD_DONE) {
        gf.state = GF_S_READY;
        PF_LOG("Card ready");
    } else if (result == GC_FOLDER_LOAD_FAILED) {
        gf.state = GF_S_FAILED;
        gf_sync_abandon();
        PF_LOG("Card stays hidden, error %d", (int)gf.error);
    }
    return result;
}

uint32_t gc_folder_card_size_bytes(void) {
    return gf.mbits * 1024U * 1024U / 8U;
}

uint8_t gc_folder_card_encoding(void) {
    return gf.encoding;
}

bool gc_folder_card_is_mounted(void) {
    return gf.state != GF_S_CLOSED;
}

void gc_folder_card_task(void) {
    if (gf.state == GF_S_CLOSED || gf.state == GF_S_LOADING || gf.state == GF_S_FAILED)
        return;
    gf_sync_task(false);
}

void gc_folder_card_note_segment(uint32_t segment) {
    uint32_t block = segment / GCFS_PAGES_PER_BLOCK;

    if (block < GCFS_MAX_BLOCKS)
        gf.dirty_now[block / 8U] |= (uint8_t)(1U << (block % 8U));
}

bool gc_folder_card_save_failing(void) {
    return gf.state == GF_S_SAVE_FAILED || gf.state == GF_S_INVALID;
}

bool gc_folder_card_flush(void) {
    if (gf.state == GF_S_CLOSED || gf.state == GF_S_FAILED || gf.state == GF_S_LOADING)
        return true;

    if (gf.state == GF_S_SAVE_FAILED)
        gf.retry_at_us = 0;

    for (;;) {
        gf_sync_task(true);
        if (gf_sync_clean())
            return true;
        if (gf.state == GF_S_INVALID)
            return false;
        if (gf.state == GF_S_SAVE_FAILED)
            return false;
    }
}

bool gc_folder_card_close(void) {
    if (gf.state == GF_S_CLOSED)
        return true;
    if (gf.state != GF_S_FAILED && gf.state != GF_S_LOADING && !gf_sync_clean())
        return false;

    gf_import_abandon();
    gf_sync_abandon();
    gf.state = GF_S_CLOSED;
    return true;
}

const char *gc_folder_card_status_text(void) {
    switch (gf.state) {
        case GF_S_LOADING:
            return "Loading saves...";
        case GF_S_SYNCING:
            return "Saving card...";
        case GF_S_SAVE_FAILED:
            return "Save failed!";
        case GF_S_INVALID:
            return "Card data invalid";
        case GF_S_READY:
            return gf_sync_clean() ? NULL : "Unsaved changes";
        case GF_S_FAILED:
            switch (gf.error) {
                case GF_E_BAD_GCI: return "Bad GCI file";
                case GF_E_DUPLICATE: return "Duplicate save";
                case GF_E_TOO_BIG: return "Too big for card";
                case GF_E_TOO_MANY: return "Too many saves";
                case GF_E_NAME: return "Bad GCI name";
                case GF_E_HEADER: return "Bad card.dat";
                case GF_E_RECOVERY: return "Recovery required";
                case GF_E_CORRUPT: return "Card data invalid";
                default: return "SD error";
            }
        default:
            return NULL;
    }
}
