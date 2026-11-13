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

/* Whether the host is done with the card. Hosts say so in different ways, and some never
   do: one that has safely removed the drive can just stop talking to it. */
#define USB_RELEASE_IDLE_US 2000000U /* a suspended bus can be a moment's pause */

typedef struct {
    bool ejected;       /* told to unload, or unconfigured: the medium stays away */
    bool stopped;       /* told to stop: the medium stays, if it is asked for */
    bool suspended;
    uint64_t suspended_at_us;
} usb_release_t;

void usb_release_reset(usb_release_t *r);
void usb_release_stop(usb_release_t *r, bool load_eject);
void usb_release_start(usb_release_t *r);
void usb_release_access(usb_release_t *r);
void usb_release_unconfigured(usb_release_t *r);
void usb_release_suspend(usb_release_t *r, uint64_t now_us);
void usb_release_resume(usb_release_t *r);

/* True if the medium must be reported as absent. */
bool usb_release_medium_gone(const usb_release_t *r);
/* True if unplugging now is safe as far as the device can tell. */
bool usb_release_safe(const usb_release_t *r, uint64_t now_us);
