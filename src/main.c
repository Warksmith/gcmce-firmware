#include <hardware/gpio.h>
#include <stdio.h>
#include "hardware/watchdog.h"

#include "mmceman/gc_mmceman.h"
#include "pico/stdio.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"

#if WITH_GUI
#include "oled.h"
#include "gui.h"
#endif
#include "input.h"
#include "config.h"
#include "debug.h"
#include "pico/time.h"
#include "sd.h"
#include "settings.h"
#include "usb_storage.h"
#include "version/version.h"
#include "psram/psram.h"

#include "card_emu/gc_memory_card.h"
//#include "mmceman/gc_mmceman.h"
//#include "mmceman/gc_mmceman_commands.h"
#include "gc_cardman.h"
#include "gc.h"

#include "game_db/game_db.h"

/* boot buttons, as indexes for input_is_down_raw() */
#define BOOT_BTN_USB_STORAGE 0
#define BOOT_BTN_BOOTLOADER 1

/* reboot to bootloader if button 2 is held on startup to make the device
   easier to flash when assembled inside case. returns true if button 1 is
   held, which selects USB storage mode. button 2 wins if both are held */
static bool check_boot_buttons(void) {
    bool usb_storage = true;

    /* make sure at least DEBOUNCE interval passes or we won't get inputs */
    for (int i = 0; i < 2 * DEBOUNCE_MS; ++i) {
        input_task();
        sleep_ms(1);
        if (i >= DEBOUNCE_MS && !input_is_down_raw(BOOT_BTN_USB_STORAGE))
            usb_storage = false;
    }

    if (input_is_down_raw(BOOT_BTN_BOOTLOADER))
        reset_usb_boot(0, 0);

    return usb_storage;
}

static void debug_task(void) {

    for (int i = 0; i < 10; ++i) {
        char ch = debug_get();
        if (ch) {
            #if DEBUG_USB_UART
                putchar(ch);
            #else
                if (ch == '\n')
                    uart_putc_raw(UART_PERIPH, '\r');
                uart_putc_raw(UART_PERIPH, ch);
            #endif
        } else {
            break;
        }
    }
#if DEBUG_USB_UART
    int charin = getchar_timeout_us(0);
    if ((charin != PICO_ERROR_TIMEOUT) && (charin > 0x20) && (charin < 0x7A)) {
        QPRINTF("Got %c Input\n", charin);

        char in[3] = {0};
        in[0] = (char)charin;
        in[1] = (char)getchar_timeout_us(1000*1000*3);
        in[2] = (char)getchar_timeout_us(1000*1000*3);
        if (in[0] == 'b') {
            if ((in[1] == 'l') && (in[2] == 'r')) {
                QPRINTF("Resetting to Bootloader");
                reset_usb_boot(0, 0);
            }
        } else if (in[0] == 'r') {
            if ((in[1] == 'r') && (in[2] == 'r')) {
                QPRINTF("Resetting");
                watchdog_reboot(0, 0, 0);
            }
        }
        else if (in[0] == 'c') {
            if ((in[1] == 'h') && (in[2] == '+')) {
                DPRINTF("Received Channel Up!\n");
                gc_mmceman_next_ch(false);
            } else if ((in[1] == 'h') && (in[2] == '-')) {
                DPRINTF("Received Channel Down!\n");
                gc_mmceman_prev_ch(false);
            } else if (in[1] == '+') {
                gc_mmceman_next_idx(false);
            } else if (in[1] == '-') {
               gc_mmceman_prev_idx(false);
            }
        }
    }
#endif
}



int main() {
    input_init();
    bool usb_storage = check_boot_buttons();

    printf("prepare...\n");

    uint32_t mhz = 240;

    set_sys_clock_khz(mhz * 1000U, true);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, mhz * 1000000, mhz * 1000000);

#if DEBUG_USB_UART
    /* storage mode takes the USB port for itself */
    if (!usb_storage)
        stdio_usb_init();
#else
    //stdio_uart_init_full(UART_PERIPH, UART_BAUD, UART_TX, UART_RX);
#endif

    /* set up core1 as high priority bus access */
    bus_ctrl_hw->priority |= BUSCTRL_BUS_PRIORITY_PROC1_BITS;
    while (!bus_ctrl_hw->priority_ack) {}

    printf("\n\n\nStarted! Clock %d; bus priority 0x%X\n", (int)clock_get_hz(clk_sys), (unsigned)bus_ctrl_hw->priority);
    printf("FlipperMCE Version %s\n", flippermce_version);
    printf("FlipperMCE HW Variant: %s\n", flippermce_variant);

    /* the SD card belongs to the host in this mode, so nothing here may mount it */
    if (usb_storage) {
        settings_load_flash();
        usb_storage_run();
    }

    settings_init();

    psram_init();
#if !FLIPPER
    game_db_init();
#endif


    while (1) {
        gc_init();
        while (1) {
            debug_task();
            if (!gc_task())
                break;
        }
        gc_deinit();
    }
}
