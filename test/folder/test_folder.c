/* The folder card engine against a simulated SD card and console. The oracles are the
 * console model, which shares no code with the engine, and the .gci files on the card. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_config.h"
#include "gc_folder_internal.h"
#include "sd.h"
#include "sim.h"

#define CARD "MemoryCards/GC/Card1/Card1-1.folder"
#define META CARD "/.flippermeta"
#define MAX_SAVE_BLOCKS 1019U

static int checks;
static const char *current;
static uint8_t big_a[MAX_SAVE_BLOCKS * SIM_BLOCK];

#define CHECK(c) do { \
    checks++; \
    if (!(c)) { \
        printf("FAIL %s:%d %s: %s\n", __FILE__, __LINE__, current, #c); \
        exit(1); \
    } \
} while (0)

/* --- helpers --- */

static void fresh(void) {
    sim_sd_reset();
    sim_console_reset();
    gf_crc_init();
    sim_sd_mkdirs("MemoryCards/GC/Card1");
}

static void put_be16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* A .gci as Dolphin writes it. first_block is whatever the card it came from had. */
static void make_gci(const char *host, const char *game, const char *maker, const char *name, uint32_t blocks, uint32_t seed) {
    size_t size = 64U + (size_t)blocks * SIM_BLOCK;
    uint8_t *file = calloc(1, size);
    char path[300];

    memcpy(file, game, 4);
    memcpy(&file[4], maker, 2);
    file[6] = 0xFF;
    strncpy((char *)&file[8], name, 32);
    file[0x28] = 0x34;
    file[0x34] = 4;
    put_be16(&file[0x36], 0x1234);
    put_be16(&file[0x38], blocks);
    file[0x3A] = file[0x3B] = 0xFF;
    for (uint32_t i = 0; i < blocks; i++)
        sim_fill(&file[64U + i * SIM_BLOCK], seed, i);

    snprintf(path, sizeof(path), "%s/%s", CARD, host);
    sim_sd_mkdirs(CARD);
    sim_sd_put(path, file, size);
    free(file);
}

static gc_folder_load_t mount_size(uint32_t ini_mbits, uint32_t default_mbits) {
    gc_folder_load_t result;
    int percent = 0;

    gc_folder_card_open(CARD, ini_mbits, default_mbits, 0);
    do {
        result = gc_folder_card_load_task(&percent);
    } while (result == GC_FOLDER_LOAD_BUSY);
    return result;
}

static gc_folder_load_t mount(void) {
    return mount_size(0, 4);
}

static void console_up(uint32_t mbits) {
    const char *why = con_mount(mbits);

    if (why)
        printf("console: %s\n", why);
    CHECK(why == NULL);
}

/* Lets the background task run until the card has nothing left to do. */
static void settle(void) {
    sim_drain();
    sim_time_advance_ms(1600);
    for (long i = 0; i < 10000000L; i++) {
        gc_folder_card_task();
        sim_time_advance_ms(1);
        if (gf_sync_clean() && gf.state == GF_S_READY)
            return;
        if (gf.state == GF_S_SAVE_FAILED)
            sim_time_advance_ms(1000);
        if (gf.state == GF_S_INVALID)
            return;
    }
    CHECK(!"the card never became clean");
}

typedef struct {
    uint32_t digest;
    uint32_t files;
} folder_sum_t;

static void sum_cb(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx) {
    folder_sum_t *sum = ctx;

    if (is_dir || strchr(path, '/'))
        return;
    sum->digest ^= gf_crc32(gf_crc32(0, path, strlen(path)), data, len) * 2654435761U + 1U;
    sum->files++;
}

/* Everything in the folder a computer would call a save. */
static folder_sum_t folder_sum(void) {
    folder_sum_t sum = {0, 0};

    sim_sd_walk(CARD, sum_cb, &sum);
    return sum;
}

static bool same(folder_sum_t a, folder_sum_t b) {
    return a.digest == b.digest && a.files == b.files;
}

typedef struct {
    int files;
    size_t bytes;
} count_t;

static void count_cb(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx) {
    count_t *c = ctx;

    (void)path; (void)data;
    if (!is_dir) {
        c->files++;
        c->bytes += len;
    }
}

static count_t count_in(const char *dir) {
    count_t c = {0, 0};

    sim_sd_walk(dir, count_cb, &c);
    return c;
}

static void no_transaction_left(void) {
    CHECK(!sd_exists(META "/journal.dat"));
    CHECK(count_in(META "/stage").files == 0);
}

static const uint8_t *find_gci(const uint8_t *entry, size_t *len) {
    /* the entry says which file: walk them all, since the name is the engine's business */
    static const uint8_t *found;
    static size_t found_len;
    static const uint8_t *wanted;
    void cb(const char *path, bool is_dir, const uint8_t *data, size_t l, void *ctx) {
        (void)ctx;
        if (is_dir || strchr(path, '/') || l < 64)
            return;
        if (memcmp(data, wanted, 6) == 0 && strncmp((const char *)&data[8], (const char *)&wanted[8], 32) == 0) {
            found = data;
            found_len = l;
        }
    }

    found = NULL;
    wanted = entry;
    sim_sd_walk(CARD, cb, NULL);
    *len = found_len;
    return found;
}

/* The folder holds exactly what the console sees, entry and data. */
static void folder_matches_console(void) {
    uint8_t entry[64];
    uint32_t saves = 0;

    for (uint32_t slot = 0; slot < 127; slot++) {
        if (!con_entry(slot, entry))
            continue;

        size_t len = 0;
        const uint8_t *file = find_gci(entry, &len);
        uint32_t blocks = (uint32_t)entry[0x38] << 8 | entry[0x39];

        CHECK(file != NULL);
        CHECK(len == 64U + blocks * SIM_BLOCK);

        uint8_t a[64], b[64];

        memcpy(a, file, 64);
        memcpy(b, entry, 64);
        a[0x36] = a[0x37] = b[0x36] = b[0x37] = 0;
        CHECK(memcmp(a, b, 64) == 0);

        con_read_data(slot, big_a);
        CHECK(memcmp(&file[64], big_a, blocks * SIM_BLOCK) == 0);
        saves++;
    }
    CHECK(folder_sum().files == saves);

    /* and the firmware's own files are good */
    size_t len = 0;
    const uint8_t *card = sim_sd_get(META "/card.dat", &len);
    uint32_t seq, count;
    int fd = gf_saves_open_read(&seq, &count);

    CHECK(card != NULL && len == 512 && memcmp(card, sim_psram, 512) == 0);
    CHECK(fd >= 0 && count == saves);
    sd_close(fd);
}

static void flip_file(const char *path, size_t offset) {
    size_t len = 0;
    const uint8_t *data = sim_sd_get(path, &len);
    uint8_t *copy = malloc(len);

    CHECK(data != NULL && offset < len);
    memcpy(copy, data, len);
    copy[offset] ^= 1;
    sim_sd_put(path, copy, len);
    free(copy);
}

/* Unmount, power cycle, mount: the card that comes back is the one that went. */
static void remount_matches(uint32_t mbits) {
    folder_sum_t before = folder_sum();

    CHECK(gc_folder_card_close());
    sim_sd_power_cycle();
    memset(sim_psram, 0x55, sizeof(sim_psram));
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(mbits);
    folder_matches_console();
    CHECK(same(before, folder_sum()));
    no_transaction_left();
}

/* --- import --- */

static void test_empty_card_all_sizes(void) {
    static const uint32_t sizes[] = {4, 8, 16, 32, 64};
    static const uint32_t usable[] = {59, 123, 251, 507, 1019};

    for (int i = 0; i < 5; i++) {
        fresh();
        CHECK(mount_size(sizes[i], 4) == GC_FOLDER_LOAD_DONE);
        console_up(sizes[i]);
        CHECK(con_free_blocks() == usable[i]);
        CHECK(con_saves() == 0);
        CHECK(gc_folder_card_size_bytes() == sizes[i] * 1024U * 1024U / 8U);
        CHECK(folder_sum().files == 0);

        size_t len = 0;

        CHECK(sim_sd_get(META "/card.dat", &len) && len == 512);
        no_transaction_left();
        CHECK(gc_folder_card_close());
    }
}

static void test_no_full_image(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "save", 3, 7);
    CHECK(mount_size(64, 4) == GC_FOLDER_LOAD_DONE);
    console_up(64);

    /* metadata stays small whatever the card size, and nothing is a card image */
    CHECK(count_in(META).bytes < 8192);
    CHECK(gc_folder_card_close());
}

static void test_import_saves(void) {
    fresh();
    make_gci("01-GAFE-one.gci", "GAFE", "01", "one", 2, 11);
    make_gci("my own name.GCI", "GZLE", "01", "zelda", 3, 12);
    make_gci("03.gci", "GFZE", "8P", "fzero", 1, 13);

    folder_sum_t before = folder_sum();

    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    CHECK(con_saves() == 3);
    CHECK(con_free_blocks() == 59 - 6);
    CHECK(con_find("GZLE", "01", "zelda") >= 0);
    folder_matches_console();
    CHECK(same(before, folder_sum()));
    CHECK(gc_folder_card_close());
}

/* The console's serial comes from the first 32 bytes of the header. New cards sharing it is
 * what lets a protected save move between them, and a size must not change it. */
static void test_new_cards_share_a_serial(void) {
    static const uint32_t sizes[] = {4, 8, 16, 32, 64};
    uint8_t first[32];

    for (int i = 0; i < 5; i++) {
        fresh();
        CHECK(mount_size(sizes[i], 4) == GC_FOLDER_LOAD_DONE);
        if (i == 0)
            memcpy(first, sim_psram, sizeof(first));
        CHECK(memcmp(first, sim_psram, sizeof(first)) == 0);
        CHECK(gc_folder_card_close());
    }
}

static void test_second_mount_writes_nothing(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 2, 11);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());

    sim_sd_clear_faults();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(sim_op_count == 0);
    CHECK(gc_folder_card_close());
}

static void test_slots_are_stable(void) {
    int a, b, c;

    fresh();
    make_gci("a.gci", "GAAA", "01", "a", 1, 1);
    make_gci("b.gci", "GBBB", "01", "b", 1, 2);
    make_gci("c.gci", "GCCC", "01", "c", 1, 3);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    a = con_find("GAAA", "01", "a");
    b = con_find("GBBB", "01", "b");
    c = con_find("GCCC", "01", "c");
    CHECK(gc_folder_card_close());

    sim_sd_del(CARD "/b.gci");
    make_gci("z.gci", "GZZZ", "01", "z", 1, 4);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    CHECK(con_find("GAAA", "01", "a") == a);
    CHECK(con_find("GCCC", "01", "c") == c);
    CHECK(con_find("GZZZ", "01", "z") >= 0 && b >= 0);
    CHECK(gc_folder_card_close());
}

static void test_card_exactly_full(void) {
    fresh();
    make_gci("big.gci", "GAFE", "01", "big", 59, 5);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    CHECK(con_free_blocks() == 0);
    folder_matches_console();
    CHECK(gc_folder_card_close());

    fresh();
    for (uint32_t i = 0; i < 127; i++) {
        char host[16], name[16], game[5];

        snprintf(host, sizeof(host), "s%u.gci", i);
        snprintf(name, sizeof(name), "s%u", i);
        snprintf(game, sizeof(game), "G%03u", i);
        make_gci(host, game, "01", name, 1, i + 1U);
    }
    CHECK(mount_size(16, 4) == GC_FOLDER_LOAD_DONE);
    console_up(16);
    CHECK(con_saves() == 127);
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

static void test_import_refuses(void) {
    struct {
        const char *what;
        gf_error_t error;
        void (*setup)(void);
    } cases[8];
    int n = 0;

    void too_short(void) { sim_sd_put(CARD "/x.gci", "abc", 3); }
    void bad_size(void) {
        make_gci("x.gci", "GAFE", "01", "x", 2, 1);
        size_t len;
        const uint8_t *d = sim_sd_get(CARD "/x.gci", &len);
        uint8_t *copy = malloc(len);
        memcpy(copy, d, len);
        sim_sd_put(CARD "/x.gci", copy, len - 1);
        free(copy);
    }
    void bad_entry(void) { make_gci("x.gci", "\xFF" "AFE", "01", "x", 1, 1); }
    void duplicate(void) {
        make_gci("x.gci", "GAFE", "01", "x", 1, 1);
        make_gci("y.gci", "GAFE", "01", "x", 2, 2);
    }
    void too_big(void) { make_gci("x.gci", "GAFE", "01", "x", 60, 1); }
    void too_many(void) {
        for (uint32_t i = 0; i < 128; i++) {
            char host[16], game[5];

            snprintf(host, sizeof(host), "s%u.gci", i);
            snprintf(game, sizeof(game), "G%03u", i);
            make_gci(host, game, "01", "s", 1, i + 1U);
        }
    }
    void long_name(void) {
        char host[200];

        memset(host, 'n', sizeof(host));
        strcpy(&host[150], ".gci");
        make_gci(host, "GAFE", "01", "x", 1, 1);
    }

#define CASE(fn, err) do { cases[n].what = #fn; cases[n].error = err; cases[n].setup = fn; n++; } while (0)
    CASE(too_short, GF_E_BAD_GCI);
    CASE(bad_size, GF_E_BAD_GCI);
    CASE(bad_entry, GF_E_BAD_GCI);
    CASE(duplicate, GF_E_DUPLICATE);
    CASE(too_big, GF_E_TOO_BIG);
    CASE(too_many, GF_E_TOO_MANY);
    CASE(long_name, GF_E_NAME);

    for (int i = 0; i < n; i++) {
        fresh();
        sim_sd_mkdirs(CARD);
        make_gci("good.gci", "GOOD", "01", "keep", 1, 77);
        cases[i].setup();

        folder_sum_t before = folder_sum();

        current = cases[i].what;
        CHECK(mount_size(cases[i].setup == too_many ? 16 : 4, 4) == GC_FOLDER_LOAD_FAILED);
        CHECK(gf.error == cases[i].error);
        CHECK(gc_folder_card_status_text() != NULL);
        CHECK(same(before, folder_sum()));
        CHECK(sim_sd_open_fds() == 0);

        size_t len = 0;

        CHECK(sim_sd_get(META "/import-errors.txt", &len) && len > 0);
        CHECK(gc_folder_card_close());
    }
    current = "test_import_refuses";

    /* fixing the problem clears the report */
    fresh();
    sim_sd_mkdirs(CARD);
    make_gci("good.gci", "GOOD", "01", "keep", 1, 77);
    make_gci("x.gci", "GAFE", "01", "x", 60, 1);
    CHECK(mount() == GC_FOLDER_LOAD_FAILED);
    CHECK(gc_folder_card_close());
    sim_sd_del(CARD "/x.gci");
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(!sd_exists(META "/import-errors.txt"));
    CHECK(gc_folder_card_close());
}

static void test_ignores_others(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 1, 1);
    sim_sd_put(CARD "/notes.txt", "hello", 5);
    sim_sd_put(CARD "/._a.gci", "resource fork", 13);
    sim_sd_put("MemoryCards/GC/Card1/Card1-1.raw", "raw image", 9);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    CHECK(con_saves() == 1);

    size_t len = 0;

    CHECK(sim_sd_get("MemoryCards/GC/Card1/Card1-1.raw", &len) && len == 9);
    CHECK(gc_folder_card_close());
}

/* --- identity --- */

static void test_header_is_kept(void) {
    uint8_t seeded[512];
    uint16_t sum = 0, inv = 0;

    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());

    /* a card image's real header: the serial must reach the console unchanged */
    size_t len = 0;
    memcpy(seeded, sim_sd_get(META "/card.dat", &len), 512);
    for (int i = 0; i < 12; i++)
        seeded[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 0x1FC; i += 2) {
        uint16_t w = (uint16_t)(seeded[i] << 8 | seeded[i + 1]);
        sum = (uint16_t)(sum + w);
        inv = (uint16_t)(inv + (uint16_t)~w);
    }
    put_be16(&seeded[0x1FC], sum == 0xFFFF ? 0 : sum);
    put_be16(&seeded[0x1FE], inv == 0xFFFF ? 0 : inv);
    sim_sd_put(META "/card.dat", seeded, 512);

    for (int i = 0; i < 3; i++) {
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        CHECK(memcmp(sim_psram, seeded, 512) == 0);
        console_up(4);

        int slot = con_create("GFZE", "8P", "fzero", 2);

        con_write_save((uint32_t)slot, 900U + (uint32_t)i);
        settle();
        CHECK(memcmp(sim_sd_get(META "/card.dat", &len), seeded, 512) == 0);
        con_delete((uint32_t)slot);
        settle();
        CHECK(gc_folder_card_close());
    }
}

static void test_bad_card_dat_stays_hidden(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 1, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());

    uint8_t junk[512];

    memset(junk, 0x33, sizeof(junk));
    sim_sd_put(META "/card.dat", junk, sizeof(junk));

    folder_sum_t before = folder_sum();

    CHECK(mount() == GC_FOLDER_LOAD_FAILED);
    CHECK(gf.error == GF_E_HEADER);
    CHECK(same(before, folder_sum()));
    size_t len;
    CHECK(memcmp(sim_sd_get(META "/card.dat", &len), junk, 512) == 0);
    CHECK(gc_folder_card_close());
}

static void test_damaged_saves_dat_rebuilds(void) {
    fresh();
    make_gci("kept name.gci", "GAFE", "01", "one", 1, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());

    sim_sd_put(META "/saves.dat", "garbage", 7);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    folder_matches_console();
    settle();

    /* the save keeps its host name when it changes */
    con_write_save((uint32_t)con_find("GAFE", "01", "one"), 5);
    settle();
    CHECK(sd_exists(CARD "/kept name.gci"));
    CHECK(folder_sum().files == 1);
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

/* --- live changes --- */

static void test_create_write_delete(void) {
    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    int a = con_create("GAFE", "01", "first", 3);
    int b = con_create("GZLE", "01", "second", 1);

    CHECK(a >= 0 && b >= 0);
    con_write_save((uint32_t)a, 100);
    con_write_save((uint32_t)b, 200);
    settle();
    CHECK(gf.state == GF_S_READY);
    CHECK(sd_exists(CARD "/01-GAFE-first.gci"));
    folder_matches_console();

    con_write_save((uint32_t)a, 101);
    settle();
    folder_matches_console();

    con_delete((uint32_t)b);
    settle();
    CHECK(!sd_exists(CARD "/01-GZLE-second.gci"));
    folder_matches_console();
    no_transaction_left();

    remount_matches(4);
    CHECK(gc_folder_card_close());
}

static void test_directory_only_change(void) {
    fresh();
    make_gci("host.gci", "GAFE", "01", "one", 2, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    con_touch((uint32_t)con_find("GAFE", "01", "one"), 0x11223344);
    settle();
    folder_matches_console();
    CHECK(sd_exists(CARD "/host.gci"));
    CHECK(gc_folder_card_close());
}

/* A game can rewrite its data and leave the directory alone. */
static void test_data_only_change(void) {
    fresh();
    make_gci("host.gci", "GAFE", "01", "one", 2, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    uint32_t slot = (uint32_t)con_find("GAFE", "01", "one");

    con_write_block(slot, 1, 77);
    settle();
    folder_matches_console();

    /* and back to the same bytes: nothing needs saving, and nothing may break */
    folder_sum_t before = folder_sum();

    con_write_block(slot, 1, 77);
    settle();
    CHECK(same(before, folder_sum()));
    no_transaction_left();
    CHECK(gc_folder_card_close());
}

static void test_rename_is_a_new_save(void) {
    fresh();
    make_gci("host.gci", "GAFE", "01", "one", 1, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    con_rename((uint32_t)con_find("GAFE", "01", "one"), "two");
    settle();
    folder_matches_console();
    CHECK(!sd_exists(CARD "/host.gci"));
    CHECK(sd_exists(CARD "/01-GAFE-two.gci"));
    CHECK(gc_folder_card_close());
}

static void test_fragmented_chains(void) {
    fresh();
    CHECK(mount_size(8, 4) == GC_FOLDER_LOAD_DONE);
    console_up(8);

    int a = con_create("GAAA", "01", "a", 3);
    int b = con_create("GBBB", "01", "b", 3);
    int c = con_create("GCCC", "01", "c", 3);

    con_write_save((uint32_t)a, 1);
    con_write_save((uint32_t)b, 2);
    con_write_save((uint32_t)c, 3);
    con_delete((uint32_t)b);

    /* takes b's blocks and then runs on past c */
    int d = con_create("GDDD", "01", "d", 100);

    CHECK(d >= 0);
    con_write_save((uint32_t)d, 4);
    settle();
    folder_matches_console();

    uint32_t seed;

    con_read_data((uint32_t)d, big_a);
    seed = sim_seed_of(big_a, 100);
    CHECK(seed == 4);

    /* a remount makes it contiguous, and the same save */
    remount_matches(8);
    con_read_data((uint32_t)con_find("GDDD", "01", "d"), big_a);
    CHECK(sim_seed_of(big_a, 100) == 4);
    CHECK(gc_folder_card_close());
}

static void test_all_sizes_round_trip(void) {
    static const uint32_t sizes[] = {4, 8, 16, 32, 64};

    for (int i = 0; i < 5; i++) {
        fresh();
        CHECK(mount_size(0, sizes[i]) == GC_FOLDER_LOAD_DONE);
        console_up(sizes[i]);

        int a = con_create("GAFE", "01", "last", sizes[i] * 16U - 5U);

        CHECK(a >= 0);
        CHECK(con_free_blocks() == 0);
        con_write_save((uint32_t)a, 321);
        settle();
        folder_matches_console();
        CHECK(gc_folder_card_close());

        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        CHECK(gc_folder_card_size_bytes() == sizes[i] * 1024U * 1024U / 8U);
        console_up(sizes[i]);
        CHECK(con_free_blocks() == 0);
        folder_matches_console();
        CHECK(gc_folder_card_close());
    }
}

static void test_names(void) {
    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    /* differ only in case: distinct saves to the console, one name to FAT */
    int a = con_create("GAFE", "01", "Save", 1);
    int b = con_create("GAFE", "01", "SAVE", 1);
    /* nothing a filesystem accepts as it is */
    int c = con_create("GAFE", "01", "a/b:c*d?\"<>|", 1);
    int d = con_create("G\x01\xFE" "E", " 1", "%done", 1);
    /* every byte must be escaped: the name gets long and has to stay a .gci */
    int e = con_create("GAFE", "01", "\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x8B\x8C\x8D\x8E\x8F\x90"
                                     "\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9A\x9B\x9C\x9D\x9E\x9F", 1);

    con_write_save((uint32_t)a, 1);
    con_write_save((uint32_t)b, 2);
    con_write_save((uint32_t)c, 3);
    con_write_save((uint32_t)d, 4);
    con_write_save((uint32_t)e, 5);
    settle();
    CHECK(gf.state == GF_S_READY);
    CHECK(folder_sum().files == 5);
    folder_matches_console();
    remount_matches(4);
    CHECK(gc_folder_card_close());
}

/* --- deletions --- */

static void test_format_goes_to_trash(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 1, 1);
    make_gci("b.gci", "GZLE", "01", "two", 1, 2);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    size_t len;
    uint8_t old_header[512];

    memcpy(old_header, sim_sd_get(META "/card.dat", &len), 512);

    con_format(0xC0FFEE);
    settle();
    CHECK(gf.state == GF_S_READY);
    console_up(4);
    CHECK(con_saves() == 0);
    CHECK(folder_sum().files == 0);
    CHECK(count_in(META "/trash").files == 2);
    CHECK(memcmp(sim_sd_get(META "/card.dat", &len), old_header, 512) != 0);

    /* the new identity is the one that comes back */
    uint8_t new_header[512];

    memcpy(new_header, sim_psram, 512);
    remount_matches(4);
    CHECK(memcmp(sim_psram, new_header, 512) == 0);
    CHECK(gc_folder_card_close());
}

static void test_last_save_goes_to_trash(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 1, 1);
    make_gci("b.gci", "GZLE", "01", "two", 1, 2);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    con_delete((uint32_t)con_find("GAFE", "01", "one"));
    settle();
    CHECK(count_in(META "/trash").files == 0);

    con_delete((uint32_t)con_find("GZLE", "01", "two"));
    settle();
    CHECK(count_in(META "/trash").files == 1);
    CHECK(folder_sum().files == 0);
    CHECK(gc_folder_card_close());
}

static void test_removed_name_is_not_reused(void) {
    fresh();
    make_gci("01-GAFE-one.gci", "GAFE", "01", "one", 1, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    /* same default name, because the name only differs in case, in one pass */
    con_delete((uint32_t)con_find("GAFE", "01", "one"));
    int a = con_create("GAFE", "01", "ONE", 1);

    con_write_save((uint32_t)a, 9);
    settle();
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

/* --- quiet time and coherence --- */

static void test_waits_for_quiet(void) {
    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    int a = con_create("GAFE", "01", "one", 2);

    con_write_save((uint32_t)a, 1);
    sim_drain();

    /* less than the quiet time: nothing moves */
    sim_time_advance_ms(100);
    for (int i = 0; i < 100; i++)
        gc_folder_card_task();
    CHECK(folder_sum().files == 0);
    CHECK(gc_folder_card_status_text() != NULL);

    /* a busy console holds it back whatever the time */
    sim_console_busy = true;
    sim_time_advance_ms(5000);
    for (int i = 0; i < 100; i++)
        gc_folder_card_task();
    CHECK(folder_sum().files == 0);
    sim_console_busy = false;

    settle();
    CHECK(folder_sum().files == 1);
    CHECK(gc_folder_card_close());
}

static void test_undrained_marks_hold_a_pass(void) {
    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    sim_hold_marks = true;

    int a = con_create("GAFE", "01", "one", 1);

    con_write_save((uint32_t)a, 1);
    sim_time_advance_ms(5000);
    for (int i = 0; i < 100; i++)
        gc_folder_card_task();
    CHECK(folder_sum().files == 0);

    sim_hold_marks = false;
    settle();
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

static void test_flush_and_close(void) {
    fresh();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    int a = con_create("GAFE", "01", "one", 2);

    con_write_save((uint32_t)a, 1);
    sim_drain();
    CHECK(!gc_folder_card_close());
    CHECK(gc_folder_card_flush());
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

static void test_invalid_card_is_not_saved(void) {
    fresh();
    make_gci("a.gci", "GAFE", "01", "one", 2, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    folder_sum_t before = folder_sum();

    /* something wrote over both directories */
    con_write_save((uint32_t)con_find("GAFE", "01", "one"), 9);
    sim_drain();
    memset(&sim_psram[SIM_BLOCK], 0x12, SIM_BLOCK * 2U);
    settle();
    CHECK(gf.state == GF_S_INVALID);
    CHECK(gc_folder_card_save_failing());
    CHECK(same(before, folder_sum()));
    CHECK(!gc_folder_card_flush());
    CHECK(!gc_folder_card_close());
    no_transaction_left();

    /* a later good write gets it going again */
    gf_sync_abandon();
    gf.state = GF_S_CLOSED;
}

/* What fails validation is one thing, and what validates but could never be imported is
 * another: neither may reach the card. */
static void test_unsaveable_cards(void) {
    for (int i = 0; i < 2; i++) {
        fresh();
        make_gci("a.gci", "GAFE", "01", "one", 2, 1);
        make_gci("b.gci", "GZLE", "01", "two", 2, 2);
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);

        folder_sum_t before = folder_sum();

        if (i == 0)
            con_share_blocks((uint32_t)con_find("GAFE", "01", "one"), (uint32_t)con_find("GZLE", "01", "two"));
        else
            CHECK(con_create("GNEW", "01", "", 1) >= 0);
        settle();
        CHECK(gf.state == GF_S_INVALID);
        CHECK(same(before, folder_sum()));
        no_transaction_left();

        gf_sync_abandon();
        gf.state = GF_S_CLOSED;
    }
}

/* --- power cuts, faults and racing writes --- */

typedef void (*scenario_t)(void);

static void scenario_mixed(void) {
    con_write_save((uint32_t)con_find("GAFE", "01", "keep"), 50);
    con_delete((uint32_t)con_find("GZLE", "01", "gone"));

    int a = con_create("GNEW", "01", "fresh", 2);

    con_write_save((uint32_t)a, 51);
}

static void scenario_format(void) {
    con_format(0xBEEF);
}

static void setup_three(void) {
    fresh();
    make_gci("01-GAFE-keep.gci", "GAFE", "01", "keep", 2, 41);
    make_gci("01-GZLE-gone.gci", "GZLE", "01", "gone", 1, 42);
    make_gci("01-GSTL-still.gci", "GSTL", "01", "still", 1, 43);
}

/* Cuts the power at each operation of one save in turn. Whatever happens, the folder must
 * be the old state or the new one and never anything else. */
static void power_cut_sweep(scenario_t scenario) {
    folder_sum_t before, after;
    void *snapshot;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());
    before = folder_sum();
    snapshot = sim_sd_snapshot();

    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario();
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    after = folder_sum();
    CHECK(gc_folder_card_close());
    CHECK(total > 20);

    int old_seen = 0, new_seen = 0;

    for (long at = 1; at <= total; at++) {
        sim_sd_restore(snapshot);
        sim_console_reset();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario();

        sim_op_count = 0;
        sim_crash_at = at;
        if (setjmp(sim_crash_point) == 0) {
            settle();
            CHECK(!"the crash was not reached");
        }

        sim_sd_power_cycle();
        sim_sd_clear_faults();
        memset(sim_psram, 0x55, sizeof(sim_psram));

        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        folder_matches_console();
        no_transaction_left();

        folder_sum_t now = folder_sum();

        CHECK(same(now, before) || same(now, after));
        old_seen += same(now, before);
        new_seen += same(now, after);

        /* and the card it recovered to is an ordinary one */
        sim_op_count = 0;
        CHECK(gc_folder_card_close());
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        CHECK(sim_op_count == 0);
        CHECK(gc_folder_card_close());
    }
    CHECK(old_seen > 0 && new_seen > 0);
    sim_sd_snapshot_free(snapshot);
}

static void test_power_cut_every_operation(void) {
    power_cut_sweep(scenario_mixed);
}

static void test_power_cut_during_format(void) {
    power_cut_sweep(scenario_format);
}

/* A format and a new save whose host name is the old save's, differing only in case, in one
 * transaction: a trash operation that ran twice would find the new save there. Whatever
 * survives, the old save must still exist somewhere. */
static void trash_replay_scenario(void) {
    int slot;

    con_format(0xBEEF);
    slot = con_create("GAFE", "01", "ONE", 1);
    CHECK(slot >= 0);
    con_write_save((uint32_t)slot, 9);
}

static void find_seed_cb(const char *path, bool is_dir, const uint8_t *data, size_t len, void *ctx) {
    (void)path;
    if (!is_dir && len == 64U + SIM_BLOCK && sim_seed_of(&data[64], 1) == 1)
        *(bool *)ctx = true;
}

static void test_trash_survives_replay(void) {
    long total;

    fresh();
    make_gci("01-GAFE-one.gci", "GAFE", "01", "one", 1, 1);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    trash_replay_scenario();
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    CHECK(gc_folder_card_close());
    CHECK(total > 10);

    for (long at = 1; at <= total; at++) {
        bool found = false;

        fresh();
        make_gci("01-GAFE-one.gci", "GAFE", "01", "one", 1, 1);
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        trash_replay_scenario();

        sim_op_count = 0;
        sim_crash_at = at;
        if (setjmp(sim_crash_point) == 0) {
            settle();
            CHECK(!"the crash was not reached");
        }
        sim_sd_power_cycle();
        sim_sd_clear_faults();
        memset(sim_psram, 0x55, sizeof(sim_psram));

        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        sim_sd_walk(CARD, find_seed_cb, &found);
        CHECK(found);
        CHECK(gc_folder_card_close());
    }
}

/* A second cut while recovering must not change the outcome. */
static void test_power_cut_during_recovery(void) {
    void *snapshot;
    folder_sum_t after;
    long apply_op = 0;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    after = folder_sum();
    CHECK(gc_folder_card_close());

    /* the earliest cut that leaves a committed journal behind */
    for (long at = 1; at <= total && !apply_op; at++) {
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario_mixed();
        sim_op_count = 0;
        sim_crash_at = at + 6;
        if (setjmp(sim_crash_point) == 0)
            settle();
        sim_sd_power_cycle();
        sim_sd_clear_faults();

        uint32_t seq, count;

        if (gf_journal_check(&seq, &count) == GF_JOURNAL_VALID)
            apply_op = at + 6;
    }
    CHECK(apply_op > 0);

    snapshot = sim_sd_snapshot();

    for (long at = 1; at < 200; at++) {
        sim_sd_restore(snapshot);
        sim_console_reset();
        sim_op_count = 0;
        sim_crash_at = at;
        if (setjmp(sim_crash_point) == 0) {
            mount();
            CHECK(gc_folder_card_close());
            if (sim_crash_at)
                break;
        }
        sim_sd_power_cycle();
        sim_sd_clear_faults();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        folder_matches_console();
        no_transaction_left();
        CHECK(same(folder_sum(), after));
        CHECK(gc_folder_card_close());
    }
    sim_sd_snapshot_free(snapshot);
}

/* Stops a pass right after its commit point, as a power cut would. */
static void cut_after_commit(void) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_drain();
    gc_folder_card_task();
    sim_time_advance_ms(1600);
    while (gf.phase != GF_SY_APPLY)
        gc_folder_card_task();
    sim_sd_power_cycle();
    gf_sync_abandon();
    gf.state = GF_S_CLOSED;
}

/* Recovery with a wrong bit in any read: a card that came up is checked after a clean mount,
 * and one that stayed hidden comes back. Nothing is given up on. */
static void test_read_bit_errors_in_recovery(void) {
    folder_sum_t after;
    void *snapshot;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    settle();
    after = folder_sum();
    CHECK(gc_folder_card_close());

    cut_after_commit();
    snapshot = sim_sd_snapshot();

    sim_sd_clear_faults();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    total = sim_read_count;
    CHECK(same(folder_sum(), after));
    CHECK(gc_folder_card_close());
    CHECK(total > 40);

    for (long at = 1; at <= total * 2; at++) {
        sim_sd_restore(snapshot);
        sim_console_reset();
        sim_sd_clear_faults();
        sim_read_flip_at = at <= total ? at : at - total;
        sim_read_flip_first = at > total;
        mount();
        CHECK(gc_folder_card_close());

        sim_sd_clear_faults();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        folder_matches_console();
        no_transaction_left();
        CHECK(same(folder_sum(), after));
        CHECK(gc_folder_card_close());
    }
    sim_sd_snapshot_free(snapshot);
}

static void test_damaged_committed_journal_stays_hidden(void) {
    static const size_t offsets[] = {9, 40};

    for (int i = 0; i < 2; i++) {
        cut_after_commit();
        flip_file(META "/journal.dat", offsets[i]);

        folder_sum_t before = folder_sum();

        CHECK(mount() == GC_FOLDER_LOAD_FAILED);
        CHECK(gf.error == GF_E_SD);
        CHECK(sd_exists(META "/journal.dat"));
        CHECK(same(before, folder_sum()));
        CHECK(gc_folder_card_close());
    }
}

/* Half a transaction is not started on a stage file that cannot finish it. */
static void test_journal_damaged_stage_stays_hidden(void) {
    cut_after_commit();
    flip_file(META "/stage/200.bin", 100);

    folder_sum_t before = folder_sum();

    CHECK(mount() == GC_FOLDER_LOAD_FAILED);
    CHECK(gf.error == GF_E_RECOVERY);
    CHECK(same(before, folder_sum()));
    CHECK(sd_exists(META "/journal.dat"));
    CHECK(gc_folder_card_close());
}

static void test_broken_journal_is_dropped(void) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    CHECK(gc_folder_card_close());

    folder_sum_t before = folder_sum();

    sim_sd_put(META "/journal.dat", "GCFJRN01 and then the power went out", 36);
    sim_sd_put(META "/stage/3.bin", "half a save", 11);
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    no_transaction_left();
    CHECK(same(before, folder_sum()));
    CHECK(gc_folder_card_close());
}

static void test_journal_missing_stage_stays_hidden(void) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_drain();

    /* stop right after the commit point */
    sim_op_count = 0;
    sim_time_advance_ms(1600);
    while (gf.phase != GF_SY_APPLY)
        gc_folder_card_task();
    sim_sd_power_cycle();

    uint32_t seq, count;

    CHECK(gf_journal_check(&seq, &count) == GF_JOURNAL_VALID);
    sim_sd_del(META "/stage/200.bin");

    folder_sum_t before = folder_sum();

    gf_sync_abandon();
    CHECK(mount() == GC_FOLDER_LOAD_FAILED);
    CHECK(gf.error == GF_E_RECOVERY);
    CHECK(same(before, folder_sum()));
    CHECK(sd_exists(META "/journal.dat"));
    CHECK(gc_folder_card_close());
}

static void test_unreadable_journal_is_kept(void) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_drain();
    sim_time_advance_ms(1600);
    while (gf.phase != GF_SY_APPLY)
        gc_folder_card_task();
    sim_sd_power_cycle();
    gf_sync_abandon();

    /* a read error is not a journal that was never committed */
    sim_read_count = 0;
    sim_read_fail_at = 1;
    CHECK(mount() == GC_FOLDER_LOAD_FAILED);
    CHECK(gf.error == GF_E_SD);
    CHECK(sd_exists(META "/journal.dat"));
    CHECK(gc_folder_card_close());

    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    no_transaction_left();
    CHECK(gc_folder_card_close());
}

static void test_fault_every_operation(void) {
    folder_sum_t after;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    after = folder_sum();
    CHECK(gc_folder_card_close());

    for (long at = 1; at <= total; at++) {
        /* once: the retry gets it through */
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario_mixed();
        sim_op_count = 0;
        sim_fail_at = at;
        settle();
        CHECK(gf.state == GF_S_READY);
        folder_matches_console();
        CHECK(same(folder_sum(), after));
        CHECK(!sd_exists(META "/journal.dat"));
        CHECK(sim_sd_open_fds() == 0);
        CHECK(gc_folder_card_close());

        /* and for good: the card refuses to go until the SD card works again */
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario_mixed();
        sim_op_count = 0;
        sim_fail_at = at;
        sim_fail_sticky = true;
        sim_drain();
        for (int i = 0; i < 40; i++) {
            sim_time_advance_ms(1000);
            for (int j = 0; j < 400; j++)
                gc_folder_card_task();
        }
        sim_drain();
        if (!gf_sync_clean()) {
            CHECK(gc_folder_card_save_failing());
            CHECK(!gc_folder_card_flush());
            CHECK(!gc_folder_card_close());
            CHECK(gc_folder_card_status_text() != NULL);
        }
        sim_fail_at = 0;
        sim_fail_sticky = false;
        CHECK(gc_folder_card_flush());
        folder_matches_console();
        CHECK(same(folder_sum(), after));
        CHECK(gc_folder_card_close());
    }
}

/* A stage file that reads back different is a failed pass, not a wrong .gci. */
static void stage_bit_sweep(scenario_t scenario) {
    folder_sum_t after;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario();
    sim_sd_clear_faults();
    settle();
    total = sim_stage_writes;
    after = folder_sum();
    CHECK(gc_folder_card_close());
    CHECK(total > 0);

    for (long at = 1; at <= total; at++) {
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario();
        sim_sd_clear_faults();
        sim_stage_flip_at = at;
        settle();
        CHECK(gf.state == GF_S_READY);
        folder_matches_console();
        CHECK(same(folder_sum(), after));
        CHECK(gc_folder_card_close());
    }
}

static void test_stage_bit_errors(void) {
    stage_bit_sweep(scenario_mixed);
    stage_bit_sweep(scenario_format);
}

static void read_bit_sweep(scenario_t scenario, bool first_byte) {
    folder_sum_t after;
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario();
    sim_sd_clear_faults();
    settle();
    total = sim_read_count;
    after = folder_sum();
    CHECK(gc_folder_card_close());
    CHECK(total > 0);

    for (long at = 1; at <= total; at++) {
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario();
        sim_sd_clear_faults();
        sim_read_flip_at = at;
        sim_read_flip_first = first_byte;
        settle();
        sim_read_flip_at = 0;
        CHECK(gf.state == GF_S_READY);
        folder_matches_console();
        CHECK(same(folder_sum(), after));
        CHECK(gc_folder_card_close());
    }
}

static void test_read_bit_errors(void) {
    read_bit_sweep(scenario_mixed, false);
    read_bit_sweep(scenario_mixed, true);
    read_bit_sweep(scenario_format, true);
}

static void test_card_fills_up(void) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);

    folder_sum_t before = folder_sum();

    scenario_mixed();
    sim_free_bytes = 1000;
    sim_drain();
    gc_folder_card_task();
    sim_time_advance_ms(1600);
    for (int i = 0; i < 2000; i++)
        gc_folder_card_task();
    CHECK(gf.state == GF_S_SAVE_FAILED);
    CHECK(same(before, folder_sum()));

    sim_free_bytes = -1;
    settle();
    folder_matches_console();
    CHECK(gc_folder_card_close());
}

/* The console writes while a pass runs. */
static long race_at;
static uint32_t race_slot;
static uint32_t race_seed;
static bool race_whole_save;

static void race_hook(long op) {
    if (op == race_at && race_whole_save) {
        con_write_save(race_slot, race_seed);
        sim_drain();
    } else if (op == race_at) {
        con_write_block(race_slot, 0, race_seed);
        sim_drain();
    } else if (op == race_at + 2 && !race_whole_save) {
        con_write_block(race_slot, 1, race_seed);
        con_touch(race_slot, race_seed);
        sim_drain();
    }
}

static void test_console_write_at_every_operation(void) {
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    CHECK(gc_folder_card_close());

    for (long at = 1; at <= total; at++) {
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);
        scenario_mixed();

        race_at = at;
        race_slot = (uint32_t)con_find("GAFE", "01", "keep");
        race_seed = 60;
        sim_op_count = 0;
        sim_op_hook = race_hook;
        settle();
        sim_op_hook = NULL;

        /* a write late in the last pass may need one more */
        settle();
        CHECK(gf.state == GF_S_READY);
        folder_matches_console();

        size_t len = 0;
        const uint8_t *file = sim_sd_get(CARD "/01-GAFE-keep.gci", &len);

        CHECK(file != NULL && sim_seed_of(&file[64], 2) == 60);
        CHECK(gc_folder_card_close());
    }
}

/* A cut after a racing write: what is on the card must never mix two versions. */
static void race_and_cut(long at, long cut, bool whole_save) {
    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    scenario_mixed();

    race_at = at;
    race_slot = (uint32_t)con_find("GAFE", "01", "keep");
    race_seed = 60;
    race_whole_save = whole_save;
    sim_op_count = 0;
    sim_op_hook = race_hook;
    sim_crash_at = at + cut;
    if (setjmp(sim_crash_point) == 0) {
        settle();
        sim_op_hook = NULL;
        sim_crash_at = 0;
        settle();
        CHECK(gc_folder_card_close());
    }
    race_whole_save = false;
    sim_sd_power_cycle();
    sim_sd_clear_faults();
    memset(sim_psram, 0x55, sizeof(sim_psram));

    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    folder_matches_console();

    size_t len = 0;
    const uint8_t *file = sim_sd_get(CARD "/01-GAFE-keep.gci", &len);

    CHECK(file != NULL);

    uint32_t seed = sim_seed_of(&file[64], 2);

    CHECK(seed == 41 || seed == 50 || seed == 60);
    CHECK(gc_folder_card_close());
}

static void test_racing_write_then_power_cut(void) {
    for (long at = 1; at < 160; at += 3) {
        for (long cut = 1; cut < 12; cut += 2)
            race_and_cut(at, cut, false);
    }

    /* one write landing while a save is being staged, and enough time to commit whatever
     * that made of it */
    for (long at = 1; at < 120; at += 4) {
        for (long cut = 1; cut < 100; cut += 5)
            race_and_cut(at, cut, true);
    }
}

/* A pass given up on must not forget what it had seen. */
static uint32_t abandon_slot;
static long abandon_at;

static void abandon_hook(long op) {
    if (op == abandon_at) {
        con_write_block(abandon_slot, 0, 71);
        sim_drain();
    }
}

static void test_abandoned_pass_keeps_marks(void) {
    long total;

    setup_three();
    CHECK(mount() == GC_FOLDER_LOAD_DONE);
    console_up(4);
    con_write_block((uint32_t)con_find("GAFE", "01", "keep"), 0, 70);
    sim_op_count = 0;
    settle();
    total = sim_op_count;
    CHECK(gc_folder_card_close());

    for (long at = 1; at <= total; at++) {
        setup_three();
        CHECK(mount() == GC_FOLDER_LOAD_DONE);
        console_up(4);

        /* data only, in two saves: nothing but the marks says that either changed */
        con_write_block((uint32_t)con_find("GAFE", "01", "keep"), 0, 70);
        abandon_slot = (uint32_t)con_find("GSTL", "01", "still");
        abandon_at = at;
        sim_op_count = 0;
        sim_op_hook = abandon_hook;
        settle();
        sim_op_hook = NULL;
        settle();
        folder_matches_console();
        CHECK(gc_folder_card_close());
    }
}

/* Random sessions, with power cuts in places nobody chose. */
static uint32_t rng_state;

static uint32_t rnd(uint32_t limit) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % limit;
}

static void random_op(uint32_t *seed) {
    uint32_t slots[127];
    uint32_t used = 0;
    uint8_t entry[64];

    for (uint32_t slot = 0; slot < 127; slot++) {
        if (con_entry(slot, entry))
            slots[used++] = slot;
    }

    switch (rnd(6)) {
        case 0:
        case 1: {
            char game[5], name[12];

            snprintf(game, sizeof(game), "G%03u", rnd(40));
            snprintf(name, sizeof(name), "%s%u", rnd(2) ? "save" : "SAVE", rnd(6));
            if (con_find(game, "01", name) < 0) {
                int slot = con_create(game, "01", name, 1 + rnd(6));

                if (slot >= 0)
                    con_write_save((uint32_t)slot, ++*seed);
            }
            break;
        }
        case 2:
        case 3:
            if (used)
                con_write_save(slots[rnd(used)], ++*seed);
            break;
        case 4:
            if (used)
                con_delete(slots[rnd(used)]);
            break;
        default:
            if (used)
                con_touch(slots[rnd(used)], ++*seed);
            break;
    }
}

static void test_random_sessions(void) {
    long rounds = getenv("PF_LONG") ? 2000 : 150;
    uint32_t seed = 1000;

    for (long run = 0; run < rounds; run++) {
        rng_state = (uint32_t)run * 2654435761U + 12345U;
        fresh();
        CHECK(mount_size(0, rnd(2) ? 4 : 8) == GC_FOLDER_LOAD_DONE);

        for (int cycle = 0; cycle < 6; cycle++) {
            uint32_t mbits = gcfs_header_mbits(gf.header);

            console_up(mbits);
            for (uint32_t i = 0, n = 1 + rnd(6); i < n; i++)
                random_op(&seed);

            bool cut = rnd(3) == 0;

            if (cut) {
                sim_op_count = 0;
                sim_crash_at = 1 + rnd(120);
            }
            if (setjmp(sim_crash_point) == 0) {
                settle();
                sim_crash_at = 0;
                CHECK(gf.state == GF_S_READY);
                folder_matches_console();
                CHECK(gc_folder_card_close());
            }

            sim_sd_power_cycle();
            sim_sd_clear_faults();
            memset(sim_psram, 0x55, sizeof(sim_psram));
            CHECK(mount() == GC_FOLDER_LOAD_DONE);
            console_up(mbits);
            folder_matches_console();
            no_transaction_left();
        }
        CHECK(gc_folder_card_close());
        CHECK(sim_sd_open_fds() == 0);
    }
}

/* --- configuration --- */

static void ini(const char *text) {
    sim_sd_put("MemoryCards/GC/Card1/Card1.ini", text, strlen(text));
}

static void test_backend_selection(void) {
    fresh();

    /* a directory that exists selects nothing */
    sim_sd_mkdirs(CARD);
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_RAW);

    ini("[Settings]\nStorageBackend=FOLDER\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_FOLDER);

    ini("[Settings]\nStorageBackend=RAW\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_RAW);

    ini("[Settings]\nStorageBackend=folder\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_RAW);

    ini("[Settings]\nStorageBackend=\nCardSize=16\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_RAW);
    CHECK(card_config_get_gc_cardsize("Card1", "Card1") == 16);

    ini("[Other]\nStorageBackend=FOLDER\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_RAW);

    /* the default from settings.ini, for a card that does not choose */
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_FOLDER) == CARD_BACKEND_FOLDER);
    CHECK(card_config_get_gc_backend("NoSuchCard", "NoSuchCard", CARD_BACKEND_FOLDER) == CARD_BACKEND_FOLDER);

    /* a card that does choose wins, and a value it got wrong still means RAW */
    ini("[Settings]\nStorageBackend=RAW\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_FOLDER) == CARD_BACKEND_RAW);
    ini("[Settings]\nStorageBackend=folder\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_FOLDER) == CARD_BACKEND_RAW);
    ini("[Settings]\nStorageBackend=FOLDER\n");
    CHECK(card_config_get_gc_backend("Card1", "Card1", CARD_BACKEND_RAW) == CARD_BACKEND_FOLDER);

    /* a folder name as long as they get */
    char dir[128], file[200], text[] = "[Settings]\nStorageBackend=FOLDER\n";
    char name[32];

    memset(name, 'n', 31);
    name[31] = 0;
    snprintf(dir, sizeof(dir), "MemoryCards/GC/%s", name);
    sim_sd_mkdirs(dir);
    snprintf(file, sizeof(file), "%s/%s.ini", dir, name);
    sim_sd_put(file, text, strlen(text));
    CHECK(card_config_get_gc_backend(name, name, CARD_BACKEND_RAW) == CARD_BACKEND_FOLDER);
}

#define TEST(fn) { #fn, fn }

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    TEST(test_empty_card_all_sizes),
    TEST(test_no_full_image),
    TEST(test_import_saves),
    TEST(test_new_cards_share_a_serial),
    TEST(test_second_mount_writes_nothing),
    TEST(test_slots_are_stable),
    TEST(test_card_exactly_full),
    TEST(test_import_refuses),
    TEST(test_ignores_others),
    TEST(test_header_is_kept),
    TEST(test_bad_card_dat_stays_hidden),
    TEST(test_damaged_saves_dat_rebuilds),
    TEST(test_create_write_delete),
    TEST(test_directory_only_change),
    TEST(test_data_only_change),
    TEST(test_rename_is_a_new_save),
    TEST(test_fragmented_chains),
    TEST(test_all_sizes_round_trip),
    TEST(test_names),
    TEST(test_format_goes_to_trash),
    TEST(test_last_save_goes_to_trash),
    TEST(test_removed_name_is_not_reused),
    TEST(test_waits_for_quiet),
    TEST(test_undrained_marks_hold_a_pass),
    TEST(test_flush_and_close),
    TEST(test_invalid_card_is_not_saved),
    TEST(test_unsaveable_cards),
    TEST(test_power_cut_every_operation),
    TEST(test_power_cut_during_format),
    TEST(test_trash_survives_replay),
    TEST(test_power_cut_during_recovery),
    TEST(test_read_bit_errors_in_recovery),
    TEST(test_damaged_committed_journal_stays_hidden),
    TEST(test_journal_damaged_stage_stays_hidden),
    TEST(test_broken_journal_is_dropped),
    TEST(test_journal_missing_stage_stays_hidden),
    TEST(test_unreadable_journal_is_kept),
    TEST(test_fault_every_operation),
    TEST(test_stage_bit_errors),
    TEST(test_read_bit_errors),
    TEST(test_card_fills_up),
    TEST(test_console_write_at_every_operation),
    TEST(test_racing_write_then_power_cut),
    TEST(test_abandoned_pass_keeps_marks),
    TEST(test_random_sessions),
    TEST(test_backend_selection),
};

int main(int argc, char **argv) {
    int ran = 0;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc > 1 && !strstr(tests[i].name, argv[1]))
            continue;
        current = tests[i].name;
        tests[i].fn();
        printf("ok   %s\n", tests[i].name);
        ran++;
    }
    printf("%d tests, %d checks\n", ran, checks);
    return 0;
}
