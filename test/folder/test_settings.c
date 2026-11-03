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

/* Startup selection only needs these database hooks, not the embedded database. */
void game_db_extract_game_id(const char *id, char *out) { out[0] = '\0'; }
void game_db_update_game(const char *id) { }
void game_db_get_current_id(const char **id, const char **region) {
    *id = NULL;
    *region = NULL;
}

static void expect_startup(const char *folder, int channel, gc_cardman_state_t state, int index) {
    gc_cardman_init();
    CHECK(strcmp(gc_cardman_get_folder_name(), folder) == 0);
    CHECK(gc_cardman_get_channel() == channel);
    CHECK(gc_cardman_get_state() == state);
    CHECK(gc_cardman_get_idx() == index);
    CHECK(sim_sd_open_fds() == 0);
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
    CHECK(file_has("StartupCard=\n"));

    /* Restore wins; otherwise startup selects channel 1 independently of GameID. */
    put("[GC]\nCardRestore=OFF\nGameID=OFF\nStartupCard=Card12\n");
    boot();
    expect_startup("Card12", 1, GC_CM_STATE_NORMAL, 12);
    gc_cardman_next_idx();
    CHECK(strcmp(gc_cardman_get_folder_name(), "Card13") == 0);
    settings_set_gc_last_card(GC_CM_STATE_NORMAL, 7, 3, "Card7");
    settings_set_gc_card_restore(true);
    boot();
    expect_startup("Card7", 3, GC_CM_STATE_NORMAL, 7);
    settings_set_gc_card_restore(false);
    boot();
    expect_startup("Card12", 1, GC_CM_STATE_NORMAL, 12);

    /* Named and not-yet-created cards, using the real directory iterator. */
    sim_sd_mkdirs("MemoryCards/GC/Shared");
    put("[GC]\nCardRestore=OFF\nStartupCard=Shared\n");
    boot();
    expect_startup("Shared", 1, GC_CM_STATE_NAMED, 1);
    settings_set_gc_cardsize(8);
    CHECK(file_has("StartupCard=Shared\n"));
    boot();
    expect_startup("Shared", 1, GC_CM_STATE_NAMED, 1);
    put("[GC]\nCardRestore=OFF\nStartupCard=NewCard\n");
    boot();
    expect_startup("NewCard", 1, GC_CM_STATE_NAMED, 0);
    settings_set_gc_last_card(GC_CM_STATE_NAMED, 1, 2, "Shared");
    settings_set_gc_card_restore(true);
    boot();
    expect_startup("Shared", 2, GC_CM_STATE_NAMED, 1);

    const char *invalid[] = { "", ".", "..", "../Shared", "dir/card", "dir\\card", "C:card",
        "Shared*", "Shared?", "Shared.", "12345678901234567890123456789012" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char text[128];
        snprintf(text, sizeof(text), "[GC]\nCardRestore=OFF\nStartupCard=%s\n", invalid[i]);
        put(text);
        boot();
        CHECK(settings_get_gc_startup_card()[0] == '\0');
        expect_startup("Card1", 1, GC_CM_STATE_NORMAL, 1);
    }
    put("[GC]\nCardRestore=OFF\nStartupCard=1234567890123456789012345678901\n");
    boot();
    expect_startup("1234567890123456789012345678901", 1, GC_CM_STATE_NAMED, 0);
    put("[GC]\nCardRestore=OFF\n[Other]\nStartupCard=Shared\n");
    boot();
    expect_startup("Card1", 1, GC_CM_STATE_NORMAL, 1);
    /* Missing SD settings must not retain a startup card from a previous load. */
    put("[GC]\nCardRestore=OFF\nStartupCard=Shared\n");
    boot();
    sd_remove(path);
    boot();
    CHECK(settings_get_gc_startup_card()[0] == '\0');
    expect_startup("Card1", 1, GC_CM_STATE_NORMAL, 1);

    /* Rewriting a shorter file must remove old trailing settings. */
    char padded[512];
    memset(padded, ';', 400);
    strcpy(padded + 400, "\n[GC]\nCardRestore=ON\nStartupCard=Shared\n");
    put(padded);
    boot();
    settings_set_gc_card_restore(false);
    boot();
    CHECK(!settings_get_gc_card_restore());
    expect_startup("Shared", 1, GC_CM_STATE_NAMED, 1);

    put("[GC]\nCardRestore=OFF\nStartupCard=card65535\n");
    boot();
    expect_startup("card65535", 1, GC_CM_STATE_NORMAL, 65535);
    put("[GC]\nCardRestore=OFF\nStartupCard=DL-DOL-GT4P-EUR\n");
    boot();
    expect_startup("DL-DOL-GT4P-EUR", 1, GC_CM_STATE_NAMED, 0);

    if (failures) {
        printf("settings: %d failed\n", failures);
        return 1;
    }
    printf("settings: all passed\n");
    return 0;
}
