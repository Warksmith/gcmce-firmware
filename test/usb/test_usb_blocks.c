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

int main(void) {
    test_aligned();
    test_unaligned();
    test_bounds();
    test_failures();
    puts("usb blocks: all passed");
    return 0;
}
