#include "usb_storage.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pico/time.h"
#include "sd.h"
#include "tusb.h"
#include "usb_storage_blocks.h"

#if WITH_GUI
#include "oled.h"
#endif

/* The USB stack in this firmware belongs to pico_stdio_usb, which brings its own CDC
   descriptors. Storage mode keeps that code in place and replaces only the descriptors,
   through the --wrap link options in CMakeLists.txt. Normal boots never set
   usb_storage_active, and the USB stack only starts here or in debug builds. */

#define STORAGE_VID 0xCAFE /* TinyUSB's test VID, with its PID for an MSC-only device */
#define STORAGE_PID 0x4002

#define STORAGE_ITF 0
#define STORAGE_EP_OUT 0x01
#define STORAGE_EP_IN 0x81
#define STORAGE_EP_SIZE 64
#define STORAGE_MAX_POWER_MA 250
#define STORAGE_CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

/* indexes of the strings in pico_stdio_usb's table, which is used as it is */
#define STORAGE_STR_MANUF 1
#define STORAGE_STR_PRODUCT 2
#define STORAGE_STR_SERIAL 3

#define SCSI_CMD_PREVENT_ALLOW_REMOVAL 0x1E
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35

/* additional sense codes */
#define SENSE_WRITE_FAULT 0x03
#define SENSE_SYNC_FAILED 0x0C
#define SENSE_READ_ERROR 0x11
#define SENSE_BAD_COMMAND 0x20
#define SENSE_NO_MEDIUM 0x3A

typedef enum {
    STATUS_NONE,
    STATUS_WAITING,
    STATUS_READY,
    STATUS_SAFE,
    STATUS_SD_ERROR
} storage_status_t;

static const tusb_desc_device_t storage_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = STORAGE_VID,
    .idProduct = STORAGE_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STORAGE_STR_MANUF,
    .iProduct = STORAGE_STR_PRODUCT,
    .iSerialNumber = STORAGE_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t storage_desc_cfg[STORAGE_CFG_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, STORAGE_CFG_LEN, 0, STORAGE_MAX_POWER_MA),
    TUD_MSC_DESCRIPTOR(STORAGE_ITF, 0, STORAGE_EP_OUT, STORAGE_EP_IN, STORAGE_EP_SIZE),
};

static bool usb_storage_active;
static usb_release_t host;
static usb_blocks_t sd_blocks = { .read = sd_raw_read, .write = sd_raw_write };

#if DEBUG_USB_UART
/* Storage mode replaces the debug console, so in a debug build the display shows which of
   the host's signals arrived, in case a host releases the card in some other way.
   L: stop and unload  S: stop  A: allow removal  Y: sync  Z: suspend  U: unconfigured */
static char host_seen[8];

static void note_host(char event) {
    size_t len = strlen(host_seen);

    if (!strchr(host_seen, event) && len + 1 < sizeof(host_seen))
        host_seen[len] = event;
}
#else
#define note_host(event) ((void)0)
#endif

const uint8_t *__real_tud_descriptor_device_cb(void);
const uint8_t *__real_tud_descriptor_configuration_cb(uint8_t index);

const uint8_t *__wrap_tud_descriptor_device_cb(void) {
    if (usb_storage_active)
        return (const uint8_t *)&storage_desc_device;
    return __real_tud_descriptor_device_cb();
}

const uint8_t *__wrap_tud_descriptor_configuration_cb(uint8_t index) {
    if (usb_storage_active)
        return storage_desc_cfg;
    return __real_tud_descriptor_configuration_cb(index);
}

static void copy_padded(uint8_t *dst, const char *src, size_t size) {
    size_t len = strlen(src);

    memset(dst, ' ', size);
    memcpy(dst, src, len < size ? len : size);
}

static bool medium_ready(void) {
    return sd_blocks.sectors != 0 && !usb_release_medium_gone(&host);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;

    copy_padded(vendor_id, "BitFunX", 8);
    copy_padded(product_id, "FlipperMCE SD", 16);
    copy_padded(product_rev, "1.0", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!medium_ready()) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, SENSE_NO_MEDIUM, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;

    *block_count = sd_blocks.sectors;
    *block_size = USB_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    /* a power condition asks for a power state, and start means nothing with it */
    if (power_condition != 0)
        return true;

    if (start) {
        usb_release_start(&host);
        return true;
    }

    if (sd_blocks.sectors != 0 && !sd_raw_sync()) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SENSE_SYNC_FAILED, 0x00);
        return false;
    }
    note_host(load_eject ? 'L' : 'S');
    usb_release_stop(&host, load_eject);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    int32_t done = medium_ready() ? usb_blocks_read(&sd_blocks, lba, offset, buffer, bufsize) : -1;

    usb_release_access(&host);
    if (done < 0)
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SENSE_READ_ERROR, 0x00);
    return done;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    int32_t done = medium_ready() ? usb_blocks_write(&sd_blocks, lba, offset, buffer, bufsize) : -1;

    usb_release_access(&host);
    if (done < 0)
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SENSE_WRITE_FAULT, 0x00);
    return done;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_REMOVAL:
            if (!(scsi_cmd[4] & 1))
                note_host('A');
            return 0;
        case SCSI_CMD_SYNCHRONIZE_CACHE_10:
            note_host('Y');
            if (sd_raw_sync())
                return 0;
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, SENSE_SYNC_FAILED, 0x00);
            return -1;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, SENSE_BAD_COMMAND, 0x00);
            return -1;
    }
}

void tud_mount_cb(void) {
    usb_release_reset(&host);
}

void tud_umount_cb(void) {
    note_host('U');
    usb_release_unconfigured(&host);
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;

    note_host('Z');
    usb_release_suspend(&host, time_us_64());
}

void tud_resume_cb(void) {
    usb_release_resume(&host);
}

static storage_status_t get_status(void) {
    if (sd_blocks.sectors == 0)
        return STATUS_SD_ERROR;
    if (usb_release_safe(&host, time_us_64()))
        return STATUS_SAFE;
    return tud_mounted() ? STATUS_READY : STATUS_WAITING;
}

static void show_status(storage_status_t status) {
#if WITH_GUI
    oled_clear();
    oled_draw_text("USB STORAGE\n\n");
    if (status == STATUS_SD_ERROR)
        oled_draw_text("SD Error\nCheck the card\nand reconnect");
    else if (status == STATUS_WAITING)
        oled_draw_text("Waiting for PC");
    else if (status == STATUS_READY)
        oled_draw_text("Connected\nEject before\nunplugging");
    else
        oled_draw_text("Safe to\ndisconnect");
#if DEBUG_USB_UART
    oled_draw_text("\n");
    oled_draw_text(host_seen);
#endif
    oled_show();
#else
    (void)status;
#endif
}

void usb_storage_run(void) {
    storage_status_t shown = STATUS_NONE;

#if WITH_GUI
    oled_init();
#endif

    sd_blocks.sectors = sd_raw_init();
    usb_storage_active = true;
    tusb_init();

    while (true) {
        tud_task();

        /* the display blocks while it updates, so only touch it when the text changes */
        storage_status_t status = get_status();
#if DEBUG_USB_UART
        static size_t shown_seen;
        if (strlen(host_seen) != shown_seen) {
            shown_seen = strlen(host_seen);
            shown = STATUS_NONE;
        }
#endif
        if (status != shown) {
            show_status(status);
            shown = status;
        }
    }
}
