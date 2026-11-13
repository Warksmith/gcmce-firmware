#pragma once

/* Exposes the whole microSD card to a computer as a removable drive. Never returns: the
   device is power cycled to leave this mode. */
void usb_storage_run(void);
