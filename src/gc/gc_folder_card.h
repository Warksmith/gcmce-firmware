#pragma once

/* A memory card backed by individual .gci files. The card lives in PSRAM while mounted and
 * the console uses it like any other; changes reach the SD card in the background. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    GC_FOLDER_LOAD_BUSY,
    GC_FOLDER_LOAD_DONE,
    GC_FOLDER_LOAD_FAILED /* the card stays hidden and nothing was changed */
} gc_folder_load_t;

/* Starts mounting path, which is created if it does not exist.
 * ini_mbits: CardSize from the card's ini, 0 if it has none.
 * default_mbits: the size of a new card without one.
 * encoding: the encoding of a new card. */
void gc_folder_card_open(const char *path, uint32_t ini_mbits, uint32_t default_mbits, uint8_t encoding);

/* Advances the mount by one bounded slice. */
gc_folder_load_t gc_folder_card_load_task(int *percent);

uint32_t gc_folder_card_size_bytes(void);
uint8_t gc_folder_card_encoding(void);

/* True from a completed or attempted mount until gc_folder_card_close(). */
bool gc_folder_card_is_mounted(void);

/* Background synchronization, one bounded slice per call. */
void gc_folder_card_task(void);

/* Drained from the dirty heap: this 512-byte segment of the card changed. */
void gc_folder_card_note_segment(uint32_t segment);

/* True if changes are in PSRAM and saving them is currently failing, so that switching
 * away would lose them. */
bool gc_folder_card_save_failing(void);

/* With the console stopped, saves everything. False if the card is not clean afterwards. */
bool gc_folder_card_flush(void);

/* Forgets the card. Refused, returning false, while PSRAM holds unsaved changes. */
bool gc_folder_card_close(void);

/* Short status for the display, NULL when there is nothing to report. */
const char *gc_folder_card_status_text(void);
