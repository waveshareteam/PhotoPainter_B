#ifndef _USB_DISK_H_
#define _USB_DISK_H_

#include <stdbool.h>

// How long NEXT must be held (in the charging loop, or at power-up with USB
// connected) to switch into USB disk mode.
#define USB_DISK_HOLD_MS 3000

typedef enum {
    USB_DISK_BOOT_NORMAL,   // regular photo-frame boot
    USB_DISK_BOOT_ENTER,    // start USB disk mode (call usb_disk_run)
    USB_DISK_BOOT_REFRESH,  // just left USB disk mode: show a picture right away
} usb_disk_boot_t;

// True while running in USB disk mode; selects the USB descriptors.
extern bool usb_disk_active;

// Why the previous run of USB disk mode ended (or a brown-out happened), or
// NULL if there is nothing to report. Valid after usb_disk_boot_check().
const char *usb_disk_last_exit(void);

// Call first thing in main(), before DEV_Module_Init().
usb_disk_boot_t usb_disk_boot_check(void);

// Reboot into USB disk mode (from the normal charging loop).
void usb_disk_request_and_reboot(void) __attribute__((noreturn));

// Run USB disk mode. Never returns: reboots into normal mode when the host
// ejects the drive, NEXT is pressed, USB is unplugged, or no host shows up.
void usb_disk_run(void) __attribute__((noreturn));

#endif
