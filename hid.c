#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack_config.h"
#include "btstack.h"
#include "hid.h"

bool tud_suspended(void);
bool tud_remote_wakeup(void);
bool tud_mounted(void);

bool usb_hid_pointer_ready(void);
bool usb_hid_digitizer_ready(void);
bool usb_hid_keyboard_ready(void);
bool usb_hid_consumer_control_ready(void);
bool usb_hid_gamepad_ready(void);
bool usb_hid_send_pointer_report(uint8_t buttons, int8_t dx, int8_t dy);
bool usb_hid_send_digitizer_report(uint8_t switches, uint16_t x, uint16_t y);
bool usb_hid_send_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);
bool usb_hid_send_consumer_control_report(uint16_t keycode);
bool usb_hid_send_gamepad_report(uint16_t buttons, uint8_t hat);

#define POINTER_GAIN_X 3.0f
#define POINTER_GAIN_Y 1.25f

#define HID_CONSUMER_AC_HOME 0x0223
#define HID_CONSUMER_AC_BACK 0x0224
#define HID_CONSUMER_TV_INPUT 0x00B5
#define HID_CONSUMER_SLEEP 0x32
#define HID_CONSUMER_VOLUME_UP 0xE9
#define HID_CONSUMER_VOLUME_DOWN 0xEA
#define HID_CONSUMER_MUTE 0xE2

static hid_state_t *g_hid_state = NULL;

static void set_onboard_led(bool on) {
    if (g_hid_state->onboard_led_on == on) {
        return;
    }
    g_hid_state->onboard_led_on = on;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
}

void hid_request_usb_remote_wake_on_reconnect(void) {
    if (!tud_mounted()) {
        printf("USB remote wake on reconnect: skipped (not mounted)\n");
        return;
    }

    if (!tud_suspended()) {
        printf("USB remote wake on reconnect: skipped (host not suspended)\n");
        return;
    }

    bool requested = tud_remote_wakeup();
    printf("USB remote wake on reconnect: %s\n", requested ? "requested" : "not allowed");
}

static void send_hid_keyboard_report(const hid_state_t *state) {
    if (!state->connected || !usb_hid_keyboard_ready()) {
        return;
    }

    uint8_t keycodes[6] = {0};
    usb_hid_send_keyboard_report(0, keycodes);
}

static bool process_hid_sleep_signal(hid_state_t *state) {
    if (state->sleep_signal_stage == HID_SLEEP_SIGNAL_IDLE) {
        return false;
    }
    if (!usb_hid_consumer_control_ready()) {
        return true;
    }

    if (state->sleep_signal_stage == HID_SLEEP_SIGNAL_PRESS_PENDING) {
        usb_hid_send_consumer_control_report(HID_CONSUMER_SLEEP);
        state->sleep_signal_stage = HID_SLEEP_SIGNAL_RELEASE_PENDING;
        return true;
    }

    usb_hid_send_consumer_control_report(0);
    state->prev_consumer_keycode = 0;
    state->home_b_combo_latched = false;
    state->sleep_signal_stage = HID_SLEEP_SIGNAL_IDLE;
    printf("Sent HID Sleep press+release\n");
    return true;
}

static bool process_hid_wake_nudge(hid_state_t *state) {
    if (state->wake_nudge_stage == HID_WAKE_NUDGE_IDLE) {
        return false;
    }
    if (!state->connected || !usb_hid_pointer_ready()) {
        return true;
    }

    if (state->wake_nudge_stage == HID_WAKE_NUDGE_POSITIVE_PENDING) {
        usb_hid_send_pointer_report(0, 1, 0);
        state->wake_nudge_stage = HID_WAKE_NUDGE_NEGATIVE_PENDING;
        return true;
    }

    usb_hid_send_pointer_report(0, -1, 0);
    state->wake_nudge_stage = HID_WAKE_NUDGE_IDLE;
    printf("Sent HID wake nudge\n");
    return true;
}

static void reset_pointer_motion_state(hid_state_t *state) {
    state->pointer_had_tracking = false;
    state->pointer_prev_norm_x = 0;
    state->pointer_prev_norm_y = 0;
    state->pointer_motion_locked = false;
    state->pointer_prev_norm_valid = false;
    state->pointer_no_ir_frames = 0;
}

static void send_raw_pointer_report(hid_state_t *state, int8_t dx, int8_t dy, uint8_t buttons) {
    if (!state->connected || !usb_hid_pointer_ready()) {
        return;
    }
    usb_hid_send_pointer_report(buttons, dx, dy);
}

static void send_raw_digitizer_report(hid_state_t *state, uint8_t switches, uint16_t x, uint16_t y) {
    if (!state->connected || !usb_hid_digitizer_ready()) {
        return;
    }
    usb_hid_send_digitizer_report(switches, x, y);
}

static void send_raw_gamepad_report(hid_state_t *state, uint16_t buttons, uint8_t hat) {
    if (!state->connected || !usb_hid_gamepad_ready()) {
        return;
    }
    usb_hid_send_gamepad_report(buttons, hat);
}

static void send_raw_consumer_control_report(hid_state_t *state, uint16_t keycode) {
    if (!state->connected || !usb_hid_consumer_control_ready()) {
        return;
    }
    usb_hid_send_consumer_control_report(keycode);
}

static void hid_report_timer_callback(void) {
    if (g_hid_state != NULL) {
        if (process_hid_sleep_signal(g_hid_state)) {
            timer_manager_start(&g_hid_state->report_timer_manager, 0, 10);
            return;
        }

        if (process_hid_wake_nudge(g_hid_state)) {
            timer_manager_start(&g_hid_state->report_timer_manager, 0, 10);
            return;
        }

        // Keyboard always sent (currently empty)
        send_hid_keyboard_report(g_hid_state);

        timer_manager_start(&g_hid_state->report_timer_manager, 0, 10);
    }
}

void hid_set_connected(hid_state_t *state, bool connected) {
    state->connected = connected;
}

bool hid_is_connected(const hid_state_t *state) {
    return state->connected;
}

void hid_reset_output_state(hid_state_t *state) {
    reset_pointer_motion_state(state);
    state->prev_consumer_keycode = 0;
    state->home_b_combo_latched = false;
    state->prev_gamepad_hat = 0x08u;
    state->prev_gamepad_buttons = 0;
}

void hid_queue_sleep_signal(hid_state_t *state) {
    state->sleep_signal_stage = HID_SLEEP_SIGNAL_PRESS_PENDING;
    printf("Queued HID Sleep signal\n");
}

void hid_queue_wake_nudge(hid_state_t *state) {
    state->wake_nudge_stage = HID_WAKE_NUDGE_POSITIVE_PENDING;
    printf("Queued HID wake nudge\n");
}

void hid_set_output_mode(hid_state_t *state, hid_output_mode_t mode) {
    if (state->output_mode != mode) {
        state->output_mode = mode;
        reset_pointer_motion_state(state);
        if (mode == HID_MODE_POINTER) {
            printf("Switched to HID Pointer mode\n");
        } else {
            printf("Switched to HID Digitizer mode\n");
        }
    }
}

void hid_send_pointer_delta(hid_state_t *state, int8_t dx, int8_t dy, uint8_t buttons) {
    if (state->output_mode != HID_MODE_POINTER) {
        return;
    }
    send_raw_pointer_report(state, dx, dy, buttons);
}

void hid_set_gamepad_hat(hid_state_t *state, uint8_t hat, uint16_t buttons) {
    if (state->prev_gamepad_hat == hat && state->prev_gamepad_buttons == buttons) {
        return;
    }
    state->prev_gamepad_hat = hat;
    state->prev_gamepad_buttons = buttons;
    send_raw_gamepad_report(state, buttons, hat);
}

void hid_send_consumer_keycode(hid_state_t *state, uint16_t keycode) {
    if (keycode == state->prev_consumer_keycode) {
        return;
    }
    state->prev_consumer_keycode = keycode;
    send_raw_consumer_control_report(state, keycode);
}

void hid_init(hid_state_t *state) {
    g_hid_state = state;
    state->connected = false;
    state->output_mode = HID_MODE_POINTER;
    state->pointer_had_tracking = false;
    state->pointer_prev_norm_x = 0;
    state->pointer_prev_norm_y = 0;
    state->pointer_motion_locked = false;
    state->pointer_prev_norm_valid = false;
    state->prev_consumer_keycode = 0;
    state->home_b_combo_latched = false;
    state->prev_gamepad_hat = 0x08u;
    state->prev_gamepad_buttons = 0;
    state->pointer_no_ir_frames = 0;
    state->onboard_led_on = false;
    state->sleep_signal_stage = HID_SLEEP_SIGNAL_IDLE;
    state->wake_nudge_stage = HID_WAKE_NUDGE_IDLE;

    state->report_timer_slot.callback = hid_report_timer_callback;
    timer_manager_init(&state->report_timer_manager, &state->report_timer_slot, 1);
    timer_manager_start(&state->report_timer_manager, 0, 10);
}

hid_state_t *hid_get_state(void) {
    return g_hid_state;
}
