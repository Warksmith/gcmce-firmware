#pragma once

/* Maps USB mass storage transfers onto whole SD sectors. Separate from usb_storage.c so
   that the host tests can run it without a USB stack. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_BLOCK_SIZE 512U

typedef struct {
    uint32_t sectors;
    bool (*read)(uint32_t sector, uint8_t *dst, size_t count);
    bool (*write)(uint32_t sector, const uint8_t *src, size_t count);
} usb_blocks_t;

/* Both take a transfer at offset bytes into sector lba and return size, or -1 if it reaches
   past the end of the card or the card reports an error. */
int32_t usb_blocks_read(const usb_blocks_t *dev, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size);
int32_t usb_blocks_write(const usb_blocks_t *dev, uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size);
