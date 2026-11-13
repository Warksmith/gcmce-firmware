#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "card_config.h"
#include "sim.h"
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static void fresh(void) { sim_sd_reset(); }
static void mapping(const char *text) {
    sim_sd_mkdirs(".flippermce");
    sim_sd_put(".flippermce/Game2Folder.ini", text, strlen(text));
}

static void expect_mapping(const char *id, const char *expected) {
    char folder[MAX_FOLDER_NAME_LENGTH] = "stale";
    card_config_get_card_folder(id, folder, sizeof(folder));
    CHECK(strcmp(folder, expected) == 0);
    CHECK(sim_sd_open_fds() == 0);
}

static void test_game2folder_precedence(void) {
    fresh();
    expect_mapping("DL-DOL-GT4P-EUR", "");
    mapping("[GC]\nDL-DOL-GT4P-EUR=Exact\nDL-DOL-GT4*=Series\nDL-DOL-*=All\n");
    expect_mapping("DL-DOL-GT4P-EUR", "Exact");
    expect_mapping("DL-DOL-GT4E-USA", "Series");
    expect_mapping("DL-DOL-GALE-USA", "All");
    mapping("[GC]\nDL-DOL-*=All\nDL-DOL-GT4**=Series\nDL-DOL-GT4P-EUR=Exact\n");
    expect_mapping("DL-DOL-GT4P-EUR", "Exact");
    expect_mapping("DL-DOL-GT4E-USA", "Series");
    mapping("[GC]\nDL-DOL-GT4*=First\nDL-DOL-GT4**=Last\n");
    expect_mapping("DL-DOL-GT4P-EUR", "Last");
    mapping("[GC]\nDL-DOL-GT4P-EUR=First\nDL-DOL-GT4P-EUR=Last\n");
    expect_mapping("DL-DOL-GT4P-EUR", "Last");
    mapping("[GC]\n*=CatchAll\nDL-DOL-GT4*=Series\n");
    expect_mapping("DL-DOL-GALE-USA", "CatchAll");
    expect_mapping("DL-DOL-GT4P-EUR", "Series");
    mapping("[GC]\nDL-*-EUR=Invalid\nDL-DOL-GT4?-EUR=Invalid\nGT4P=Short\n"
            "[PS2]\n*=WrongSection\n");
    expect_mapping("DL-DOL-GT4P-EUR", "");
    expect_mapping("GT4P", "Short");
}

static void test_game2folder_default(void) {
    fresh();
    mapping("[GC]\nDefaultCard=Shared\n");
    expect_mapping("DL-DOL-GT4P-EUR", "");
    mapping("[GC]\nUseDefaultCard=OFF\nDefaultCard=Shared\n");
    expect_mapping("DL-DOL-GT4P-EUR", "");
    mapping("[GC]\nUseDefaultCard=yes\nDefaultCard=Shared\n");
    expect_mapping("DL-DOL-GT4P-EUR", "");
    mapping("[GC]\nDefaultCard=Shared\nUseDefaultCard=on\nDL-DOL-GT4*=Series\n");
    expect_mapping("DL-DOL-GALE-USA", "Shared");
    expect_mapping("DL-DOL-GT4P-EUR", "Series");
    mapping("[GC]\nUseDefaultCard=ON\nDefaultCard=Shared\nDL-DOL-GT4P-EUR=\n");
    expect_mapping("DL-DOL-GT4P-EUR", ""); /* Empty mapping opts out of fallback. */
    mapping("[GC]\nUseDefaultCard=ON\n");
    expect_mapping("DL-DOL-GALE-USA", "");
    mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=Wrong\n[GC]\n");
    expect_mapping("DL-DOL-GALE-USA", "");
    mapping("[GC]\nUseDefaultCard=ON\nDefaultCard=Shared\n");
    expect_mapping(NULL, "");
    expect_mapping("", "");
}

static void test_game2folder_buffers(void) {
    fresh();
    mapping("[GC]\nX=Shared\nUseDefaultCard=ON\nDefaultCard=Shared\n");
    char small[5] = "keep";
    card_config_get_card_folder("X", small, 0);
    CHECK(strcmp(small, "keep") == 0);
    card_config_get_card_folder("X", NULL, 8);
    card_config_get_card_folder("X", small, 1);
    CHECK(small[0] == '\0');
    card_config_get_card_folder("X", small, sizeof(small));
    CHECK(strcmp(small, "Shar") == 0);
    card_config_get_card_folder("Y", small, sizeof(small));
    CHECK(strcmp(small, "Shar") == 0);
    expect_mapping("X", "Shared"); /* Short input must not be read as a 15-byte array. */
    mapping("[GC]\nX=123456789012345678901234567890123456789\n"
            "UseDefaultCard=ON\nDefaultCard=123456789012345678901234567890123456789\n");
    expect_mapping("X", "1234567890123456789012345678901");
    expect_mapping("Y", "1234567890123456789012345678901");
}

int main(void) {
    test_game2folder_precedence();
    test_game2folder_default();
    test_game2folder_buffers();
    sim_sd_reset();
    printf("mapping: %d checks passed\n", checks);
    return 0;
}
