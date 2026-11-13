#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gc_folder_card.h"
#include "gc_folder_meta.h"
#include "gc_fs.h"
#include "gc_gci.h"

#define GF_PATH_MAX       256U
#define GF_NO_SLOT        0xFFU
#define GF_SLICE_US       20000U
#define GF_QUIET_US       1500000U /* console silence before a sync starts */
#define GF_RETRY_US       5000000U /* after a failed sync */

typedef enum {
    GF_S_CLOSED,
    GF_S_LOADING,
    GF_S_FAILED,       /* hidden: see error */
    GF_S_READY,
    GF_S_SYNCING,
    GF_S_SAVE_FAILED,  /* SD is behind PSRAM and retries are running */
    GF_S_INVALID       /* PSRAM does not hold a filesystem that can be saved */
} gf_state_t;

typedef enum {
    GF_E_NONE,
    GF_E_BAD_GCI,
    GF_E_DUPLICATE,
    GF_E_TOO_BIG,
    GF_E_TOO_MANY,
    GF_E_NAME,
    GF_E_SD,
    GF_E_HEADER,
    GF_E_RECOVERY,
    GF_E_CORRUPT
} gf_error_t;

typedef enum {
    GF_SY_IDLE,
    GF_SY_STAGE,
    GF_SY_COMMIT,
    GF_SY_APPLY
} gf_sync_phase_t;

typedef struct {
    gf_state_t state;
    gf_error_t error;

    char path[GF_PATH_MAX];
    uint32_t mbits;
    uint32_t blocks;
    uint8_t encoding;
    uint32_t default_mbits;
    uint32_t ini_mbits;

    uint8_t header[GCFS_HEADER_SIZE];
    uint32_t header_crc;       /* of what card.dat holds */
    bool header_known;         /* card.dat exists and matches header_crc */

    /* The folder as last committed, by directory slot. */
    gf_save_t base[GCFS_DIR_ENTRIES];
    uint32_t seq;              /* journal sequence, so that trash directories stay unique */

    /* Change tracking. A block stays in dirty_scan until a commit includes it. */
    uint32_t committed_gen;
    uint32_t seen_gen;
    uint64_t last_change_us;
    uint64_t retry_at_us;
    uint8_t dirty_now[GCFS_MAX_BLOCKS / 8U];
    uint8_t dirty_scan[GCFS_MAX_BLOCKS / 8U];

    gf_sync_phase_t phase;
} gf_ctx_t;

extern gf_ctx_t gf;
extern gcfs_view_t gf_view;
extern uint8_t gf_owned[GCFS_MAX_BLOCKS / 8U];
extern uint8_t gf_page[GCFS_PAGE_SIZE];

void gf_path(char *out, const char *sub);
bool gf_time_left(uint64_t start);

/* gc_folder_import.c */
void gf_import_begin(void);
void gf_import_abandon(void);
gc_folder_load_t gf_import_step(int *percent);

/* gc_folder_sync.c */
extern gf_save_t gf_next[GCFS_DIR_ENTRIES]; /* the state a pass or a mount works towards */

void gf_sync_reset(void);
void gf_sync_abandon(void);
/* force: the console has stopped and no quiet time is needed. */
void gf_sync_task(bool force);
bool gf_sync_clean(void);
/* Finishes or discards an interrupted transaction. */
gf_error_t gf_recover(void);
/* Commits stage/200.bin as saves.dat, and the mount's header if it is new. */
bool gf_commit_meta(uint32_t seq);
gf_sync_phase_t gf_apply_step(uint64_t start_us);
void gf_header_committed(void);
