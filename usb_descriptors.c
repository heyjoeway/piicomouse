#include "pico/stdio_usb.h"
#include "pico/stdio_usb/reset_interface.h"
#include "pico/unique_id.h"
#include "tusb.h"

#ifndef USBD_VID
#define USBD_VID 0x2E8A
#endif

#ifndef USBD_PID
#define USBD_PID 0x0009
#endif

#ifndef USBD_MANUFACTURER
#define USBD_MANUFACTURER "Raspberry Pi"
#endif

#ifndef USBD_PRODUCT
#define USBD_PRODUCT "Pico Wii HID Bridge"
#endif

#define TUD_RPI_RESET_DESC_LEN 9
#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_RPI_RESET_DESC_LEN + 5*TUD_HID_DESC_LEN)

#if !PICO_STDIO_USB_DEVICE_SELF_POWERED
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP
#define USBD_MAX_POWER_MA 250
#else
#define USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE (TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP)
#define USBD_MAX_POWER_MA 1
#endif

#define USBD_ITF_CDC 0
#define USBD_ITF_RPI_RESET 2
#define USBD_ITF_HID_POINTER 3
#define USBD_ITF_HID_DIGITIZER 4
#define USBD_ITF_HID_KEYBOARD 5
#define USBD_ITF_HID_CONSUMER_CONTROL 6
#define USBD_ITF_HID_GAMEPAD 7
#define USBD_ITF_MAX 8

#define USBD_CDC_EP_CMD 0x81
#define USBD_CDC_EP_OUT 0x02
#define USBD_CDC_EP_IN 0x82
#define USBD_HID_POINTER_EP_IN 0x83
#define USBD_HID_DIGITIZER_EP_IN 0x84
#define USBD_HID_KEYBOARD_EP_IN 0x85
#define USBD_HID_CONSUMER_CONTROL_EP_IN 0x86
#define USBD_HID_GAMEPAD_EP_IN 0x87

#define USBD_CDC_CMD_MAX_SIZE 8
#define USBD_CDC_IN_OUT_MAX_SIZE 64
#define USBD_HID_EP_SIZE 16
#define USBD_HID_POLL_MS 1

#define USBD_STR_0 0x00
#define USBD_STR_MANUF 0x01
#define USBD_STR_PRODUCT 0x02
#define USBD_STR_SERIAL 0x03
#define USBD_STR_CDC 0x04
#define USBD_STR_RPI_RESET 0x05
#define USBD_STR_HID_POINTER 0x06
#define USBD_STR_HID_DIGITIZER 0x07
#define USBD_STR_HID_KEYBOARD 0x08
#define USBD_STR_HID_CONSUMER_CONTROL 0x09
#define USBD_STR_HID_GAMEPAD 0x0A

#define TUD_RPI_RESET_DESCRIPTOR(_itfnum, _stridx) \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 0, TUSB_CLASS_VENDOR_SPECIFIC, RESET_INTERFACE_SUBCLASS, RESET_INTERFACE_PROTOCOL, _stridx

static const tusb_desc_device_t usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
#if PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE && PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR
    .bcdUSB = 0x0210,
#else
    .bcdUSB = 0x0200,
#endif
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USBD_STR_MANUF,
    .iProduct = USBD_STR_PRODUCT,
    .iSerialNumber = USBD_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t hid_report_desc_pointer[] = {
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
      0x85, 0x01,
      0x05, 0x01,
      0x09, 0x01,
      0xA1, 0x00,
        0x05, 0x09,
        0x19, 0x01,
        0x29, 0x03,
        0x15, 0x00,
        0x25, 0x01,
        0x95, 0x03,
        0x75, 0x01,
        0x81, 0x02,
        0x95, 0x05,
        0x81, 0x01,
        0x05, 0x01,
        0x09, 0x30,
    0x09, 0x31,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x02,
    0x81, 0x06,
      0xC0,
    0xC0
};

static const uint8_t hid_report_desc_digitizer[] = {
    0x05, 0x0D,
    0x09, 0x02,
    0xA1, 0x01,
      0x85, 0x02,
      0x05, 0x0D,
      0x09, 0x42,
      0x09, 0x44,
      0x15, 0x00,
      0x25, 0x01,
      0x75, 0x01,
      0x95, 0x02,
      0x81, 0x02,
      0x95, 0x06,
      0x81, 0x01,
      0x05, 0x0D,
      0x09, 0x30,
      0x09, 0x31,
      0x15, 0x00,
    0x26, 0xE8, 0x03,
      0x95, 0x02,
      0x75, 0x10,
      0x81, 0x02,
    0xC0
};

static const uint8_t hid_report_desc_keyboard[] = {
        0x05, 0x01,
        0x09, 0x06,
        0xA1, 0x01,
            0x85, 0x03,
            0x05, 0x07,
            0x19, 0xE0,
            0x29, 0xE7,
            0x15, 0x00,
            0x25, 0x01,
            0x75, 0x01,
            0x95, 0x08,
            0x81, 0x02,
            0x95, 0x01,
            0x75, 0x08,
            0x81, 0x01,
            0x95, 0x06,
            0x75, 0x08,
            0x15, 0x00,
            0x25, 0xE7,
            0x05, 0x07,
            0x19, 0x00,
            0x29, 0xE7,
            0x81, 0x00,
        0xC0
};

static const uint8_t hid_report_desc_consumer_control[] = {
    0x05, 0x0C,                  // Usage Page (Consumer Devices)
    0x09, 0x01,                  // Usage (Consumer Control)
    0xA1, 0x01,                  // Collection (Application)
      0x85, 0x04,                // Report ID (4)
      0x15, 0x00,                // Logical Minimum (0)
      0x26, 0xFF, 0x03,          // Logical Maximum (1023)
      0x19, 0x00,                // Usage Minimum (0)
      0x2A, 0xFF, 0x03,          // Usage Maximum (1023)
      0x75, 0x10,                // Report Size (16 bits)
      0x95, 0x01,                // Report Count (1)
      0x81, 0x00,                // Input (Data,Array,Absolute)
    0xC0                         // End Collection
};

static const uint8_t hid_report_desc_gamepad[] = {
        0x05, 0x01,                  // Usage Page (Generic Desktop)
        0x09, 0x05,                  // Usage (Game Pad)
        0xA1, 0x01,                  // Collection (Application)
            0x85, 0x05,                // Report ID (5)
            0x05, 0x09,                // Usage Page (Button)
            0x19, 0x01,                // Usage Minimum (Button 1)
            0x29, 0x10,                // Usage Maximum (Button 16)
            0x15, 0x00,                // Logical Minimum (0)
            0x25, 0x01,                // Logical Maximum (1)
            0x75, 0x01,                // Report Size (1)
            0x95, 0x10,                // Report Count (16)
            0x81, 0x02,                // Input (Data,Var,Abs)
            0x05, 0x01,                // Usage Page (Generic Desktop)
            0x09, 0x39,                // Usage (Hat switch)
            0x15, 0x00,                // Logical Minimum (0)
            0x25, 0x07,                // Logical Maximum (7)
            0x35, 0x00,                // Physical Minimum (0)
            0x46, 0x3B, 0x01,          // Physical Maximum (315)
            0x65, 0x14,                // Unit (Eng Rot: Degree)
            0x75, 0x04,                // Report Size (4)
            0x95, 0x01,                // Report Count (1)
            0x81, 0x42,                // Input (Data,Var,Abs,Null)
            0x65, 0x00,                // Unit (None)
            0x75, 0x04,                // Report Size (4)
            0x95, 0x01,                // Report Count (1)
            0x81, 0x01,                // Input (Const,Array,Abs)
            0x09, 0x30,                // Usage (X)
            0x09, 0x31,                // Usage (Y)
            0x09, 0x32,                // Usage (Z)
            0x09, 0x35,                // Usage (Rz)
            0x15, 0x81,                // Logical Minimum (-127)
            0x25, 0x7F,                // Logical Maximum (127)
            0x75, 0x08,                // Report Size (8)
            0x95, 0x04,                // Report Count (4)
            0x81, 0x02,                // Input (Data,Var,Abs)
        0xC0                         // End Collection
};

static const uint8_t usbd_desc_cfg[USBD_DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        USBD_ITF_MAX,
        USBD_STR_0,
        USBD_DESC_LEN,
        USBD_CONFIGURATION_DESCRIPTOR_ATTRIBUTE,
        USBD_MAX_POWER_MA),

    TUD_CDC_DESCRIPTOR(
        USBD_ITF_CDC,
        USBD_STR_CDC,
        USBD_CDC_EP_CMD,
        USBD_CDC_CMD_MAX_SIZE,
        USBD_CDC_EP_OUT,
        USBD_CDC_EP_IN,
        USBD_CDC_IN_OUT_MAX_SIZE),

    TUD_RPI_RESET_DESCRIPTOR(USBD_ITF_RPI_RESET, USBD_STR_RPI_RESET),

    TUD_HID_DESCRIPTOR(
        USBD_ITF_HID_POINTER,
        USBD_STR_HID_POINTER,
        HID_ITF_PROTOCOL_NONE,
        sizeof(hid_report_desc_pointer),
        USBD_HID_POINTER_EP_IN,
        USBD_HID_EP_SIZE,
        USBD_HID_POLL_MS),

    TUD_HID_DESCRIPTOR(
        USBD_ITF_HID_DIGITIZER,
        USBD_STR_HID_DIGITIZER,
        HID_ITF_PROTOCOL_NONE,
        sizeof(hid_report_desc_digitizer),
        USBD_HID_DIGITIZER_EP_IN,
        USBD_HID_EP_SIZE,
        USBD_HID_POLL_MS),

    TUD_HID_DESCRIPTOR(
        USBD_ITF_HID_KEYBOARD,
        USBD_STR_HID_KEYBOARD,
        HID_ITF_PROTOCOL_KEYBOARD,
        sizeof(hid_report_desc_keyboard),
        USBD_HID_KEYBOARD_EP_IN,
        USBD_HID_EP_SIZE,
        USBD_HID_POLL_MS),

    TUD_HID_DESCRIPTOR(
        USBD_ITF_HID_CONSUMER_CONTROL,
        USBD_STR_HID_CONSUMER_CONTROL,
        HID_ITF_PROTOCOL_NONE,
        sizeof(hid_report_desc_consumer_control),
        USBD_HID_CONSUMER_CONTROL_EP_IN,
        USBD_HID_EP_SIZE,
        USBD_HID_POLL_MS),

    TUD_HID_DESCRIPTOR(
        USBD_ITF_HID_GAMEPAD,
        USBD_STR_HID_GAMEPAD,
        HID_ITF_PROTOCOL_NONE,
        sizeof(hid_report_desc_gamepad),
        USBD_HID_GAMEPAD_EP_IN,
        USBD_HID_EP_SIZE,
        USBD_HID_POLL_MS),
};

static char usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

static const char *const usbd_desc_str[] = {
    [USBD_STR_MANUF] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = usbd_serial_str,
    [USBD_STR_CDC] = "Board CDC",
    [USBD_STR_RPI_RESET] = "Reset",
    [USBD_STR_HID_POINTER] = "HID Pointer",
    [USBD_STR_HID_DIGITIZER] = "HID Digitizer",
    [USBD_STR_HID_KEYBOARD] = "HID Keyboard",
    [USBD_STR_HID_CONSUMER_CONTROL] = "HID Consumer Control",
    [USBD_STR_HID_GAMEPAD] = "HID Gamepad",
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&usbd_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return usbd_desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

#ifndef USBD_DESC_STR_MAX
#define USBD_DESC_STR_MAX 20
#endif
    static uint16_t desc_str[USBD_DESC_STR_MAX];

    if (!usbd_serial_str[0]) {
        pico_get_unique_board_id_string(usbd_serial_str, sizeof(usbd_serial_str));
    }

    uint8_t len;
    if (index == 0) {
        desc_str[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(usbd_desc_str) / sizeof(usbd_desc_str[0])) {
            return NULL;
        }
        const char *str = usbd_desc_str[index];
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            desc_str[1 + len] = str[len];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == 0) {
        return hid_report_desc_pointer;
    } else if (instance == 1) {
        return hid_report_desc_digitizer;
    } else if (instance == 2) {
        return hid_report_desc_keyboard;
    } else if (instance == 3) {
        return hid_report_desc_consumer_control;
    } else if (instance == 4) {
        return hid_report_desc_gamepad;
    }
    return NULL;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

bool usb_hid_pointer_ready(void) {
    return tud_hid_n_ready(0);
}

bool usb_hid_digitizer_ready(void) {
    return tud_hid_n_ready(1);
}

bool usb_hid_send_pointer_report(uint8_t buttons, int8_t dx, int8_t dy) {
    uint8_t report[3];

    report[0] = buttons;
    report[1] = (uint8_t)dx;
    report[2] = (uint8_t)dy;

    return tud_hid_n_report(0, 1, report, sizeof(report));
}

bool usb_hid_send_digitizer_report(uint8_t switches, uint16_t x, uint16_t y) {
    uint8_t report[5];

    report[0] = switches;
    report[1] = (uint8_t)(x & 0xFF);
    report[2] = (uint8_t)((x >> 8) & 0xFF);
    report[3] = (uint8_t)(y & 0xFF);
    report[4] = (uint8_t)((y >> 8) & 0xFF);

    return tud_hid_n_report(1, 2, report, sizeof(report));
}

bool usb_hid_keyboard_ready(void) {
    return tud_hid_n_ready(2);
}

bool usb_hid_send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]) {
    uint8_t report[8] = {0};

    report[0] = modifiers;
    if (keycodes) {
        for (int i = 0; i < 6; i++) {
            report[2 + i] = keycodes[i];
        }
    }

    return tud_hid_n_report(2, 3, report, sizeof(report));
}

bool usb_hid_consumer_control_ready(void) {
    return tud_hid_n_ready(3);
}

bool usb_hid_send_consumer_control_report(uint16_t keycode) {
    uint8_t report[2];

    report[0] = (uint8_t)(keycode & 0xFF);
    report[1] = (uint8_t)((keycode >> 8) & 0xFF);

    return tud_hid_n_report(3, 4, report, sizeof(report));
}

bool usb_hid_gamepad_ready(void) {
    return tud_hid_n_ready(4);
}

bool usb_hid_send_gamepad_report(uint16_t buttons, uint8_t hat) {
    // 16 button bits + hat nibble + 4 analog axes (X,Y,Z,Rz)
    uint8_t report[7] = {0};

    report[0] = (uint8_t)(buttons & 0xFFu);
    report[1] = (uint8_t)((buttons >> 8) & 0xFFu);
    report[2] = (uint8_t)(hat & 0x0Fu);
    report[3] = 0;
    report[4] = 0;
    report[5] = 0;
    report[6] = 0;

    return tud_hid_n_report(4, 5, report, sizeof(report));
}
