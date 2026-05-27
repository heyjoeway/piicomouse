#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack_config.h"
#include "btstack.h"

#define INQUIRY_SECONDS 5
#define BUTTON_PRINT_PERIOD_MS 200
#define MAX_ATTRIBUTE_VALUE_SIZE 300
#define WIIMOTE_IR_POINTS 4

static const char *wii_name_prefix = "Nintendo RVL-CNT-01";
static const char *wii_target_addr_string = "00:1E:35:40:63:9F";

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t button_print_timer;
static btstack_timer_source_t ir_init_timer;

static uint8_t hid_descriptor_storage[MAX_ATTRIBUTE_VALUE_SIZE];

static hid_protocol_mode_t hid_host_report_mode = HID_PROTOCOL_MODE_REPORT;
static uint16_t hid_host_cid;
static bool hid_connected;
static bool pending_connect;
static bool have_button_state;
static uint16_t current_buttons;
static bool have_ir_state;
static bool have_ir_center;
static bool seen_report_id[256];
static bool ir_init_in_progress;
static uint8_t ir_init_step;
static uint8_t ir_debug_frames_remaining;
static bool ir_report_mode_active;
static uint8_t ir_mode_index;
static uint8_t ir_mode_retry_count;
static uint8_t ir_sensitivity_index = 2;
static uint16_t ir_center_x;
static uint16_t ir_center_y;
static uint16_t ir_points_x[WIIMOTE_IR_POINTS];
static uint16_t ir_points_y[WIIMOTE_IR_POINTS];
static uint8_t ir_points_size[WIIMOTE_IR_POINTS];
static bool ir_points_valid[WIIMOTE_IR_POINTS];
static uint16_t previous_buttons;
static bd_addr_t target_addr;
static bool target_addr_configured;
static bool wii_pin_use_reversed = true;

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
        case BTSTACK_MEMORY_ALLOC_FAILED:
            return "BTSTACK_MEMORY_ALLOC_FAILED";
        case L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY:
            return "L2CAP_REFUSED_SECURITY";
        default:
            return "UNKNOWN";
    }
}

static void start_scan(void) {
    if (pending_connect || hid_connected) {
        return;
    }
    printf("Starting inquiry. Put Wii Remote into discoverable mode (press 1+2).\n");
    gap_inquiry_start(INQUIRY_SECONDS);
}

static bool is_wii_name(const char *name) {
    return strncmp(name, wii_name_prefix, strlen(wii_name_prefix)) == 0;
}

static bool is_target_addr(const bd_addr_t addr) {
    return target_addr_configured && (bd_addr_cmp(addr, target_addr) == 0);
}

static void respond_wii_pin_code(const bd_addr_t remote_addr) {
    bd_addr_t local_addr;
    uint8_t pin_data[6];

    gap_local_bd_addr(local_addr);
    for (int i = 0; i < 6; i++) {
        pin_data[i] = wii_pin_use_reversed ? local_addr[5 - i] : local_addr[i];
    }

    printf("PIN requested by target Wii Remote %s, replying with %s local BD_ADDR bytes\n",
           bd_addr_to_str(remote_addr),
           wii_pin_use_reversed ? "reversed" : "forward");

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
    if (buttons & 0x8000) append_button(text, sizeof(text), &first, "Home");
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

    hid_host_send_set_report(hid_host_cid, HID_REPORT_TYPE_OUTPUT, 0x16, payload, (uint16_t)(len + 5));
}

static uint8_t wiimote_send_output_report(uint16_t report_id, const uint8_t *data, uint16_t len) {
    uint8_t status = hid_host_send_set_report(hid_host_cid, HID_REPORT_TYPE_OUTPUT, report_id, data, len);
    if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED) {
        printf("Output report 0x%02x send failed (0x%02x)\n", report_id, status);
    }
    return status;
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

static void select_ir_sensitivity_profile(uint8_t new_index) {
    const uint8_t profile_count = (uint8_t)(sizeof(wiimote_ir_profiles) / sizeof(wiimote_ir_profiles[0]));
    ir_sensitivity_index = new_index % profile_count;
    printf("IR sensitivity profile: %s\n", wiimote_ir_profiles[ir_sensitivity_index].name);
}

static void request_wiimote_ir_report(void) {
    ir_init_in_progress = true;
    ir_init_step = WIIMOTE_IR_INIT_ENABLE_1;
    ir_debug_frames_remaining = 24;
    ir_report_mode_active = false;
    ir_mode_index = 0;
    ir_mode_retry_count = 0;
    printf("Applying IR profile: %s\n", wiimote_ir_profiles[ir_sensitivity_index].name);
    btstack_run_loop_set_timer(&ir_init_timer, 10);
    btstack_run_loop_add_timer(&ir_init_timer);
}

static void wiimote_ir_init_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;

    if (!hid_connected || !ir_init_in_progress) {
        return;
    }

    uint8_t ir_enable_payload[1] = {0x04};
    uint8_t mode_payload[2] = {0x04, 0x33};
    const wiimote_ir_sensitivity_profile_t *profile = &wiimote_ir_profiles[ir_sensitivity_index];
    uint8_t ir_mode_ext[1] = {0x03};

    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;

    switch (ir_init_step) {
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
            mode_payload[1] = wiimote_ir_mode_candidates[ir_mode_index];
            status = wiimote_send_output_report(0x12, mode_payload, sizeof(mode_payload));
            break;
        default:
            return;
    }

    if (ir_init_step == WIIMOTE_IR_INIT_REPORT_MODE_SELECT) {
        if (ir_report_mode_active) {
            ir_init_step = WIIMOTE_IR_INIT_DONE;
        } else if (status == ERROR_CODE_SUCCESS || status == ERROR_CODE_COMMAND_DISALLOWED) {
            ir_mode_retry_count++;
            if (ir_mode_retry_count >= 8) {
                ir_mode_retry_count = 0;
                ir_mode_index++;
                if (ir_mode_index < (uint8_t)(sizeof(wiimote_ir_mode_candidates) / sizeof(wiimote_ir_mode_candidates[0]))) {
                    printf("IR mode 0x%02x not active yet, trying 0x%02x\n",
                           wiimote_ir_mode_candidates[ir_mode_index - 1],
                           wiimote_ir_mode_candidates[ir_mode_index]);
                } else {
                    printf("IR mode negotiation failed (0x33/0x36/0x37)\n");
                    ir_init_in_progress = false;
                    return;
                }
            }
        }
    } else {
        if (status == ERROR_CODE_SUCCESS) {
            ir_init_step++;
        } else if (status != ERROR_CODE_COMMAND_DISALLOWED) {
            printf("IR init step %u send status 0x%02x\n", ir_init_step, status);
        }
    }

    if (ir_init_step >= WIIMOTE_IR_INIT_DONE) {
        ir_init_in_progress = false;
        printf("Wii IR init sequence complete\n");
        return;
    }

    btstack_run_loop_set_timer(&ir_init_timer, ir_init_step == WIIMOTE_IR_INIT_REPORT_MODE_SELECT ? 120 : 40);
    btstack_run_loop_add_timer(&ir_init_timer);
}

static void parse_wiimote_ir_extended(const uint8_t *ir_data, uint16_t ir_len) {
    if (ir_len < 12) {
        return;
    }

    int valid_count = 0;
    uint32_t sum_x = 0;
    uint32_t sum_y = 0;

    for (int i = 0; i < WIIMOTE_IR_POINTS; i++) {
        const uint8_t x_low = ir_data[i * 3];
        const uint8_t y_low = ir_data[i * 3 + 1];
        const uint8_t xy_hi = ir_data[i * 3 + 2];

        const uint16_t x = (uint16_t)x_low | (uint16_t)(((xy_hi >> 4) & 0x03u) << 8);
        const uint16_t y = (uint16_t)y_low | (uint16_t)(((xy_hi >> 6) & 0x03u) << 8);
        const uint8_t size = xy_hi & 0x0Fu;

        const bool valid = !((x == 1023u) && (y == 1023u));

        ir_points_valid[i] = valid;
        ir_points_x[i] = x;
        ir_points_y[i] = y;
        ir_points_size[i] = size;

        if (valid) {
            sum_x += x;
            sum_y += y;
            valid_count++;
        }
    }

    have_ir_state = true;
    if (valid_count > 0) {
        ir_center_x = (uint16_t)(sum_x / (uint32_t)valid_count);
        ir_center_y = (uint16_t)(sum_y / (uint32_t)valid_count);
        have_ir_center = true;
    } else {
        have_ir_center = false;
    }
}

static void handle_ir_profile_hotkeys(uint16_t buttons) {
    uint16_t changed = buttons ^ previous_buttons;
    bool home_down = (buttons & 0x8000u) != 0;

    if (!home_down) {
        previous_buttons = buttons;
        return;
    }

    // Home+Up / Home+Down cycles sensitivity profiles live.
    if ((changed & 0x0800u) && (buttons & 0x0800u)) {
        select_ir_sensitivity_profile((uint8_t)(ir_sensitivity_index + 1));
        if (hid_connected) {
            request_wiimote_ir_report();
        }
    } else if ((changed & 0x0400u) && (buttons & 0x0400u)) {
        const uint8_t profile_count = (uint8_t)(sizeof(wiimote_ir_profiles) / sizeof(wiimote_ir_profiles[0]));
        select_ir_sensitivity_profile((uint8_t)((ir_sensitivity_index + profile_count - 1) % profile_count));
        if (hid_connected) {
            request_wiimote_ir_report();
        }
    }

    previous_buttons = buttons;
}

static void parse_wiimote_report(const uint8_t *report, uint16_t report_len) {
    if (report_len < 4) {
        return;
    }
    if (report[0] != 0xA1) {
        return;
    }

    uint8_t report_id = report[1];
    if (!seen_report_id[report_id]) {
        seen_report_id[report_id] = true;
        printf("Observed input report 0x%02x (len=%u)\n", report_id, report_len);
    }

    if (report_id == 0x22 && report_len >= 4) {
        printf("Output report ack: id=0x%02x result=0x%02x\n", report[2], report[3]);
    }

    if (ir_init_in_progress &&
        report_id == wiimote_ir_mode_candidates[ir_mode_index]) {
        ir_report_mode_active = true;
        printf("IR report mode active: 0x%02x\n", report_id);
    }

    if (report_id < 0x30 || report_id > 0x3F) {
        return;
    }

    current_buttons = ((uint16_t)report[2] << 8) | report[3];
    have_button_state = true;
    handle_ir_profile_hotkeys(current_buttons);

    if (report_id == 0x33 && report_len >= 19) {
        if (ir_debug_frames_remaining > 0) {
            ir_debug_frames_remaining--;
            printf("IR raw: %02x %02x %02x  %02x %02x %02x  %02x %02x %02x  %02x %02x %02x\n",
                   report[7], report[8], report[9],
                   report[10], report[11], report[12],
                   report[13], report[14], report[15],
                   report[16], report[17], report[18]);
        }
        parse_wiimote_ir_extended(&report[7], (uint16_t)(report_len - 7));
    }
}

static void button_print_timer_handler(btstack_timer_source_t *ts) {
    (void)ts;

    if (hid_connected && have_button_state) {
        printf("Buttons: %s\n", buttons_to_string(current_buttons));
    }

    if (hid_connected && have_ir_state) {
        bool printed_any = false;
        printf("IR:");
        for (int i = 0; i < WIIMOTE_IR_POINTS; i++) {
            if (!ir_points_valid[i]) {
                continue;
            }
            printf(" p%d=(%u,%u,s%u)", i, ir_points_x[i], ir_points_y[i], ir_points_size[i]);
            printed_any = true;
        }
        if (!printed_any) {
            printf(" (no blobs)");
        }
        if (have_ir_center) {
            printf(" center=(%u,%u)", ir_center_x, ir_center_y);
        }
        printf("\n");
    }

    btstack_run_loop_set_timer(&button_print_timer, BUTTON_PRINT_PERIOD_MS);
    btstack_run_loop_add_timer(&button_print_timer);
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
            if (hid_connected || pending_connect) {
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

            printf("Inquiry: %s  cod=0x%06lx", bd_addr_to_str(addr),
                   (unsigned long)gap_event_inquiry_result_get_class_of_device(packet));
            if (has_rssi) {
                printf("  rssi=%d", rssi);
            }
            printf("  name='%s'\n", name);

            if (!is_target_addr(addr) && (!has_name || !is_wii_name(name))) {
                break;
            }

            if (!is_target_addr(addr)) {
                memcpy(target_addr, addr, sizeof(bd_addr_t));
                target_addr_configured = true;
            }
            pending_connect = true;

            printf("Target Wii Remote candidate at %s (name='%s')\n", bd_addr_to_str(target_addr), name);
            printf("Stopping inquiry and connecting...\n");
            gap_inquiry_stop();
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            if (pending_connect) {
                pending_connect = false;
                uint8_t status = hid_host_connect(target_addr, hid_host_report_mode, &hid_host_cid);
                if (status != ERROR_CODE_SUCCESS) {
                    printf("HID connect failed (0x%02x), retrying inquiry\n", status);
                    start_scan();
                }
            } else if (!hid_connected) {
                start_scan();
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            if (is_target_addr(addr)) {
                respond_wii_pin_code(addr);
            } else {
                printf("PIN requested by %s, responding with 0000\n", bd_addr_to_str(addr));
                gap_pin_code_response(addr, "0000");
            }
            break;

        case HCI_EVENT_HID_META: {
            uint8_t subevent = hci_event_hid_meta_get_subevent_code(packet);
            switch (subevent) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    hid_host_accept_connection(
                        hid_subevent_incoming_connection_get_hid_cid(packet),
                        hid_host_report_mode);
                    printf("Accepting incoming HID connection\n");
                    break;

                case HID_SUBEVENT_CONNECTION_OPENED: {
                    uint8_t status = hid_subevent_connection_opened_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS) {
                        printf("HID connection failed (0x%02x: %s)\n", status, error_code_to_string(status));
                        if (status == L2CAP_CONNECTION_RESPONSE_RESULT_REFUSED_SECURITY) {
                            wii_pin_use_reversed = !wii_pin_use_reversed;
                            printf("Security refusal: next attempt will use %s local BD_ADDR PIN byte order\n",
                                   wii_pin_use_reversed ? "reversed" : "forward");
                        }
                        hid_connected = false;
                        hid_host_cid = 0;
                        start_scan();
                        break;
                    }

                    hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
                    hid_connected = true;
                    have_button_state = false;
                    current_buttons = 0;
                    have_ir_state = false;
                    have_ir_center = false;
                    ir_init_in_progress = false;
                    ir_init_step = WIIMOTE_IR_INIT_ENABLE_1;
                    ir_debug_frames_remaining = 0;
                    previous_buttons = 0;
                    memset(seen_report_id, 0, sizeof(seen_report_id));
                    printf("Wii Remote connected\n");
                    break;
                }

                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    uint8_t status = hid_subevent_descriptor_available_get_status(packet);
                    if (status == ERROR_CODE_SUCCESS) {
                        printf("HID descriptor ready, enabling IR report mode\n");
                        request_wiimote_ir_report();
                    } else {
                        printf("Descriptor unavailable (0x%02x)\n", status);
                    }
                    break;
                }

                case HID_SUBEVENT_REPORT:
                    parse_wiimote_report(
                        hid_subevent_report_get_report(packet),
                        hid_subevent_report_get_report_len(packet));
                    break;

                case HID_SUBEVENT_SET_REPORT_RESPONSE: {
                    uint8_t hs = hid_subevent_set_report_response_get_handshake_status(packet);
                    if (hs != HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        printf("SET_REPORT handshake error (step %u): 0x%02x\n", ir_init_step, hs);
                    }
                    break;
                }

                case HID_SUBEVENT_CONNECTION_CLOSED:
                    printf("Wii Remote disconnected\n");
                    hid_connected = false;
                    hid_host_cid = 0;
                    have_button_state = false;
                    have_ir_state = false;
                    have_ir_center = false;
                    ir_init_in_progress = false;
                    start_scan();
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

static void hid_host_setup(void) {
    l2cap_init();
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);

    // Wii Remotes use legacy PIN pairing and can fail with SSP enabled.
    gap_ssp_set_enable(0);

    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    gap_discoverable_control(1);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    btstack_run_loop_set_timer_handler(&ir_init_timer, wiimote_ir_init_timer_handler);
}

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    printf("Pi Pico W 2 Wii Remote HID host proof-of-concept\n");
    printf("Using report mode + legacy PIN pairing (SSP disabled)\n");

    sscanf_bd_addr(wii_target_addr_string, target_addr);
    target_addr_configured = true;
    printf("Using hard-coded target address: %s\n", wii_target_addr_string);

    if (cyw43_arch_init() != 0) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    hid_host_setup();

    btstack_run_loop_set_timer_handler(&button_print_timer, button_print_timer_handler);
    btstack_run_loop_set_timer(&button_print_timer, BUTTON_PRINT_PERIOD_MS);
    btstack_run_loop_add_timer(&button_print_timer);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
