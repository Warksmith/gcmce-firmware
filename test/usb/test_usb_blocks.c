/* The transfer logic of USB storage mode, against a simulated card. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_storage_blocks.h"

#define SECTORS 64U

static uint8_t disk[SECTORS * USB_BLOCK_SIZE];
static int fail_after = -1;
static unsigned reads, writes;

static bool consume(void) {
    if (fail_after == 0)
        return false;
    if (fail_after > 0)
        fail_after--;
    return true;
}

static bool sim_read(uint32_t sector, uint8_t *dst, size_t count) {
    assert(sector + count <= SECTORS);
    reads++;
    if (!consume())
        return false;
    memcpy(dst, &disk[sector * USB_BLOCK_SIZE], count * USB_BLOCK_SIZE);
    return true;
}

static bool sim_write(uint32_t sector, const uint8_t *src, size_t count) {
    assert(sector + count <= SECTORS);
    writes++;
    if (!consume())
        return false;
    memcpy(&disk[sector * USB_BLOCK_SIZE], src, count * USB_BLOCK_SIZE);
    return true;
}

static const usb_blocks_t dev = { SECTORS, sim_read, sim_write };

static void reset(void) {
    for (size_t i = 0; i < sizeof(disk); i++)
        disk[i] = (uint8_t)(i * 31U + (i >> 9));
    fail_after = -1;
    reads = writes = 0;
}

static void test_aligned(void) {
    static uint8_t buf[USB_BLOCK_SIZE * 8];

    reset();
    assert(usb_blocks_read(&dev, 3, 0, buf, sizeof(buf)) == (int32_t)sizeof(buf));
    assert(memcmp(buf, &disk[3 * USB_BLOCK_SIZE], sizeof(buf)) == 0);
    assert(reads == 1);

    memset(buf, 0x5A, sizeof(buf));
    assert(usb_blocks_write(&dev, 10, 0, buf, sizeof(buf)) == (int32_t)sizeof(buf));
    assert(memcmp(buf, &disk[10 * USB_BLOCK_SIZE], sizeof(buf)) == 0);
    assert(writes == 1);
}

static void test_unaligned(void) {
    static uint8_t expect[sizeof(disk)];
    static uint8_t buf[3000];

    for (uint32_t offset = 0; offset < 1100; offset += 137) {
        for (uint32_t size = 1; size < sizeof(buf); size += 251) {
            uint32_t pos = 2 * USB_BLOCK_SIZE + offset;

            reset();
            memcpy(expect, disk, sizeof(disk));
            for (uint32_t i = 0; i < size; i++)
                buf[i] = (uint8_t)~i;
            memcpy(&expect[pos], buf, size);

            assert(usb_blocks_write(&dev, 2, offset, buf, size) == (int32_t)size);
            assert(memcmp(disk, expect, sizeof(disk)) == 0);

            memset(buf, 0, size);
            assert(usb_blocks_read(&dev, 2, offset, buf, size) == (int32_t)size);
            assert(memcmp(buf, &expect[pos], size) == 0);
        }
    }
}

static void test_bounds(void) {
    static uint8_t buf[USB_BLOCK_SIZE * 2];

    reset();
    assert(usb_blocks_read(&dev, SECTORS - 1, 0, buf, USB_BLOCK_SIZE) == (int32_t)USB_BLOCK_SIZE);
    assert(usb_blocks_read(&dev, SECTORS - 1, 0, buf, sizeof(buf)) == -1);
    assert(usb_blocks_read(&dev, SECTORS, 0, buf, USB_BLOCK_SIZE) == -1);
    assert(usb_blocks_write(&dev, SECTORS - 1, 1, buf, USB_BLOCK_SIZE) == -1);
    assert(usb_blocks_write(&dev, 0xFFFFFFFFU, 0, buf, USB_BLOCK_SIZE) == -1);
    assert(usb_blocks_read(&dev, 0xFFFFFFFFU, 0xFFFFFFFFU, buf, sizeof(buf)) == -1);

    static const usb_blocks_t none = { 0, sim_read, sim_write };

    assert(usb_blocks_read(&none, 0, 0, buf, USB_BLOCK_SIZE) == -1);
    assert(usb_blocks_write(&none, 0, 0, buf, USB_BLOCK_SIZE) == -1);
}

static void test_failures(void) {
    static uint8_t expect[sizeof(disk)];
    static uint8_t buf[USB_BLOCK_SIZE * 3];

    reset();
    fail_after = 0;
    assert(usb_blocks_read(&dev, 0, 0, buf, sizeof(buf)) == -1);

    fail_after = 0;
    assert(usb_blocks_write(&dev, 0, 0, buf, sizeof(buf)) == -1);

    /* a failed read of a partial sector must not write anything back */
    reset();
    memcpy(expect, disk, sizeof(disk));
    fail_after = 0;
    assert(usb_blocks_write(&dev, 1, 100, buf, 50) == -1);
    assert(writes == 0);
    assert(memcmp(disk, expect, sizeof(disk)) == 0);
}

#define AHEAD_SECTORS 4U

static uint8_t ahead_buffer[AHEAD_SECTORS * USB_BLOCK_SIZE];
static usb_ahead_t ahead;
static const usb_blocks_t ahead_dev = { SECTORS, sim_read, sim_write, &ahead };

static void reset_ahead(void) {
    reset();
    memset(&ahead, 0, sizeof(ahead));
    ahead.buffer = ahead_buffer;
    ahead.capacity = AHEAD_SECTORS;
    ahead.next = USB_AHEAD_NO_SECTOR;
}

static void read_expect(uint32_t lba, uint32_t count) {
    static uint8_t buf[USB_BLOCK_SIZE * 8];
    uint32_t size = count * USB_BLOCK_SIZE;

    assert(usb_blocks_read(&ahead_dev, lba, 0, buf, size) == (int32_t)size);
    assert(memcmp(buf, &disk[lba * USB_BLOCK_SIZE], size) == 0);
}

static void test_ahead_in_order(void) {
    /* the first read only shows where the host is: nothing suggests it will carry on */
    reset_ahead();
    read_expect(0, 4);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 1);

    /* from the second, each request is answered from what was read after the last one */
    read_expect(4, 4);
    assert(reads == 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);
    read_expect(8, 4);
    assert(reads == 3);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 4);
    read_expect(12, 4);
    assert(reads == 4);

    /* calling again without a new request does nothing */
    usb_blocks_read_ahead(&ahead_dev);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 5);
}

static void test_ahead_small_steps(void) {
    reset_ahead();
    read_expect(0, 1);
    read_expect(1, 1);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);

    /* sectors 2 to 5 are held: no card access until the host reaches the end of them */
    for (uint32_t i = 2; i < 5; i++) {
        read_expect(i, 1);
        usb_blocks_read_ahead(&ahead_dev);
        assert(reads == 3);
    }
    read_expect(5, 1);
    assert(reads == 3);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 4);
    read_expect(6, 1);
    assert(reads == 4);
}

static void test_ahead_out_of_order(void) {
    /* a host jumping around gets no read-ahead, which would only delay its next request */
    reset_ahead();
    read_expect(10, 2);
    usb_blocks_read_ahead(&ahead_dev);
    read_expect(30, 2);
    usb_blocks_read_ahead(&ahead_dev);
    read_expect(5, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);

    /* a request only partly held goes to the card, and still returns the right data */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);
    read_expect(6, 4);
    assert(reads == 4);
    read_expect(3, 8);
    assert(reads == 5);
}

static void test_ahead_end_of_card(void) {
    reset_ahead();
    read_expect(SECTORS - 6, 2);
    read_expect(SECTORS - 4, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);
    read_expect(SECTORS - 2, 2);
    assert(reads == 3);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);

    /* fewer sectors left than the buffer holds */
    reset_ahead();
    read_expect(SECTORS - 5, 1);
    read_expect(SECTORS - 4, 1);
    assert(reads == 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 3);
    assert(ahead.count == 3);
}

static void test_ahead_writes(void) {
    static uint8_t buf[USB_BLOCK_SIZE * 2];

    /* a write must never leave old sectors to be served */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    usb_blocks_read_ahead(&ahead_dev);
    memset(buf, 0xA5, sizeof(buf));
    assert(usb_blocks_write(&ahead_dev, 4, 0, buf, sizeof(buf)) == (int32_t)sizeof(buf));
    read_expect(4, 2);
    assert(reads == 4);

    /* nor may one that failed part way: the card may hold either version */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(ahead.count != 0);
    fail_after = 0;
    assert(usb_blocks_write(&ahead_dev, 4, 0, buf, sizeof(buf)) == -1);
    fail_after = -1;
    assert(ahead.count == 0);

    /* read-ahead requested before a write is dropped with it */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    assert(usb_blocks_write(&ahead_dev, 20, 0, buf, sizeof(buf)) == (int32_t)sizeof(buf));
    usb_blocks_read_ahead(&ahead_dev);
    assert(reads == 2);

    /* a partial write goes through the same path */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(usb_blocks_write(&ahead_dev, 4, 100, buf, 50) == 50);
    read_expect(4, 1);
}

static void test_ahead_failure(void) {
    static uint8_t buf[USB_BLOCK_SIZE * 2];

    /* a failed read-ahead reports nothing and keeps nothing, and the host's read finds out */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    fail_after = 0;
    usb_blocks_read_ahead(&ahead_dev);
    assert(ahead.count == 0);
    assert(usb_blocks_read(&ahead_dev, 4, 0, buf, sizeof(buf)) == -1);

    /* and once the card works again, so does reading */
    fail_after = -1;
    read_expect(4, 2);
}

static void test_ahead_unaligned(void) {
    static uint8_t buf[USB_BLOCK_SIZE * 3];

    /* a transfer starting inside a sector still returns the right bytes */
    reset_ahead();
    read_expect(0, 2);
    read_expect(2, 2);
    usb_blocks_read_ahead(&ahead_dev);
    assert(usb_blocks_read(&ahead_dev, 4, 100, buf, sizeof(buf)) == (int32_t)sizeof(buf));
    assert(memcmp(buf, &disk[4 * USB_BLOCK_SIZE + 100], sizeof(buf)) == 0);
}

static void test_release(void) {
    usb_release_t r;

    usb_release_reset(&r);
    assert(!usb_release_safe(&r, 0));
    assert(!usb_release_medium_gone(&r));

    /* stop and unload: gone until the host loads it again */
    usb_release_stop(&r, true);
    assert(usb_release_safe(&r, 0));
    assert(usb_release_medium_gone(&r));
    usb_release_start(&r);
    assert(!usb_release_safe(&r, 0));
    assert(!usb_release_medium_gone(&r));

    /* stop alone: the drive is still there, for a host that was only powering it down */
    usb_release_stop(&r, false);
    assert(usb_release_safe(&r, 0));
    assert(!usb_release_medium_gone(&r));
    usb_release_access(&r);
    assert(!usb_release_safe(&r, 0));

    /* a host that just goes silent: not at once, since a pause looks the same at first */
    usb_release_suspend(&r, 1000);
    assert(!usb_release_safe(&r, 1000));
    assert(!usb_release_safe(&r, 1000 + USB_RELEASE_IDLE_US - 1));
    assert(usb_release_safe(&r, 1000 + USB_RELEASE_IDLE_US));
    assert(!usb_release_medium_gone(&r));
    usb_release_resume(&r);
    assert(!usb_release_safe(&r, 1000 + USB_RELEASE_IDLE_US));

    /* an access does not undo an eject */
    usb_release_stop(&r, true);
    usb_release_access(&r);
    assert(usb_release_safe(&r, 0));

    usb_release_reset(&r);
    usb_release_unconfigured(&r);
    assert(usb_release_safe(&r, 0));
    assert(usb_release_medium_gone(&r));
}

int main(void) {
    test_release();
    test_aligned();
    test_unaligned();
    test_bounds();
    test_failures();
    test_ahead_in_order();
    test_ahead_small_steps();
    test_ahead_out_of_order();
    test_ahead_end_of_card();
    test_ahead_writes();
    test_ahead_failure();
    test_ahead_unaligned();
    puts("usb blocks: all passed");
    return 0;
}
