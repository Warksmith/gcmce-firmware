#include "usb_storage_blocks.h"

#include <string.h>

static uint8_t partial[USB_BLOCK_SIZE];

static bool in_range(const usb_blocks_t *dev, uint32_t lba, uint32_t offset, uint32_t size) {
    uint64_t start = (uint64_t)lba * USB_BLOCK_SIZE + offset;
    uint64_t end = (uint64_t)dev->sectors * USB_BLOCK_SIZE;

    return size > 0 && start + size <= end;
}

static bool read_whole(const usb_blocks_t *dev, uint32_t sector, uint8_t *dst, uint32_t count) {
    usb_ahead_t *ahead = dev->ahead;

    if (ahead == NULL)
        return dev->read(sector, dst, count);

    if (sector >= ahead->first && sector - ahead->first < ahead->count && count <= ahead->count - (sector - ahead->first))
        memcpy(dst, &ahead->buffer[(sector - ahead->first) * USB_BLOCK_SIZE], count * USB_BLOCK_SIZE);
    else if (!dev->read(sector, dst, count))
        return false;

    ahead->wanted = sector == ahead->next;
    ahead->next = sector + count;
    return true;
}

void usb_blocks_read_ahead(const usb_blocks_t *dev) {
    usb_ahead_t *ahead = dev->ahead;
    uint32_t count;

    if (ahead == NULL || !ahead->wanted)
        return;
    ahead->wanted = false;

    /* still holding what comes next, for a host reading in smaller steps than this */
    if (ahead->next >= ahead->first && ahead->next - ahead->first < ahead->count)
        return;
    if (ahead->next >= dev->sectors)
        return;

    count = dev->sectors - ahead->next;
    if (count > ahead->capacity)
        count = ahead->capacity;

    ahead->count = 0;
    if (dev->read(ahead->next, ahead->buffer, count)) {
        ahead->first = ahead->next;
        ahead->count = count;
    }
}

int32_t usb_blocks_read(const usb_blocks_t *dev, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
    uint32_t sector = lba + offset / USB_BLOCK_SIZE;
    uint32_t skip = offset % USB_BLOCK_SIZE;
    uint32_t left = size;

    if (!in_range(dev, lba, offset, size))
        return -1;

    while (left > 0) {
        uint32_t whole = left / USB_BLOCK_SIZE;

        if (skip == 0 && whole > 0) {
            if (!read_whole(dev, sector, buffer, whole))
                return -1;
            sector += whole;
            buffer += whole * USB_BLOCK_SIZE;
            left -= whole * USB_BLOCK_SIZE;
        } else {
            uint32_t part = USB_BLOCK_SIZE - skip;

            if (part > left)
                part = left;
            if (!dev->read(sector, partial, 1))
                return -1;
            memcpy(buffer, &partial[skip], part);
            sector++;
            skip = 0;
            buffer += part;
            left -= part;
        }
    }
    return (int32_t)size;
}

int32_t usb_blocks_write(const usb_blocks_t *dev, uint32_t lba, uint32_t offset, const uint8_t *buffer, uint32_t size) {
    uint32_t sector = lba + offset / USB_BLOCK_SIZE;
    uint32_t skip = offset % USB_BLOCK_SIZE;
    uint32_t left = size;

    if (!in_range(dev, lba, offset, size))
        return -1;

    /* before the card changes, so that a failed write cannot leave old data readable here */
    if (dev->ahead != NULL) {
        dev->ahead->count = 0;
        dev->ahead->wanted = false;
    }

    while (left > 0) {
        uint32_t whole = left / USB_BLOCK_SIZE;

        if (skip == 0 && whole > 0) {
            if (!dev->write(sector, buffer, whole))
                return -1;
            sector += whole;
            buffer += whole * USB_BLOCK_SIZE;
            left -= whole * USB_BLOCK_SIZE;
        } else {
            uint32_t part = USB_BLOCK_SIZE - skip;

            if (part > left)
                part = left;
            /* the rest of the sector keeps what it holds */
            if (!dev->read(sector, partial, 1))
                return -1;
            memcpy(&partial[skip], buffer, part);
            if (!dev->write(sector, partial, 1))
                return -1;
            sector++;
            skip = 0;
            buffer += part;
            left -= part;
        }
    }
    return (int32_t)size;
}

void usb_release_reset(usb_release_t *r) {
    memset(r, 0, sizeof(*r));
}

void usb_release_stop(usb_release_t *r, bool load_eject) {
    r->stopped = true;
    if (load_eject)
        r->ejected = true;
}

void usb_release_start(usb_release_t *r) {
    r->ejected = false;
    r->stopped = false;
}

/* Reading or writing means the host has not finished, whatever it said before. */
void usb_release_access(usb_release_t *r) {
    r->stopped = false;
}

void usb_release_unconfigured(usb_release_t *r) {
    r->ejected = true;
}

void usb_release_suspend(usb_release_t *r, uint64_t now_us) {
    r->suspended = true;
    r->suspended_at_us = now_us;
}

void usb_release_resume(usb_release_t *r) {
    r->suspended = false;
}

bool usb_release_medium_gone(const usb_release_t *r) {
    return r->ejected;
}

bool usb_release_safe(const usb_release_t *r, uint64_t now_us) {
    return r->ejected || r->stopped || (r->suspended && now_us - r->suspended_at_us >= USB_RELEASE_IDLE_US);
}
