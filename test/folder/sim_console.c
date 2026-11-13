/* The console's CARD library, reduced to what reaches the card: libogc's order of updates
 * (create: BAT then directory; delete: directory then BAT; write: data in place, then the
 * directory; each directory or BAT update goes to the older copy). Deliberately does not
 * use gc_fs.c or gc_gci.c. */

#include "sim.h"

#include <assert.h>
#include <string.h>

#include "gc_folder_card.h"
#include "gc_folder_port.h"

#define PAGE 128U
#define ERASE_PAGES 16U
#define SEG 512U
#define FIRST_DATA 5U

uint8_t sim_psram[SIM_PSRAM_SIZE];
uint32_t sim_generation;
bool sim_hold_marks;
bool sim_console_busy;

static uint64_t now_us;
static uint32_t marks[65536];
static uint32_t mark_count;

static struct {
    uint32_t mbits;
    uint32_t blocks;
    uint8_t dir[SIM_BLOCK];
    uint8_t bat[SIM_BLOCK];
    uint32_t dir_target;
    uint32_t bat_target;
} con;

/* --- the platform port the engine runs on --- */

void pf_psram_read(uint32_t addr, void *buf, size_t len) {
    assert(addr + len <= SIM_PSRAM_SIZE);
    memcpy(buf, &sim_psram[addr], len);
}

void pf_psram_write(uint32_t addr, const void *buf, size_t len) {
    assert(addr + len <= SIM_PSRAM_SIZE);
    memcpy(&sim_psram[addr], buf, len);
}

uint64_t pf_time_us(void) {
    return now_us += 20;
}

void pf_write_snapshot(uint32_t *generation, bool *pending) {
    *generation = sim_generation;
    *pending = mark_count != 0;
}

bool pf_console_quiet(void) {
    return !sim_console_busy;
}

void sim_time_advance_ms(uint32_t ms) {
    now_us += (uint64_t)ms * 1000U;
}

void sim_drain(void) {
    if (sim_hold_marks)
        return;
    for (uint32_t i = 0; i < mark_count; i++)
        gc_folder_card_note_segment(marks[i]);
    mark_count = 0;
}

void sim_console_reset(void) {
    memset(&con, 0, sizeof(con));
    mark_count = 0;
    sim_hold_marks = false;
    sim_console_busy = false;
    memset(sim_psram, 0x55, sizeof(sim_psram));
}

/* --- card access --- */

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void mark(uint32_t addr) {
    assert(mark_count < sizeof(marks) / sizeof(marks[0]));
    marks[mark_count++] = addr / SEG;
    sim_generation++;
    sim_drain();
}

static void card_erase(uint32_t block) {
    uint32_t addr = block * SIM_BLOCK;

    memset(&sim_psram[addr], 0xFF, SIM_BLOCK);
    for (uint32_t i = 0; i < ERASE_PAGES; i++)
        mark(addr + i * SEG);
}

static void card_write(uint32_t addr, const uint8_t *data, uint32_t len) {
    for (uint32_t off = 0; off < len; off += PAGE) {
        memcpy(&sim_psram[addr + off], &data[off], PAGE);
        mark(addr + off);
    }
}

static void write_whole_block(uint32_t block, const uint8_t *data) {
    card_erase(block);
    card_write(block * SIM_BLOCK, data, SIM_BLOCK);
}

static void sums(const uint8_t *data, uint32_t len, uint16_t *sum, uint16_t *inv) {
    uint16_t a = 0, b = 0;

    for (uint32_t i = 0; i + 1 < len; i += 2) {
        uint16_t w = get16(&data[i]);

        a = (uint16_t)(a + w);
        b = (uint16_t)(b + (uint16_t)~w);
    }
    *sum = a == 0xFFFF ? 0 : a;
    *inv = b == 0xFFFF ? 0 : b;
}

static bool dir_ok(const uint8_t *dir) {
    uint16_t a, b;

    sums(dir, 0x1FFC, &a, &b);
    return a == get16(&dir[0x1FFC]) && b == get16(&dir[0x1FFE]);
}

static bool bat_ok(const uint8_t *bat, uint32_t blocks) {
    uint16_t a, b;
    uint32_t free_count = 0;

    sums(&bat[4], 0x1FFC, &a, &b);
    if (a != get16(&bat[0]) || b != get16(&bat[2]))
        return false;
    for (uint32_t i = 0; i < blocks - FIRST_DATA; i++)
        free_count += get16(&bat[0xA + i * 2U]) == 0;
    return free_count == get16(&bat[6]);
}

static uint16_t bat_next(uint32_t block) {
    return get16(&con.bat[0xA + (block - FIRST_DATA) * 2U]);
}

static void bat_set(uint32_t block, uint32_t next) {
    put16(&con.bat[0xA + (block - FIRST_DATA) * 2U], next);
}

static void commit_dir(void) {
    uint16_t a, b;

    put16(&con.dir[0x1FFA], (uint16_t)(get16(&con.dir[0x1FFA]) + 1U));
    sums(con.dir, 0x1FFC, &a, &b);
    put16(&con.dir[0x1FFC], a);
    put16(&con.dir[0x1FFE], b);
    write_whole_block(con.dir_target, con.dir);
    con.dir_target = con.dir_target == 1 ? 2 : 1;
}

static void commit_bat(void) {
    uint16_t a, b;
    uint32_t free_count = 0;

    for (uint32_t i = 0; i < con.blocks - FIRST_DATA; i++)
        free_count += get16(&con.bat[0xA + i * 2U]) == 0;
    put16(&con.bat[6], free_count);
    put16(&con.bat[4], (uint16_t)(get16(&con.bat[4]) + 1U));
    sums(&con.bat[4], 0x1FFC, &a, &b);
    put16(&con.bat[0], a);
    put16(&con.bat[2], b);
    write_whole_block(con.bat_target, con.bat);
    con.bat_target = con.bat_target == 3 ? 4 : 3;
}

const char *con_mount(uint32_t mbits) {
    const uint8_t *hdr = sim_psram;
    const uint8_t *dirs[2] = { &sim_psram[SIM_BLOCK], &sim_psram[SIM_BLOCK * 2U] };
    const uint8_t *bats[2] = { &sim_psram[SIM_BLOCK * 3U], &sim_psram[SIM_BLOCK * 4U] };
    static uint8_t owned[1024];
    uint16_t a, b;
    uint32_t used = 0;
    int d, t;

    con.mbits = mbits;
    con.blocks = mbits * 16U;

    sums(hdr, 0x1FC, &a, &b);
    if (a != get16(&hdr[0x1FC]) || b != get16(&hdr[0x1FE]))
        return "header checksum";
    if (get16(&hdr[0x22]) != mbits)
        return "header size";

    bool d0 = dir_ok(dirs[0]), d1 = dir_ok(dirs[1]);

    if (!d0 || !d1)
        return "a directory copy is damaged";
    d = (d0 && d1) ? ((int16_t)(uint16_t)(get16(&dirs[0][0x1FFA]) - get16(&dirs[1][0x1FFA])) >= 0 ? 0 : 1) : (d0 ? 0 : 1);

    bool b0 = bat_ok(bats[0], con.blocks), b1 = bat_ok(bats[1], con.blocks);

    if (!b0 || !b1)
        return "a BAT copy is damaged";
    t = (b0 && b1) ? ((int16_t)(uint16_t)(get16(&bats[0][4]) - get16(&bats[1][4])) >= 0 ? 0 : 1) : (b0 ? 0 : 1);

    memcpy(con.dir, dirs[d], SIM_BLOCK);
    memcpy(con.bat, bats[t], SIM_BLOCK);
    con.dir_target = d == 0 ? 2 : 1;
    con.bat_target = t == 0 ? 4 : 3;

    memset(owned, 0, sizeof(owned));
    for (uint32_t slot = 0; slot < 127; slot++) {
        const uint8_t *e = &con.dir[slot * 64U];

        if (e[0] == 0xFF)
            continue;

        uint32_t block = get16(&e[0x36]);
        uint32_t count = get16(&e[0x38]);

        if (count == 0)
            return "entry without blocks";
        for (uint32_t i = 0; i < count; i++) {
            if (block < FIRST_DATA || block >= con.blocks)
                return "chain leaves the card";
            if (owned[block])
                return "two saves share a block";
            owned[block] = 1;
            used++;

            uint32_t next = bat_next(block);

            if ((i + 1U == count) != (next == 0xFFFF))
                return "chain length does not match the entry";
            block = next;
        }
    }

    if (used + get16(&con.bat[6]) != con.blocks - FIRST_DATA)
        return "free count does not match the directory";
    return NULL;
}

uint32_t con_free_blocks(void) {
    return get16(&con.bat[6]);
}

uint32_t con_saves(void) {
    uint32_t n = 0;

    for (uint32_t slot = 0; slot < 127; slot++)
        n += con.dir[slot * 64U] != 0xFF;
    return n;
}

bool con_entry(uint32_t slot, uint8_t entry[64]) {
    memcpy(entry, &con.dir[slot * 64U], 64);
    return entry[0] != 0xFF;
}

void con_read_data(uint32_t slot, uint8_t *data) {
    const uint8_t *e = &con.dir[slot * 64U];
    uint32_t block = get16(&e[0x36]);
    uint32_t count = get16(&e[0x38]);

    for (uint32_t i = 0; i < count; i++) {
        memcpy(&data[i * SIM_BLOCK], &sim_psram[block * SIM_BLOCK], SIM_BLOCK);
        block = bat_next(block);
    }
}

int con_find(const char *game, const char *maker, const char *name) {
    uint8_t want[32] = {0};

    strncpy((char *)want, name, sizeof(want));
    for (uint32_t slot = 0; slot < 127; slot++) {
        const uint8_t *e = &con.dir[slot * 64U];

        if (e[0] != 0xFF && memcmp(e, game, 4) == 0 && memcmp(&e[4], maker, 2) == 0 && strncmp((const char *)&e[8], name, 32) == 0)
            return (int)slot;
    }
    return -1;
}

int con_create(const char *game, const char *maker, const char *name, uint32_t blocks) {
    uint32_t slot = 0;
    uint32_t cur = get16(&con.bat[8]);
    uint32_t first = 0, prev = 0;

    while (slot < 127 && con.dir[slot * 64U] != 0xFF)
        slot++;
    if (slot == 127 || con_free_blocks() < blocks)
        return -1;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t tries = 0;

        do {
            cur++;
            if (cur >= con.blocks)
                cur = FIRST_DATA;
            assert(tries++ <= con.blocks);
        } while (bat_next(cur) != 0);

        if (prev)
            bat_set(prev, cur);
        else
            first = cur;
        bat_set(cur, 0xFFFF);
        prev = cur;
    }
    put16(&con.bat[8], cur);
    commit_bat();

    uint8_t *e = &con.dir[slot * 64U];

    memset(e, 0, 64);
    memcpy(e, game, 4);
    memcpy(&e[4], maker, 2);
    e[6] = 0xFF;
    e[7] = 2;
    strncpy((char *)&e[8], name, 32);
    e[0x28] = 0x12;
    e[0x34] = 4;
    put16(&e[0x36], first);
    put16(&e[0x38], blocks);
    e[0x3A] = e[0x3B] = 0xFF;
    commit_dir();
    return (int)slot;
}

void sim_fill(uint8_t *block, uint32_t seed, uint32_t index) {
    uint32_t x = seed * 2654435761U + index * 40503U + 1U;

    for (uint32_t i = 0; i < SIM_BLOCK; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        memcpy(&block[i], &x, 4);
    }
    memcpy(block, &seed, 4);
}

uint32_t sim_seed_of(const uint8_t *data, uint32_t blocks) {
    static uint8_t expect[SIM_BLOCK];
    uint32_t seed;

    memcpy(&seed, data, 4);
    for (uint32_t i = 0; i < blocks; i++) {
        sim_fill(expect, seed, i);
        if (memcmp(expect, &data[i * SIM_BLOCK], SIM_BLOCK) != 0)
            return 0;
    }
    return seed;
}

void con_write_block(uint32_t slot, uint32_t index, uint32_t seed) {
    static uint8_t data[SIM_BLOCK];
    const uint8_t *e = &con.dir[slot * 64U];
    uint32_t block = get16(&e[0x36]);

    for (uint32_t i = 0; i < index; i++)
        block = bat_next(block);
    sim_fill(data, seed, index);
    write_whole_block(block, data);
}

void con_touch(uint32_t slot, uint32_t mtime) {
    uint8_t *e = &con.dir[slot * 64U];

    e[0x28] = (uint8_t)(mtime >> 24);
    e[0x29] = (uint8_t)(mtime >> 16);
    e[0x2A] = (uint8_t)(mtime >> 8);
    e[0x2B] = (uint8_t)mtime;
    commit_dir();
}

void con_write_save(uint32_t slot, uint32_t seed) {
    uint32_t count = get16(&con.dir[slot * 64U + 0x38U]);

    for (uint32_t i = 0; i < count; i++)
        con_write_block(slot, i, seed);
    con_touch(slot, seed);
}

void con_rename(uint32_t slot, const char *name) {
    uint8_t *e = &con.dir[slot * 64U];

    memset(&e[8], 0, 32);
    strncpy((char *)&e[8], name, 32);
    commit_dir();
}

void con_delete(uint32_t slot) {
    uint8_t *e = &con.dir[slot * 64U];
    uint32_t block = get16(&e[0x36]);
    uint32_t count = get16(&e[0x38]);

    memset(e, 0xFF, 64);
    commit_dir();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t next = bat_next(block);

        bat_set(block, 0);
        block = next;
    }
    commit_bat();
}

void con_share_blocks(uint32_t slot, uint32_t other) {
    memcpy(&con.dir[other * 64U + 0x36U], &con.dir[slot * 64U + 0x36U], 4);
    commit_dir();
}

/* A new identity, an empty directory and an empty BAT, written block by block. */
void con_format(uint32_t serial) {
    static uint8_t block[SIM_BLOCK];
    uint16_t a, b;

    memset(block, 0xFF, sizeof(block));
    memset(block, 0, 0x26);
    memcpy(block, &serial, 4);
    block[0x0C] = (uint8_t)serial;
    put16(&block[0x22], con.mbits);
    sums(block, 0x1FC, &a, &b);
    put16(&block[0x1FC], a);
    put16(&block[0x1FE], b);
    write_whole_block(0, block);

    memset(con.dir, 0xFF, sizeof(con.dir));
    put16(&con.dir[0x1FFA], 0xFFFFU);
    con.dir_target = 1;
    commit_dir();
    con.dir_target = 2;
    put16(&con.dir[0x1FFA], 0xFFFFU);
    commit_dir();

    memset(con.bat, 0, sizeof(con.bat));
    put16(&con.bat[8], 4);
    put16(&con.bat[4], 0xFFFFU);
    con.bat_target = 3;
    commit_bat();
    con.bat_target = 4;
    put16(&con.bat[4], 0xFFFFU);
    commit_bat();

    con.dir_target = 2;
    con.bat_target = 4;
    put16(&con.dir[0x1FFA], 0);
    put16(&con.bat[4], 0);
}
