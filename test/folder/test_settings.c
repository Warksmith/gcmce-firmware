/* settings.c on the simulated SD card, with a block of memory as its flash. */

#include "../../src/settings.c"

#include <stdarg.h>
#include <stdlib.h>

#include "sim.h"

static const char path[] = "/.flippermce/settings.ini";
static uint8_t flash[sizeof(settings_t)];
static int failures;

#define CHECK(x) do { \
        if (!(x)) { \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #x); \
            failures++; \
        } \
    } while (0)

wear_leveling_status_t wear_leveling_init(void) {
    return WEAR_LEVELING_SUCCESS;
}

wear_leveling_status_t wear_leveling_read(uint32_t address, void *value, size_t length) {
    memcpy(value, flash + address, length);
    return WEAR_LEVELING_SUCCESS;
}

wear_leveling_status_t wear_leveling_write(uint32_t address, const void *value, size_t length) {
    memcpy(flash + address, value, length);
    return WEAR_LEVELING_SUCCESS;
}

void fatal(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    abort();
}

void sd_init(bool reinit) {
}

static void put(const char *text) {
    sim_sd_put(path, text, strlen(text));
}

static void boot(void) {
    memset(&settings, 0, sizeof(settings));
    memset(&serialized_settings, 0, sizeof(serialized_settings));
    settings_init();
}

static void fresh(void) {
    sim_sd_reset();
    sim_sd_mkdirs(".flippermce");
    memset(flash, 0, sizeof(flash));
}

static bool file_has(const char *text) {
    size_t len;
    const uint8_t *data = sim_sd_get(path, &len);
    char copy[512] = { 0 };

    if (!data || len >= sizeof(copy))
        return false;
    memcpy(copy, data, len);
    return strstr(copy, text) != NULL;
}

int main(void) {
    /* the file as a user wrote it */
    fresh();
    boot();
    CHECK(!settings_get_gc_folder_backend());
    put("[General]\nFlippedScreen=ON\nShowInfo=ON\n"
        "[GC]\nCardRestore=ON\nGameID=ON\nEncoding=JAP\nCardSize=64\n"
        "[Settings]\nStorageBackend=FOLDER\n");
    boot();
    CHECK(settings_get_gc_folder_backend());
    CHECK(settings_get_gc_cardsize() == 64);
    CHECK(settings_get_display_flipped());

    /* it survives the firmware rewriting the file */
    settings_set_gc_cardsize(16);
    CHECK(file_has("[Settings]\nStorageBackend=FOLDER\n"));
    boot();
    CHECK(settings_get_gc_folder_backend());
    CHECK(settings_get_gc_cardsize() == 16);

    /* taking the line out turns it off, whatever flash remembers */
    put("[GC]\nCardSize=16\n");
    boot();
    CHECK(!settings_get_gc_folder_backend());

    /* only an exact FOLDER in the right section turns it on */
    put("[Settings]\nStorageBackend=FOLDER\n");
    boot();
    CHECK(settings_get_gc_folder_backend());
    put("[Settings]\nStorageBackend=RAW\n");
    boot();
    CHECK(!settings_get_gc_folder_backend());
    put("[Settings]\nStorageBackend=folder\n");
    boot();
    CHECK(!settings_get_gc_folder_backend());
    put("[GC]\nStorageBackend=FOLDER\n");
    boot();
    CHECK(!settings_get_gc_folder_backend());

    /* a file written from scratch shows the setting */
    fresh();
    boot();
    CHECK(file_has("StorageBackend=RAW\n"));

    if (failures) {
        printf("settings: %d failed\n", failures);
        return 1;
    }
    printf("settings: all passed\n");
    return 0;
}
