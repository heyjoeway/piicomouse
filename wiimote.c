#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#include "btstack_config.h"
#include "btstack.h"
#include "hid.h"
#include "wiimote.h"

bool tud_suspended(void);
bool tud_remote_wakeup(void);
bool tud_mounted(void);

#define INQUIRY_SECONDS 5
#define CONNECT_RETRY_MS 1500
#define BUTTON_PRINT_PERIOD_MS 200
#define BOOTSEL_POLL_PERIOD_MS 1000
#define SYNC_MODE_TIMEOUT_MS 60000
#define MAX_ATTRIBUTE_VALUE_SIZE 300
#define INACTIVITY_TIMEOUT_MS 300000
#define WIIMOTE_BUTTON_MASK 0x1F9Fu
#define INACTIVITY_IR_MOVE_THRESHOLD 12
#define RECONNECT_COOLDOWN_MS 1000
#define DISCONNECT_REASON_SETTLE_MS 300
#define WIIMOTE_LED_RIGHTMOST_MASK 0x80
#define BT_RESET_POWER_ON_DELAY_MS 600
#define TARGET_ADDR_STORE_MAGIC 0x574D4143u
#define TARGET_ADDR_STORE_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define WIIMOTE_DISCONNECT_REASON_REMOTE_USER_TERMINATED 0x13
#define WIIMOTE_DISCONNECT_REASON_POWER_OFF 0x15

static const char *nintendo_addr_prefixes[] = {
    "98:B6:E9",
    "7C:BB:8A",
    "40:D2:8A",
    "CC:FB:65",
    "B8:AE:6E",
    "9C:E6:35",
    "18:2A:7B",
    "8C:CD:E8",
    "34:AF:2C",
    "40:F4:07",
    "2C:10:C1",
    "58:BD:A3",
    "E0:0C:7F",
    "A4:C0:E1",
    "CC:9E:00",
    "D8:6B:F7",
    "A4:5C:27",
    "78:A2:A0",
    "8C:56:C5",
    "E0:E7:51",
    "E8:4E:CE",
    "00:27:09",
    "00:26:59",
    "00:25:A0",
    "00:24:F3",
    "00:24:44",
    "00:24:1E",
    "00:23:31",
    "00:23:CC",
    "00:22:D7",
    "00:22:AA",
    "00:22:4C",
    "00:21:BD",
    "00:21:47",
    "00:1F:C5",
    "00:1F:32",
    "00:1E:A9",
    "00:1E:35",
    "00:1D:BC",
    "00:1C:BE",
    "00:1B:EA",
    "00:1B:7A",
    "00:1A:E9",
    "00:19:FD",
    "00:19:1D",
    "00:17:AB",
    "00:16:56",
    "00:09:BF"
};

typedef struct {
    uint32_t magic;
    uint8_t addr[6];
    uint8_t reserved[2];
    uint32_t checksum;
} target_addr_store_t;

static const char *wii_name_prefix = "Nintendo RVL-CNT-01";

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t button_print_timer;
static btstack_timer_source_t bootsel_poll_timer;
static btstack_timer_source_t ir_init_timer;
static btstack_timer_source_t connect_retry_timer;
static btstack_timer_source_t inactivity_timer;
static btstack_timer_source_t reconnect_cooldown_timer;
static btstack_timer_source_t bt_reset_timer;
static btstack_timer_source_t sync_mode_timer;
static uint8_t hid_descriptor_storage[MAX_ATTRIBUTE_VALUE_SIZE];

static hid_protocol_mode_t hid_host_report_mode = HID_PROTOCOL_MODE_REPORT;
static uint16_t hid_host_cid;
static bool pending_connect;
static bool reconnect_cooldown_active;
static bool inactivity_disconnect_requested;
static bool passive_reconnect_mode;
static bool bt_reset_in_progress;
static bool bt_reset_waiting_for_power_on;
static bool sync_mode_active;
static hci_con_handle_t wiimote_con_handle = HCI_CON_HANDLE_INVALID;
static uint8_t wiimote_last_disconnect_reason = 0x00;
static wiimote_tracking_state_t *g_wiimote_state = NULL;
static hid_state_t *g_hid_state = NULL;
static uint16_t inactivity_prev_buttons = 0;
static uint16_t prev_raw_buttons = 0;
static bool inactivity_prev_have_norm = false;
static uint16_t inactivity_prev_norm_x = 0;
static uint16_t inactivity_prev_norm_y = 0;
static bool onboard_led_on = false;
static bool bootsel_prev_pressed = false;

static void set_onboard_led(bool on) {
    if (onboard_led_on == on) {
        return;
    }
    onboard_led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
}

static uint32_t target_addr_checksum(const uint8_t addr[6]) {
    uint32_t checksum = TARGET_ADDR_STORE_MAGIC;
    for (int i = 0; i < 6; i++) {
        checksum = (checksum << 5) ^ (checksum >> 2) ^ addr[i];
    }
    return checksum;
}

static bool load_persisted_target_addr(bd_addr_t addr_out) {
    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + TARGET_ADDR_STORE_OFFSET);
    const target_addr_store_t *stored = (const target_addr_store_t *)flash_ptr;

    if (stored->magic != TARGET_ADDR_STORE_MAGIC) {
        return false;
    }
    if (stored->checksum != target_addr_checksum(stored->addr)) {
        return false;
    }

    memcpy(addr_out, stored->addr, sizeof(bd_addr_t));
    return true;
}

static bool save_persisted_target_addr(const bd_addr_t addr) {
    target_addr_store_t stored = {0};
    uint8_t sector_buf[FLASH_SECTOR_SIZE];

    memset(sector_buf, 0xFF, sizeof(sector_buf));
    stored.magic = TARGET_ADDR_STORE_MAGIC;
    memcpy(stored.addr, addr, sizeof(bd_addr_t));
    stored.checksum = target_addr_checksum(stored.addr);
    memcpy(sector_buf, &stored, sizeof(stored));

    uint32_t flags = save_and_disable_interrupts();
    flash_range_erase(TARGET_ADDR_STORE_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TARGET_ADDR_STORE_OFFSET, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(flags);

    bd_addr_t verify_addr;
    if (!load_persisted_target_addr(verify_addr)) {
        return false;
    }
    return bd_addr_cmp(verify_addr, addr) == 0;
}

static bool addr_is_nintendo_prefix(const bd_addr_t addr) {
    const char *addr_str = bd_addr_to_str(addr);
    size_t prefix_count = sizeof(nintendo_addr_prefixes) / sizeof(nintendo_addr_prefixes[0]);
    for (size_t i = 0; i < prefix_count; i++) {
        const char *prefix = nintendo_addr_prefixes[i];
        if (strncmp(addr_str, prefix, strlen(prefix)) == 0) {
            return true;
        }
    }
    return false;
}

enum {
    WIIMOTE_IR_INIT_ENABLE_1 = 0,
    WIIMOTE_IR_INIT_ENABLE_2,
    WIIMOTE_IR_INIT_REG_30,
    WIIMOTE_IR_INIT_SENS_1,
    WIIMOTE_IR_INIT_SENS_2,
    WIIMOTE_IR_INIT_MODE_REG,
    WIIMOTE_IR_INIT_REG_30_FINAL,
    WIIMOTE_IR_INIT_REPORT_MODE_SELECT,
    WIIMOTE_IR_INIT_DONE
};

static const uint8_t wiimote_ir_mode_candidates[] = {0x33, 0x36, 0x37};

typedef struct {
    const char *name;
    uint8_t block1[9];
    uint8_t block2[2];
} wiimote_ir_sensitivity_profile_t;

static const wiimote_ir_sensitivity_profile_t wiimote_ir_profiles[] = {
    {
        .name = "Wii Level 2",
        .block1 = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xB4},
        .block2 = {0xB3, 0x04},
    },
    {
        .name = "Wii Level 3",
        .block1 = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64},
        .block2 = {0x63, 0x03},
    },
    {
        .name = "Wii Level 5",
        .block1 = {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20},
        .block2 = {0x1F, 0x03},
    },
    {
        .name = "Max Sensitivity",
        .block1 = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x0C},
        .block2 = {0x00, 0x00},
    },
};

static const char *error_code_to_string(uint8_t status) {
    switch (status) {
        case ERROR_CODE_SUCCESS:
            return "SUCCESS";
        case ERROR_CODE_AUTHENTICATION_FAILURE:
            return "AUTHENTICATION_FAILURE";
        case ERROR_CODE_PIN_OR_KEY_MISSING:
            return "PIN_OR_KEY_MISSING";
        case ERROR_CODE_CONNECTION_REJECTED_DUE_TO_SECURITY_REASONS:
            return "CONNECTION_REJECTED_SECURITY";
        case ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE:
            return "UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE";
        case ERROR_CODE_PAGE_TIMEOUT:
            return "PAGE_TIMEOUT";
        case ERROR_CODE_CONNECTION_TIMEOUT:
            return "CONNECTION_TIMEOUT";
        case ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS:
            return "ACL_CONNECTION_ALREADY_EXISTS";
        case BTSTACK_MEMORY_ALLOC_FAILED:
            return "BTSTACK_MEMORY_ALLOC_FAILED";
        case L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY:
            return "L2CAP_REFUSED_SECURITY";
        default:
            return "UNKNOWN";
    }
}

static void start_scan(void);

static bool is_passive_disconnect_reason(uint8_t reason) {
    return reason == WIIMOTE_DISCONNECT_REASON_POWER_OFF ||
           reason == WIIMOTE_DISCONNECT_REASON_REMOTE_USER_TERMINATED;
}

static void sync_mode_timeout_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;

    if (!sync_mode_active) {
        return;
    }

    sync_mode_active = false;
    set_onboard_led(false);
    printf("Sync mode timeout after %d ms\n", SYNC_MODE_TIMEOUT_MS);

    if (!hid_is_connected(g_hid_state)) {
        start_scan();
    }
}

static void enter_sync_mode(void) {
    if (sync_mode_active) {
        return;
    }

    sync_mode_active = true;
    passive_reconnect_mode = false;
    reconnect_cooldown_active = false;
    inactivity_disconnect_requested = false;
    pending_connect = false;

    btstack_run_loop_remove_timer(&connect_retry_timer);
    btstack_run_loop_remove_timer(&reconnect_cooldown_timer);
    btstack_run_loop_remove_timer(&sync_mode_timer);

    btstack_run_loop_set_timer(&sync_mode_timer, SYNC_MODE_TIMEOUT_MS);
    btstack_run_loop_add_timer(&sync_mode_timer);

    printf("Sync mode started (%d ms). Searching for Nintendo prefix %s\n",
           SYNC_MODE_TIMEOUT_MS,
            nintendo_addr_prefixes[0]);

    if (hid_is_connected(g_hid_state) && hid_host_cid != 0) {
        printf("Sync mode: disconnecting current Wii Remote\n");
        hid_host_disconnect(hid_host_cid);
        return;
    }

    start_scan();
}

static void request_bt_stack_reset(void) {
    if (bt_reset_in_progress) {
        return;
    }

    bt_reset_in_progress = true;
    bt_reset_waiting_for_power_on = true;

    btstack_run_loop_remove_timer(&connect_retry_timer);
    btstack_run_loop_remove_timer(&reconnect_cooldown_timer);

    printf("BT reset: power OFF\n");
    hci_power_control(HCI_POWER_OFF);

    btstack_run_loop_set_timer(&bt_reset_timer, BT_RESET_POWER_ON_DELAY_MS);
    btstack_run_loop_add_timer(&bt_reset_timer);
}

static void bt_reset_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;

    if (!bt_reset_waiting_for_power_on) {
        return;
    }

    bt_reset_waiting_for_power_on = false;
    printf("BT reset: power ON\n");
    hci_power_control(HCI_POWER_ON);

    bt_reset_in_progress = false;
    if (passive_reconnect_mode) {
        printf("BT reset complete; passive wake mode active. Press any Wii Remote button to reconnect.\n");
    } else {
        start_scan();
    }
}

static void reconnect_cooldown_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    reconnect_cooldown_active = false;
    if (passive_reconnect_mode) {
        printf("Reconnect cooldown ended; passive wake mode active. Press any Wii Remote button to reconnect.\n");
    } else {
        printf("Reconnect cooldown ended; resuming scan/connect\n");
        start_scan();
    }
}

static void connect_retry_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    if (passive_reconnect_mode) {
        printf("Reconnect retry skipped; passive wake mode active. Press any Wii Remote button to reconnect.\n");
        return;
    }
        if (!hid_is_connected(g_hid_state)) {
        start_scan();
    }
}

static void start_scan(void) {
    if (bt_reset_in_progress) {
        return;
    }
    if (sync_mode_active) {
        if (pending_connect || hid_is_connected(g_hid_state)) {
            return;
        }
         printf("Sync mode inquiry: checking %u prefix(es)\n",
             (unsigned)(sizeof(nintendo_addr_prefixes) / sizeof(nintendo_addr_prefixes[0])));
        gap_inquiry_start(INQUIRY_SECONDS);
        return;
    }
    if (reconnect_cooldown_active) {
        return;
    }
    if (passive_reconnect_mode) {
        return;
    }
    if (pending_connect || hid_is_connected(g_hid_state)) {
        return;
    }
    if (g_wiimote_state->target_addr_configured) {
        printf("Connecting directly to %s\n", bd_addr_to_str(g_wiimote_state->target_addr));
        uint8_t status = hid_host_connect(g_wiimote_state->target_addr, hid_host_report_mode, &hid_host_cid);
        if (status == ERROR_CODE_SUCCESS) {
            pending_connect = true;
        } else {
            printf("Connect attempt failed (0x%02x), retry in %dms\n", status, CONNECT_RETRY_MS);
            btstack_run_loop_set_timer(&connect_retry_timer, CONNECT_RETRY_MS);
            btstack_run_loop_add_timer(&connect_retry_timer);
        }
    } else {
        printf("No target address. Put Wii Remote into discoverable mode (press 1+2).\n");
        gap_inquiry_start(INQUIRY_SECONDS);
    }
}

static bool is_wii_name(const char *name) {
    return strncmp(name, wii_name_prefix, strlen(wii_name_prefix)) == 0;
}

static void wiimote_tracking_state_init(wiimote_tracking_state_t *state) {
    memset(state, 0, sizeof(*state));
    state->ir_sensitivity_index = 2;
    state->wii_pin_use_reversed = true;
}

static void wiimote_tracking_state_reset_session(wiimote_tracking_state_t *state) {
    bd_addr_t target_addr = {0};
    bool target_addr_configured = state->target_addr_configured;
    bool wii_pin_use_reversed = state->wii_pin_use_reversed;
    uint8_t ir_sensitivity_index = state->ir_sensitivity_index;

    memcpy(target_addr, state->target_addr, sizeof(target_addr));
    memset(state, 0, sizeof(*state));
    memcpy(state->target_addr, target_addr, sizeof(target_addr));
    state->target_addr_configured = target_addr_configured;
    state->wii_pin_use_reversed = wii_pin_use_reversed;
    state->ir_sensitivity_index = ir_sensitivity_index;
}

static bool is_target_addr(const wiimote_tracking_state_t *state, const bd_addr_t addr) {
    return state->target_addr_configured && (bd_addr_cmp(addr, state->target_addr) == 0);
}

static void respond_wii_pin_code(const wiimote_tracking_state_t *state, const bd_addr_t remote_addr) {
    bd_addr_t local_addr;
    uint8_t pin_data[6];

    gap_local_bd_addr(local_addr);
    for (int i = 0; i < 6; i++) {
        pin_data[i] = state->wii_pin_use_reversed ? local_addr[5 - i] : local_addr[i];
    }

    printf("PIN requested by target Wii Remote %s, replying with %s local BD_ADDR bytes\n",
           bd_addr_to_str(remote_addr),
           state->wii_pin_use_reversed ? "reversed" : "forward");

    int rc = gap_pin_code_response_binary(remote_addr, pin_data, sizeof(pin_data));
    if (rc != ERROR_CODE_SUCCESS) {
        printf("PIN response call failed (0x%02x)\n", (uint8_t)rc);
    }
}

static void append_button(char *buffer, size_t len, bool *first, const char *name) {
    const char *separator = *first ? "" : "|";
    snprintf(buffer + strlen(buffer), len - strlen(buffer), "%s%s", separator, name);
    *first = false;
}

static uint8_t wiimote_send_output_report(uint16_t report_id, const uint8_t *data, uint16_t len);

static const char *buttons_to_string(uint16_t buttons) {
    static char text[96];
    bool first = true;

    text[0] = '\0';
    if (buttons == 0) {
        return "(none)";
    }

    if (buttons & 0x0800) append_button(text, sizeof(text), &first, "Up");
    if (buttons & 0x0400) append_button(text, sizeof(text), &first, "Down");
    if (buttons & 0x0200) append_button(text, sizeof(text), &first, "Right");
    if (buttons & 0x0100) append_button(text, sizeof(text), &first, "Left");
    if (buttons & 0x0010) append_button(text, sizeof(text), &first, "-");
    if (buttons & 0x1000) append_button(text, sizeof(text), &first, "+");
    if (buttons & 0x0080) append_button(text, sizeof(text), &first, "Home");
    if (buttons & 0x0008) append_button(text, sizeof(text), &first, "A");
    if (buttons & 0x0004) append_button(text, sizeof(text), &first, "B");
    if (buttons & 0x0001) append_button(text, sizeof(text), &first, "2");
    if (buttons & 0x0002) append_button(text, sizeof(text), &first, "1");

    return text;
}

static void send_wiimote_write_memory(uint32_t address, const uint8_t *data, uint8_t len) {
    uint8_t payload[21];

    if (len > 16) {
        len = 16;
    }

    payload[0] = 0x04;
    payload[1] = (uint8_t)((address >> 16) & 0xFF);
    payload[2] = (uint8_t)((address >> 8) & 0xFF);
    payload[3] = (uint8_t)(address & 0xFF);
    payload[4] = len;
    memcpy(&payload[5], data, len);

    (void)wiimote_send_output_report(0x16, payload, (uint16_t)(len + 5));
}

static uint8_t wiimote_send_output_report(uint16_t report_id, const uint8_t *data, uint16_t len) {
    uint8_t status = hid_host_send_report(hid_host_cid, report_id, data, (uint8_t)len);
    if (status == ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER || status == ERROR_CODE_COMMAND_DISALLOWED) {
        status = hid_host_send_set_report(hid_host_cid, HID_REPORT_TYPE_OUTPUT, report_id, data, (uint8_t)len);
    }
    if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
        printf("Output report 0x%02x send failed (0x%02x)\n", report_id, status);
    }
    return status;
}

static void set_wiimote_rightmost_led(void) {
    uint8_t led_payload[1] = {WIIMOTE_LED_RIGHTMOST_MASK};
    uint8_t status = wiimote_send_output_report(0x11, led_payload, sizeof(led_payload));
    if (status == ERROR_CODE_SUCCESS || status == ERROR_CODE_COMMAND_DISALLOWED) {
        printf("Set Wii Remote LEDs: rightmost ON\n");
    }
}

static uint8_t wiimote_write_memory_checked(uint32_t address, const uint8_t *data, uint8_t len) {
    uint8_t payload[21];

    if (len > 16) {
        len = 16;
    }

    payload[0] = 0x04;
    payload[1] = (uint8_t)((address >> 16) & 0xFF);
    payload[2] = (uint8_t)((address >> 8) & 0xFF);
    payload[3] = (uint8_t)(address & 0xFF);
    payload[4] = len;
    memset(&payload[5], 0, 16);
    memcpy(&payload[5], data, len);

    return wiimote_send_output_report(0x16, payload, sizeof(payload));
}

static void select_ir_sensitivity_profile(wiimote_tracking_state_t *state, uint8_t new_index) {
    const uint8_t profile_count = (uint8_t)(sizeof(wiimote_ir_profiles) / sizeof(wiimote_ir_profiles[0]));
    state->ir_sensitivity_index = new_index % profile_count;
    printf("IR sensitivity profile: %s\n", wiimote_ir_profiles[state->ir_sensitivity_index].name);
}

static void request_wiimote_ir_report(wiimote_tracking_state_t *state) {
    state->ir_init_in_progress = true;
    state->ir_init_step = WIIMOTE_IR_INIT_ENABLE_1;
    state->ir_debug_frames_remaining = 24;
    state->ir_report_mode_active = false;
    state->ir_mode_index = 0;
    state->ir_mode_retry_count = 0;
    printf("Applying IR profile: %s\n", wiimote_ir_profiles[state->ir_sensitivity_index].name);
    btstack_run_loop_set_timer(&ir_init_timer, 10);
    btstack_run_loop_add_timer(&ir_init_timer);
}

static void wiimote_ir_init_timer_handler_state(wiimote_tracking_state_t *state, btstack_timer_source_t *ts) {
    (void)ts;

    if (!hid_is_connected(g_hid_state) || !state->ir_init_in_progress) {
        return;
    }

    uint8_t ir_enable_payload[1] = {0x04};
    uint8_t mode_payload[2] = {0x04, 0x33};
    const wiimote_ir_sensitivity_profile_t *profile = &wiimote_ir_profiles[state->ir_sensitivity_index];
    uint8_t ir_mode_ext[1] = {0x03};

    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;

    switch (state->ir_init_step) {
        case WIIMOTE_IR_INIT_ENABLE_1:
            status = wiimote_send_output_report(0x13, ir_enable_payload, sizeof(ir_enable_payload));
            break;
        case WIIMOTE_IR_INIT_ENABLE_2:
            status = wiimote_send_output_report(0x1A, ir_enable_payload, sizeof(ir_enable_payload));
            break;
        case WIIMOTE_IR_INIT_REG_30:
            status = wiimote_write_memory_checked(0x04B00030u, (const uint8_t[]){0x08}, 1);
            break;
        case WIIMOTE_IR_INIT_SENS_1:
            status = wiimote_write_memory_checked(0x04B00000u, profile->block1, sizeof(profile->block1));
            break;
        case WIIMOTE_IR_INIT_SENS_2:
            status = wiimote_write_memory_checked(0x04B0001Au, profile->block2, sizeof(profile->block2));
            break;
        case WIIMOTE_IR_INIT_MODE_REG:
            status = wiimote_write_memory_checked(0x04B00033u, ir_mode_ext, sizeof(ir_mode_ext));
            break;
        case WIIMOTE_IR_INIT_REG_30_FINAL:
            status = wiimote_write_memory_checked(0x04B00030u, (const uint8_t[]){0x08}, 1);
            break;
        case WIIMOTE_IR_INIT_REPORT_MODE_SELECT:
            mode_payload[1] = wiimote_ir_mode_candidates[state->ir_mode_index];
            status = wiimote_send_output_report(0x12, mode_payload, sizeof(mode_payload));
            break;
        default:
            return;
    }

    if (state->ir_init_step == WIIMOTE_IR_INIT_REPORT_MODE_SELECT) {
        if (state->ir_report_mode_active) {
            state->ir_init_step = WIIMOTE_IR_INIT_DONE;
        } else if (status == ERROR_CODE_SUCCESS || status == ERROR_CODE_COMMAND_DISALLOWED) {
            state->ir_mode_retry_count++;
            if (state->ir_mode_retry_count >= 8) {
                state->ir_mode_retry_count = 0;
                state->ir_mode_index++;
                if (state->ir_mode_index < (uint8_t)(sizeof(wiimote_ir_mode_candidates) / sizeof(wiimote_ir_mode_candidates[0]))) {
                    printf("IR mode 0x%02x not active yet, trying 0x%02x\n",
                           wiimote_ir_mode_candidates[state->ir_mode_index - 1],
                           wiimote_ir_mode_candidates[state->ir_mode_index]);
                } else {
                    printf("IR mode negotiation failed (0x33/0x36/0x37)\n");
                    state->ir_init_in_progress = false;
                    return;
                }
            }
        }
    } else {
        if (status == ERROR_CODE_SUCCESS) {
            state->ir_init_step++;
        } else if (status != ERROR_CODE_COMMAND_DISALLOWED) {
            printf("IR init step %u send status 0x%02x\n", state->ir_init_step, status);
        }
    }

    if (state->ir_init_step >= WIIMOTE_IR_INIT_DONE) {
        state->ir_init_in_progress = false;
        printf("Wii IR init sequence complete\n");
        set_wiimote_rightmost_led();
        return;
    }

    btstack_run_loop_set_timer(&ir_init_timer, state->ir_init_step == WIIMOTE_IR_INIT_REPORT_MODE_SELECT ? 120 : 40);
    btstack_run_loop_add_timer(&ir_init_timer);
}

static void wiimote_ir_init_timer_handler(btstack_timer_source_t *ts) {
    wiimote_ir_init_timer_handler_state(g_wiimote_state, ts);
}

static void compute_ir_norm(wiimote_tracking_state_t *state, uint16_t cx, uint16_t cy, uint16_t spread_x, uint16_t spread_y) {
    int32_t usable_x = 1023 - (int32_t)spread_x;
    if (usable_x > 0) {
        int32_t nx = ((int32_t)cx - (int32_t)(spread_x / 2u)) * 1000 / usable_x;
        if (nx < 0) nx = 0;
        if (nx > 1000) nx = 1000;
        state->norm_x = (uint16_t)nx;
    } else {
        state->norm_x = 500u;
    }

    int32_t usable_y = 767 - (int32_t)spread_y;
    if (usable_y > 0) {
        int32_t ny = ((int32_t)cy - (int32_t)(spread_y / 2u)) * 1000 / usable_y;
        if (ny < 0) ny = 0;
        if (ny > 1000) ny = 1000;
        state->norm_y = (uint16_t)ny;
    } else {
        state->norm_y = 500u;
    }
}

static void parse_wiimote_ir_extended(wiimote_tracking_state_t *state, const uint8_t *ir_data, uint16_t ir_len) {
    if (ir_len < 12) {
        return;
    }

    int valid_count = 0;
    uint16_t valid_x[WIIMOTE_IR_POINTS];
    uint16_t valid_y[WIIMOTE_IR_POINTS];

    for (int i = 0; i < WIIMOTE_IR_POINTS; i++) {
        const uint8_t x_low = ir_data[i * 3];
        const uint8_t y_low = ir_data[i * 3 + 1];
        const uint8_t xy_hi = ir_data[i * 3 + 2];

        const uint16_t x = (uint16_t)x_low | (uint16_t)(((xy_hi >> 4) & 0x03u) << 8);
        const uint16_t y = (uint16_t)y_low | (uint16_t)(((xy_hi >> 6) & 0x03u) << 8);
        const uint8_t size = xy_hi & 0x0Fu;
        const bool valid = !((x == 1023u) && (y == 1023u));

        state->points[i].valid = valid;
        state->points[i].x = x;
        state->points[i].y = y;
        state->points[i].size = size;

        if (valid) {
            valid_x[valid_count] = x;
            valid_y[valid_count] = y;
            valid_count++;
        }
    }

    state->have_ir = true;

    if (valid_count >= 2) {
        uint16_t xl = valid_x[0], yl = valid_y[0];
        uint16_t xr = valid_x[1], yr = valid_y[1];
        if (xl > xr) {
            uint16_t t;
            t = xl; xl = xr; xr = t;
            t = yl; yl = yr; yr = t;
        }

        state->center_x = (uint16_t)((xl + xr) / 2u);
        state->center_y = (uint16_t)((yl + yr) / 2u);
        state->have_center = true;

        state->ir_last_half_dx = (int16_t)((xr - xl) / 2u);
        state->ir_last_half_dy = (int16_t)(((int32_t)yr - (int32_t)yl) / 2);
        state->ir_last_center_x = state->center_x;
        state->ir_last_center_y = state->center_y;
        state->ir_dropout_frames = 0;
        state->ir_have_spread = true;

        uint16_t spread_x = (uint16_t)(xr - xl);
        uint16_t spread_y = (yr >= yl) ? (uint16_t)(yr - yl) : (uint16_t)(yl - yr);
        compute_ir_norm(state, state->center_x, state->center_y, spread_x, spread_y);
        state->have_norm = true;

    } else if (valid_count == 1 && state->ir_have_spread && state->ir_dropout_frames < 3) {
        uint16_t sx = valid_x[0];
        uint16_t sy = valid_y[0];
        int32_t est_cx, est_cy;

        if (sx <= state->ir_last_center_x) {
            est_cx = (int32_t)sx + state->ir_last_half_dx;
            est_cy = (int32_t)sy + state->ir_last_half_dy;
        } else {
            est_cx = (int32_t)sx - state->ir_last_half_dx;
            est_cy = (int32_t)sy - state->ir_last_half_dy;
        }

        if (est_cx < 0) est_cx = 0;
        if (est_cx > 1023) est_cx = 1023;
        if (est_cy < 0) est_cy = 0;
        if (est_cy > 767) est_cy = 767;

        state->center_x = (uint16_t)est_cx;
        state->center_y = (uint16_t)est_cy;
        state->have_center = true;
        state->ir_dropout_frames++;

        uint16_t spread_x = (uint16_t)(2 * state->ir_last_half_dx);
        uint16_t spread_y = (state->ir_last_half_dy >= 0)
            ? (uint16_t)(2 * state->ir_last_half_dy)
            : (uint16_t)(-2 * state->ir_last_half_dy);
        compute_ir_norm(state, state->center_x, state->center_y, spread_x, spread_y);
        state->have_norm = true;

    } else {
        state->have_center = false;
        state->have_norm = false;
    }
}

static void handle_ir_profile_hotkeys(wiimote_tracking_state_t *state, uint16_t buttons) {
    uint16_t changed = buttons ^ state->previous_buttons;
    bool home_down = (buttons & 0x0080u) != 0;

    if (!home_down) {
        state->previous_buttons = buttons;
        return;
    }

    if ((changed & 0x0800u) && (buttons & 0x0800u)) {
        select_ir_sensitivity_profile(state, (uint8_t)(state->ir_sensitivity_index + 1));
        if (hid_is_connected(g_hid_state)) {
            request_wiimote_ir_report(state);
        }
    } else if ((changed & 0x0400u) && (buttons & 0x0400u)) {
        const uint8_t profile_count = (uint8_t)(sizeof(wiimote_ir_profiles) / sizeof(wiimote_ir_profiles[0]));
        select_ir_sensitivity_profile(state, (uint8_t)((state->ir_sensitivity_index + profile_count - 1) % profile_count));
        if (hid_is_connected(g_hid_state)) {
            request_wiimote_ir_report(state);
        }
    }

    state->previous_buttons = buttons;
}

static void reset_inactivity_timer(uint16_t buttons) {
    if (!hid_is_connected(g_hid_state)) return;
    if (buttons == inactivity_prev_buttons) return;
    printf("Inactivity timer reset (buttons 0x%04x -> 0x%04x)\n", inactivity_prev_buttons, buttons);
    inactivity_prev_buttons = buttons;
    btstack_run_loop_remove_timer(&inactivity_timer);
    btstack_run_loop_set_timer(&inactivity_timer, INACTIVITY_TIMEOUT_MS);
    btstack_run_loop_add_timer(&inactivity_timer);
}

static void reset_inactivity_timer_on_ir_activity(const wiimote_tracking_state_t *state) {
    if (!hid_is_connected(g_hid_state)) {
        return;
    }

    if (!state->have_norm) {
        inactivity_prev_have_norm = false;
        return;
    }

    if (!inactivity_prev_have_norm) {
        inactivity_prev_have_norm = true;
        inactivity_prev_norm_x = state->norm_x;
        inactivity_prev_norm_y = state->norm_y;
        printf("Inactivity timer reset (IR acquired at norm=(%u,%u))\n", state->norm_x, state->norm_y);
        btstack_run_loop_remove_timer(&inactivity_timer);
        btstack_run_loop_set_timer(&inactivity_timer, INACTIVITY_TIMEOUT_MS);
        btstack_run_loop_add_timer(&inactivity_timer);
        return;
    }

    int16_t dx = (int16_t)state->norm_x - (int16_t)inactivity_prev_norm_x;
    int16_t dy = (int16_t)state->norm_y - (int16_t)inactivity_prev_norm_y;
    uint16_t adx = (uint16_t)(dx < 0 ? -dx : dx);
    uint16_t ady = (uint16_t)(dy < 0 ? -dy : dy);

    if (adx >= INACTIVITY_IR_MOVE_THRESHOLD || ady >= INACTIVITY_IR_MOVE_THRESHOLD) {
        printf("Inactivity timer reset (IR motion norm=(%u,%u), d=(%d,%d))\n",
               state->norm_x,
               state->norm_y,
               dx,
               dy);
        inactivity_prev_norm_x = state->norm_x;
        inactivity_prev_norm_y = state->norm_y;
        btstack_run_loop_remove_timer(&inactivity_timer);
        btstack_run_loop_set_timer(&inactivity_timer, INACTIVITY_TIMEOUT_MS);
        btstack_run_loop_add_timer(&inactivity_timer);
    }
}

static void inactivity_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;
    if (hid_is_connected(g_hid_state)) {
        printf("Inactivity timeout (%d ms): disconnecting and entering passive wake mode\n", INACTIVITY_TIMEOUT_MS);
        inactivity_disconnect_requested = true;
        passive_reconnect_mode = true;
        hid_host_disconnect(hid_host_cid);
    }
}

static void apply_wiimote_buttons(wiimote_tracking_state_t *state, uint16_t raw_buttons) {
    uint16_t new_buttons = raw_buttons & WIIMOTE_BUTTON_MASK;

    if (raw_buttons != prev_raw_buttons) {
        prev_raw_buttons = raw_buttons;
    }

    reset_inactivity_timer(new_buttons);

    state->buttons = new_buttons;
    state->have_buttons = true;
    handle_ir_profile_hotkeys(state, state->buttons);
}

static void parse_wiimote_report(wiimote_tracking_state_t *state, const uint8_t *report, uint16_t report_len) {
    if (report_len < 4) {
        return;
    }
    if (report[0] != 0xA1) {
        return;
    }

    uint8_t report_id = report[1];
    if (!state->seen_report_id[report_id]) {
        state->seen_report_id[report_id] = true;
        printf("Observed input report 0x%02x (len=%u)\n", report_id, report_len);
    }

    if (report_id == 0x22 && report_len >= 4) {
        printf("Output report ack: id=0x%02x result=0x%02x\n", report[2], report[3]);
    }

    if (state->ir_init_in_progress &&
        report_id == wiimote_ir_mode_candidates[state->ir_mode_index]) {
        state->ir_report_mode_active = true;
        printf("IR report mode active: 0x%02x\n", report_id);
    }

    if (report_id == 0x20 && report_len >= 6) {
        uint8_t status_flags = report[4];
        bool extension_present = (report[4] & 0x02u) != 0;
        bool ir_enabled = (report[4] & 0x08u) != 0;
        bool status_changed = !state->status_flags_valid || state->status_flags != status_flags;

        apply_wiimote_buttons(state, ((uint16_t)report[2] << 8) | report[3]);
        state->status_flags = status_flags;
        state->status_flags_valid = true;

        if (status_changed) {
            state->have_norm = false;
            state->have_center = false;
            printf("Status report: extension=%s ir=%s flags=0x%02x, renegotiating report mode\n",
                   extension_present ? "yes" : "no",
                   ir_enabled ? "on" : "off",
                   status_flags);
        }

        if (status_changed && !state->ir_init_in_progress) {
            request_wiimote_ir_report(state);
        }
        return;
    }

    if (report_id < 0x30 || report_id > 0x3F) {
        return;
    }

    apply_wiimote_buttons(state, ((uint16_t)report[2] << 8) | report[3]);

    if (report_id == 0x33 && report_len >= 19) {
        if (state->ir_debug_frames_remaining > 0) {
            state->ir_debug_frames_remaining--;
            printf("IR raw: %02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x\n",
                   report[7], report[8], report[9],
                   report[10], report[11], report[12],
                   report[13], report[14], report[15],
                   report[16], report[17], report[18]);
        }
        parse_wiimote_ir_extended(state, &report[7], (uint16_t)(report_len - 7));
        reset_inactivity_timer_on_ir_activity(state);
    }

}

static void button_print_timer_handler_state(const wiimote_tracking_state_t *state, btstack_timer_source_t *ts) {
    (void)ts;

    if (hid_is_connected(g_hid_state) && state->have_buttons) {
        printf("Buttons: %s\n", buttons_to_string(state->buttons));
    }

    if (hid_is_connected(g_hid_state) && state->have_ir) {
        bool printed_any = false;
        printf("IR:");
        for (int i = 0; i < WIIMOTE_IR_POINTS; i++) {
            if (!state->points[i].valid) {
                continue;
            }
            printf(" p%d=(%u,%u,s%u)", i,
                   state->points[i].x,
                   state->points[i].y,
                   state->points[i].size);
            printed_any = true;
        }
        if (!printed_any) {
            printf(" (no blobs)");
        }
        if (state->have_center) {
            printf(" center=(%u,%u)", state->center_x, state->center_y);
        }
        if (state->have_norm) {
            printf(" norm=(%u,%u)", state->norm_x, state->norm_y);
        }
        printf("\n");
    }

    btstack_run_loop_set_timer(&button_print_timer, BUTTON_PRINT_PERIOD_MS);
    btstack_run_loop_add_timer(&button_print_timer);
}

static void button_print_timer_handler(btstack_timer_source_t *ts) {
    button_print_timer_handler_state(g_wiimote_state, ts);
}

static bool __no_inline_not_in_flash_func(read_bootsel_button_pressed)(void) {
    const uint cs_pin_index = 1;
    uint32_t flags = save_and_disable_interrupts();

    // Float QSPI CS so the BOOTSEL switch can pull it low while XIP is paused.
    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i) {
    }

#ifdef __ARM_ARCH_6M__
    const uint32_t cs_bit = (1u << 1);
#else
    const uint32_t cs_bit = SIO_GPIO_HI_IN_QSPI_CSN_BITS;
#endif
    bool cs_high = (sio_hw->gpio_hi_in & cs_bit) != 0;

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);

    // BOOTSEL pulls CS low when pressed.
    return !cs_high;
}

static void bootsel_poll_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;

    bool pressed = read_bootsel_button_pressed();
    if (pressed != bootsel_prev_pressed) {
        printf("BOOTSEL: %s\n", pressed ? "pressed" : "released");
        if (pressed) {
            enter_sync_mode();
        }
        bootsel_prev_pressed = pressed;
    }

    if (sync_mode_active) {
        set_onboard_led(!onboard_led_on);
    } else {
        set_onboard_led(false);
    }

    btstack_run_loop_set_timer(&bootsel_poll_timer, BOOTSEL_POLL_PERIOD_MS);
    btstack_run_loop_add_timer(&bootsel_poll_timer);
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    uint8_t event = hci_event_packet_get_type(packet);
    bd_addr_t addr;

    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                start_scan();
            }
            break;

        case GAP_EVENT_INQUIRY_RESULT: {
            if (hid_is_connected(g_hid_state) || pending_connect) {
                break;
            }

            gap_event_inquiry_result_get_bd_addr(packet, addr);

            const char *name = "(no name)";
            char name_buf[248];
            bool has_name = gap_event_inquiry_result_get_name_available(packet);
            if (has_name) {
                int name_len = gap_event_inquiry_result_get_name_len(packet);
                if (name_len >= (int)sizeof(name_buf)) {
                    name_len = (int)sizeof(name_buf) - 1;
                }
                memcpy(name_buf, gap_event_inquiry_result_get_name(packet), (size_t)name_len);
                name_buf[name_len] = '\0';
                name = name_buf;
            }

            int rssi = 0;
            bool has_rssi = gap_event_inquiry_result_get_rssi_available(packet);
            if (has_rssi) {
                rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
            }

            if (sync_mode_active) {
                bool nintendo_prefix_match = addr_is_nintendo_prefix(addr);
                printf("Sync discovery: %s  cod=0x%06lx",
                       bd_addr_to_str(addr),
                       (unsigned long)gap_event_inquiry_result_get_class_of_device(packet));
                if (has_rssi) {
                    printf("  rssi=%d", rssi);
                }
                printf("  name='%s'  nintendo_prefix=%s\n",
                       name,
                       nintendo_prefix_match ? "yes" : "no");

                if (!nintendo_prefix_match) {
                    break;
                }

                memcpy(g_wiimote_state->target_addr, addr, sizeof(bd_addr_t));
                g_wiimote_state->target_addr_configured = true;

                printf("Sync mode candidate found at %s\n", bd_addr_to_str(g_wiimote_state->target_addr));
                gap_inquiry_stop();

                uint8_t status = hid_host_connect(g_wiimote_state->target_addr, hid_host_report_mode, &hid_host_cid);
                if (status == ERROR_CODE_SUCCESS) {
                    pending_connect = true;
                } else {
                    printf("Sync connect attempt failed (0x%02x), continuing inquiry\n", status);
                }
                break;
            }

            printf("Inquiry: %s  cod=0x%06lx", bd_addr_to_str(addr),
                   (unsigned long)gap_event_inquiry_result_get_class_of_device(packet));
            if (has_rssi) {
                printf("  rssi=%d", rssi);
            }
            printf("  name='%s'\n", name);

            if (!is_target_addr(g_wiimote_state, addr) && (!has_name || !is_wii_name(name))) {
                break;
            }

            if (!is_target_addr(g_wiimote_state, addr)) {
                memcpy(g_wiimote_state->target_addr, addr, sizeof(bd_addr_t));
                g_wiimote_state->target_addr_configured = true;
            }
            pending_connect = true;

            printf("Target Wii Remote candidate at %s (name='%s')\n", bd_addr_to_str(g_wiimote_state->target_addr), name);
            printf("Stopping inquiry and connecting...\n");
            gap_inquiry_stop();
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (!hid_is_connected(g_hid_state)) {
                start_scan();
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            if (is_target_addr(g_wiimote_state, addr)) {
                respond_wii_pin_code(g_wiimote_state, addr);
            } else {
                printf("PIN requested by %s, responding with 0000\n", bd_addr_to_str(addr));
                gap_pin_code_response(addr, "0000");
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint16_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);

            if (wiimote_con_handle != HCI_CON_HANDLE_INVALID && con_handle == wiimote_con_handle) {
                wiimote_last_disconnect_reason = reason;
                printf("Wii ACL disconnection reason: 0x%02x\n", reason);
                if (reason == WIIMOTE_DISCONNECT_REASON_POWER_OFF) {
                    hid_queue_sleep_signal(g_hid_state);
                }

                if (!inactivity_disconnect_requested && is_passive_disconnect_reason(reason)) {
                    passive_reconnect_mode = true;
                    btstack_run_loop_remove_timer(&connect_retry_timer);

                    reconnect_cooldown_active = true;
                    btstack_run_loop_remove_timer(&reconnect_cooldown_timer);
                    btstack_run_loop_set_timer(&reconnect_cooldown_timer, RECONNECT_COOLDOWN_MS);
                    btstack_run_loop_add_timer(&reconnect_cooldown_timer);

                    printf("Disconnect reason 0x%02x -> passive wake mode guard for %d ms\n",
                           reason,
                           RECONNECT_COOLDOWN_MS);
                }
                wiimote_con_handle = HCI_CON_HANDLE_INVALID;
            }
            break;
        }

        case HCI_EVENT_HID_META: {
            uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
            switch (subevent) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    if (reconnect_cooldown_active) {
                        uint16_t incoming_hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
                        uint8_t status = hid_host_decline_connection(incoming_hid_cid);
                        printf("Rejecting incoming HID connection during cooldown (status 0x%02x)\n", status);
                    } else {
                        hid_host_accept_connection(
                            hid_subevent_incoming_connection_get_hid_cid(packet),
                            hid_host_report_mode);
                        printf("Accepting incoming HID connection\n");
                    }
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("HID connection failed (0x%02x: %s)\n", status, error_code_to_string(status));
                        if (status == L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY) {
                            g_wiimote_state->wii_pin_use_reversed = !g_wiimote_state->wii_pin_use_reversed;
                            printf("Security refusal: next attempt will use %s local BD_ADDR PIN byte order\n",
                                   g_wiimote_state->wii_pin_use_reversed ? "reversed" : "forward");
                        }
                            hid_set_connected(g_hid_state, false);
                        hid_host_cid = 0;
                        wiimote_con_handle = HCI_CON_HANDLE_INVALID;
                        pending_connect = false;
                        if (sync_mode_active) {
                            start_scan();
                        } else {
                            btstack_run_loop_set_timer(&connect_retry_timer, CONNECT_RETRY_MS);
                            btstack_run_loop_add_timer(&connect_retry_timer);
                        }
                        break;
                    }

                    hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    wiimote_con_handle = hid_subevent_connection_opened_get_con_handle(packet);
                        hid_set_connected(g_hid_state, true);
                    pending_connect = false;
                    passive_reconnect_mode = false;
                    inactivity_disconnect_requested = false;
                    wiimote_last_disconnect_reason = 0x00;

                    hid_reset_output_state(g_hid_state);
                    hid_request_usb_remote_wake_on_reconnect();
                    hid_queue_wake_nudge(g_hid_state);

                    bd_addr_t connected_addr;
                    hid_subevent_connection_opened_get_bd_addr(packet, connected_addr);
                    memcpy(g_wiimote_state->target_addr, connected_addr, sizeof(bd_addr_t));
                    g_wiimote_state->target_addr_configured = true;

                    if (sync_mode_active) {
                        bool saved = save_persisted_target_addr(connected_addr);
                        printf("Sync mode paired with %s (%s)\n",
                               bd_addr_to_str(connected_addr),
                               saved ? "saved" : "save failed");
                        sync_mode_active = false;
                        btstack_run_loop_remove_timer(&sync_mode_timer);
                        set_onboard_led(false);
                    }

                    wiimote_tracking_state_reset_session(g_wiimote_state);
                    set_wiimote_rightmost_led();
                    hid_reset_output_state(g_hid_state);
                    inactivity_prev_buttons = 0xFFFF; // force first reset
                    inactivity_prev_have_norm = false;
                    inactivity_prev_norm_x = 0;
                    inactivity_prev_norm_y = 0;
                    reset_inactivity_timer(0x0000);
                    printf("Wii Remote connected, inactivity timer started (%d ms)\n", INACTIVITY_TIMEOUT_MS);
                    break;
                }

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("HID descriptor ready, enabling IR report mode\n");
                        set_wiimote_rightmost_led();
                        request_wiimote_ir_report(g_wiimote_state);
                    } else {
                        printf("Descriptor unavailable (0x%02x)\n", status);
                    }
                    break;
                }

                case HID_SUBEVENT_REPORT:
                    parse_wiimote_report(
                        g_wiimote_state,
                        hid_subevent_report_get_report(packet),
                        hid_subevent_report_get_report_len(packet));
                    break;

                case HID_SUBEVENT_SET_REPORT_RESPONSE: {
                    uint8_t hs = hid_subevent_set_report_response_get_handshake_status(packet);
                    if (hs != HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        printf("SET_REPORT handshake error (step %u): 0x%02x\n", g_wiimote_state->ir_init_step, hs);
                    }
                    break;
                }

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    printf("Wii Remote disconnected\n");
                        hid_set_connected(g_hid_state, false);
                    hid_host_cid = 0;
                    pending_connect = false;
                    btstack_run_loop_remove_timer(&inactivity_timer);
                    btstack_run_loop_remove_timer(&connect_retry_timer);
                    wiimote_tracking_state_reset_session(g_wiimote_state);
                    hid_reset_output_state(g_hid_state);
                    inactivity_prev_have_norm = false;
                    inactivity_prev_norm_x = 0;
                    inactivity_prev_norm_y = 0;
                    if (inactivity_disconnect_requested) {
                        inactivity_disconnect_requested = false;
                        request_bt_stack_reset();
                        reconnect_cooldown_active = true;
                        printf("Inactivity disconnect: waiting %d ms before reconnect\n", RECONNECT_COOLDOWN_MS);
                        btstack_run_loop_set_timer(&reconnect_cooldown_timer, RECONNECT_COOLDOWN_MS);
                        btstack_run_loop_add_timer(&reconnect_cooldown_timer);
                    } else if (is_passive_disconnect_reason(wiimote_last_disconnect_reason)) {
                        passive_reconnect_mode = true;
                        reconnect_cooldown_active = true;
                        btstack_run_loop_set_timer(&reconnect_cooldown_timer, RECONNECT_COOLDOWN_MS);
                        btstack_run_loop_add_timer(&reconnect_cooldown_timer);
                        printf("Passive disconnect reason 0x%02x: passive wake mode active. Press any Wii Remote button to reconnect.\n",
                               wiimote_last_disconnect_reason);
                    } else {
                        passive_reconnect_mode = false;
                        btstack_run_loop_set_timer(&connect_retry_timer, DISCONNECT_REASON_SETTLE_MS);
                        btstack_run_loop_add_timer(&connect_retry_timer);
                        printf("Disconnect reason pending/non-passive; re-evaluating reconnect in %d ms\n",
                               DISCONNECT_REASON_SETTLE_MS);
                    }
                    wiimote_last_disconnect_reason = 0x00;
                    break;

                default:
                    break;
            }
            break;
        }

        default:
            break;
    }
}

void wiimote_init_state(wiimote_tracking_state_t *wiimote_state, hid_state_t *hid_state) {
    g_wiimote_state = wiimote_state;
    g_hid_state = hid_state;

    wiimote_tracking_state_init(g_wiimote_state);
    if (load_persisted_target_addr(g_wiimote_state->target_addr)) {
        g_wiimote_state->target_addr_configured = true;
        printf("Loaded saved target address: %s\n", bd_addr_to_str(g_wiimote_state->target_addr));
    } else {
        g_wiimote_state->target_addr_configured = false;
        printf("No saved target address; starting in discovery mode\n");
    }

    l2cap_init();
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    gap_ssp_set_enable(0);

    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_run_loop_set_timer_handler(&ir_init_timer, wiimote_ir_init_timer_handler);
    btstack_run_loop_set_timer_handler(&inactivity_timer, inactivity_timer_handler);
    btstack_run_loop_set_timer_handler(&connect_retry_timer, connect_retry_timer_handler);
    btstack_run_loop_set_timer_handler(&reconnect_cooldown_timer, reconnect_cooldown_timer_handler);
    btstack_run_loop_set_timer_handler(&bt_reset_timer, bt_reset_timer_handler);
    btstack_run_loop_set_timer_handler(&sync_mode_timer, sync_mode_timeout_timer_handler);
    btstack_run_loop_set_timer_handler(&bootsel_poll_timer, bootsel_poll_timer_handler);

    btstack_run_loop_set_timer(&bootsel_poll_timer, BOOTSEL_POLL_PERIOD_MS);
    btstack_run_loop_add_timer(&bootsel_poll_timer);
}
