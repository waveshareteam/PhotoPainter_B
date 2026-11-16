#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// TinyUSB configuration shared by both USB personalities of the firmware:
//  - normal mode: CDC only (USB serial log, handled by pico_stdio_usb)
//  - USB disk mode: MSC only (SD card exposed to the host as a USB drive)
// The descriptors in usb_descriptors.c pick which interfaces are presented.

#define CFG_TUSB_RHPORT0_MODE    (OPT_MODE_DEVICE)

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE   64
#endif

#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              1
#define CFG_TUD_HID              0
#define CFG_TUD_MIDI             0
#define CFG_TUD_VENDOR           0

// CDC FIFO / endpoint sizes (same as the Pico SDK stdio defaults)
#define CFG_TUD_CDC_RX_BUFSIZE   64
#define CFG_TUD_CDC_TX_BUFSIZE   64
#define CFG_TUD_CDC_EP_BUFSIZE   64

// MSC transfer buffer; must be a multiple of the 512-byte SD block size.
// Each read/write callback then handles up to 8 whole blocks at offset 0.
#define CFG_TUD_MSC_EP_BUFSIZE   4096

#endif
