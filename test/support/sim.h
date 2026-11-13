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
