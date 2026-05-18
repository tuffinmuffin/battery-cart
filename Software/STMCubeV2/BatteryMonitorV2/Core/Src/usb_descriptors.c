/**
 * usb_descriptors.c — TinyUSB descriptor callbacks for BatteryMonitorV2 CDC ACM device.
 *
 * Defines:
 *   - Device descriptor (one configuration)
 *   - Configuration descriptor (one CDC interface association — 2 interfaces, 3 endpoints)
 *   - String descriptors (manufacturer, product, serial)
 *
 * VID/PID note: 0xCafe/0x4001 are TinyUSB's example dev values. Fine for bring-up;
 * swap to a registered VID before any external distribution.
 */

#include "tusb.h"

#define USB_VID                  0xCafe
#define USB_PID                  0x4001
#define USB_BCD                  0x0200    /* USB 2.0 */

/* --------------- Device Descriptor --------------- */

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,      /* IAD for CDC */
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const* tud_descriptor_device_cb(void) {
    return (uint8_t const*) &desc_device;
}

/* --------------- Configuration Descriptor --------------- */

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF     0x81    /* IN  interrupt — control notifications */
#define EPNUM_CDC_OUT       0x02    /* OUT bulk      — host -> device data   */
#define EPNUM_CDC_IN        0x82    /* IN  bulk      — device -> host data   */

static uint8_t const desc_fs_configuration[] = {
    /* Config: bNumInterfaces, bConfigValue, iConfig, attributes, max power(mA/2) */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),

    /* CDC: ifaceNum, strIdx, epNotif, epNotifSize, epOut, epIn, epSize */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_fs_configuration;
}

/* --------------- String Descriptors --------------- */

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_INTERFACE
};

static char const* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  /* 0: supported language — English (0x0409) */
    "Raptacon3200",                        /* 1: Manufacturer */
    "BatteryMonitorV2",              /* 2: Product */
    "BMV2-000001",                   /* 3: Serial (placeholder; can derive from UID later) */
    "BMV2 CDC"                       /* 4: CDC interface name */
};

static uint16_t desc_str[32 + 1];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;

    size_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;

        const char* str = string_desc_arr[index];
        chr_count = strlen(str);
        size_t const max_count = sizeof(desc_str) / sizeof(desc_str[0]) - 1;
        if (chr_count > max_count) chr_count = max_count;

        for (size_t i = 0; i < chr_count; i++) {
            /* Cast through unsigned char so values >0x7F don't sign-extend into uint16_t. */
            desc_str[1 + i] = (unsigned char)str[i];
        }
    }

    desc_str[0] = (TUSB_DESC_STRING << 8) | (uint16_t)(2 * chr_count + 2);
    return desc_str;
}
