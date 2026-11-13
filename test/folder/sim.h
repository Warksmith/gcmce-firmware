#pragma once

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- simulated SD card --- */

extern long sim_op_count;
extern long sim_crash_at;       /* cut power at this operation number */
extern long sim_fail_at;        /* fail this operation */
extern bool sim_fail_sticky;    /* ...and every one after it */
extern long sim_read_count;
extern long sim_read_fail_at;   /* fail this read */
extern long sim_read_flip_at;   /* this read returns a wrong bit */
extern bool sim_read_flip_first;/* ...in its first byte, not the middle one */
extern long sim_stage_writes;
extern long sim_stage_flip_at;  /* this write to a stage file stores a wrong bit */
extern long sim_free_bytes;     /* negative: unlimited */
extern void (*sim_op_hook)(long op);
extern jmp_buf sim_crash_point;

typedef void (*sim_walk_cb)(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx);

void sim_sd_reset(void);
void sim_sd_clear_faults(void);
void sim_sd_power_cycle(void);
int sim_sd_open_fds(void);
void *sim_sd_snapshot(void);
void sim_sd_restore(void *snapshot);
void sim_sd_snapshot_free(void *snapshot);
void sim_sd_walk(const char *base, sim_walk_cb cb, void *ctx);
void sim_sd_put(const char *path, const void *data, size_t len);
const uint8_t *sim_sd_get(const char *path, size_t *len);
void sim_sd_del(const char *path);
void sim_sd_mkdirs(const char *path);

/* --- simulated PSRAM and console ---
 *
 * The console side is written from the CARD library's behaviour and shares nothing with
 * gc_fs.c: it checks what the firmware builds, and it writes what a real console writes,
 * in the same order, as erases and 128-byte page writes. */

#define SIM_PSRAM_SIZE (8U * 1024U * 1024U)
#define SIM_BLOCK 8192U

extern uint8_t sim_psram[SIM_PSRAM_SIZE];
extern uint32_t sim_generation;
extern bool sim_hold_marks;     /* console writes stay undrained, as between two cores */
extern bool sim_console_busy;

void sim_console_reset(void);
void sim_time_advance_ms(uint32_t ms);
void sim_drain(void);

/* Validates the card with the rules Dolphin and the CARD library apply. Returns NULL, or
 * what is wrong with it. */
const char *con_mount(uint32_t mbits);
uint32_t con_free_blocks(void);
uint32_t con_saves(void);
bool con_entry(uint32_t slot, uint8_t entry[64]);
/* Reads a save's blocks in chain order. */
void con_read_data(uint32_t slot, uint8_t *data);
int con_find(const char *game, const char *maker, const char *name);

int con_create(const char *game, const char *maker, const char *name, uint32_t blocks); /* slot or -1 */
void con_write_save(uint32_t slot, uint32_t seed);
void con_write_block(uint32_t slot, uint32_t index, uint32_t seed); /* no directory update */
void con_touch(uint32_t slot, uint32_t mtime);
void con_rename(uint32_t slot, const char *name);
void con_delete(uint32_t slot);
void con_format(uint32_t serial);
/* Not something a console does: a second save claims the blocks of the first. */
void con_share_blocks(uint32_t slot, uint32_t other);

/* What every save holds, so that a save with blocks of two versions can be told. */
void sim_fill(uint8_t *block, uint32_t seed, uint32_t index);
/* Returns the seed if all of a save is one version, 0 if it is not. Seeds are not 0. */
uint32_t sim_seed_of(const uint8_t *data, uint32_t blocks);
