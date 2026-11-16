// USB descriptors for the two USB personalities of the firmware.
//
// Normal mode mirrors the Pico SDK's default stdio descriptors exactly
// (same VID/PID/strings), so the host keeps treating it as the same
// USB serial device as before.
//
// USB disk mode presents a single Mass Storage interface with its own
// VID/PID, so the host does not confuse it with the serial device.

#include <string.h>

#include "tusb.h"
#include "pico/unique_id.h"
#include "usb_disk.h"

#define STR_LANG     0
#define STR_MANUF    1
#define STR_PRODUCT  2
#define STR_SERIAL   3
#define STR_ITF      4

//--------------------------------------------------------------------+
// Normal mode: CDC (USB serial)
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device_cdc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x2E8A,     // Raspberry Pi
    .idProduct = 0x0009,    // Raspberry Pi Pico SDK CDC
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CDC_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_config_cdc[CDC_CONFIG_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CDC_CONFIG_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(0, STR_ITF, 0x81, 8, 0x02, 0x82, 64),
};

static const char *const strings_cdc[] = {
    [STR_MANUF] = "Raspberry Pi",
    [STR_PRODUCT] = "Pico",
    [STR_SERIAL] = NULL,    // filled with the board's unique id
    [STR_ITF] = "Board CDC",
};

//--------------------------------------------------------------------+
// USB disk mode: MSC (SD card as a USB drive)
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device_msc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,   // class defined per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCAFE,     // TinyUSB test VID
    .idProduct = 0x4008,    // TinyUSB-style PID for an MSC-only device
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define MSC_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_config_msc[MSC_CONFIG_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CONFIG_LEN, 0, 250),
    TUD_MSC_DESCRIPTOR(0, STR_ITF, 0x01, 0x81, 64),
};

static const char *const strings_msc[] = {
    [STR_MANUF] = "PhotoPainter",
    [STR_PRODUCT] = "PhotoPainter SD",
    [STR_SERIAL] = NULL,
    [STR_ITF] = "SD Card",
};

//--------------------------------------------------------------------+
// TinyUSB descriptor callbacks
//--------------------------------------------------------------------+
const uint8_t *tud_descriptor_device_cb(void)
{
    return usb_disk_active ? (const uint8_t *)&desc_device_msc
                           : (const uint8_t *)&desc_device_cdc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return usb_disk_active ? desc_config_msc : desc_config_cdc;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t desc_str[32];
    static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

    const char *const *strings = usb_disk_active ? strings_msc : strings_cdc;
    size_t count = usb_disk_active ? TU_ARRAY_SIZE(strings_msc) : TU_ARRAY_SIZE(strings_cdc);
    uint8_t len;

    if (index == STR_LANG) {
        desc_str[1] = 0x0409;   // English
        len = 1;
    } else {
        if (index >= count) {
            return NULL;
        }
        const char *str = strings[index];
        if (index == STR_SERIAL) {
            if (!serial[0]) {
                pico_get_unique_board_id_string(serial, sizeof(serial));
            }
            str = serial;
        }
        for (len = 0; len < TU_ARRAY_SIZE(desc_str) - 1 && str[len]; ++len) {
            desc_str[1 + len] = (uint8_t)str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
