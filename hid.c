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
#define NO_IR_ENTER_THRESHOLD_FRAMES 5

#define HID_CONSUMER_AC_HOME 0x0223
#define HID_CONSUMER_AC_BACK 0x0224
#define HID_CONSUMER_TV_INPUT 0x00B5
#define HID_CONSUMER_SLEEP 0x32
#define HID_CONSUMER_VOLUME_UP 0xE9
#define HID_CONSUMER_VOLUME_DOWN 0xEA
#define HID_CONSUMER_MUTE 0xE2

typedef enum {
    HID_MODE_POINTER = 0,
    HID_MODE_DIGITIZER = 1,
} hid_output_mode_t;

typedef enum {
    HID_SLEEP_SIGNAL_IDLE = 0,
    HID_SLEEP_SIGNAL_PRESS_PENDING,
    HID_SLEEP_SIGNAL_RELEASE_PENDING,
} hid_sleep_signal_stage_t;

typedef enum {
    HID_WAKE_NUDGE_IDLE = 0,
    HID_WAKE_NUDGE_POSITIVE_PENDING,
    HID_WAKE_NUDGE_NEGATIVE_PENDING,
} hid_wake_nudge_stage_t;

static wiimote_tracking_state_t *hid_state;
static btstack_timer_source_t hid_report_timer;
static hid_output_mode_t hid_output_mode = HID_MODE_POINTER;
static bool hid_connected;
static bool pointer_had_tracking;
static uint16_t pointer_prev_norm_x;
static uint16_t pointer_prev_norm_y;
static bool pointer_motion_locked;
static bool pointer_prev_norm_valid;
static uint16_t prev_consumer_keycode;
static bool home_b_combo_latched;
static uint8_t prev_gamepad_hat = 0x08u;
static uint16_t prev_gamepad_buttons;
static uint8_t pointer_no_ir_frames;
static bool onboard_led_on;
static hid_sleep_signal_stage_t hid_sleep_signal_stage = HID_SLEEP_SIGNAL_IDLE;
static hid_wake_nudge_stage_t hid_wake_nudge_stage = HID_WAKE_NUDGE_IDLE;

static void set_onboard_led(bool on) {
    if (onboard_led_on == on) {
        return;
    }
    onboard_led_on = on;
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

static void send_hid_keyboard_report(const wiimote_tracking_state_t *state) {
    if (!hid_connected || !usb_hid_keyboard_ready()) {
        return;
    }

    (void)state;
    uint8_t keycodes[6] = {0};
    usb_hid_send_keyboard_report(0, keycodes);
}

static bool process_hid_sleep_signal(void) {
    if (hid_sleep_signal_stage == HID_SLEEP_SIGNAL_IDLE) {
        return false;
    }
    if (!usb_hid_consumer_control_ready()) {
        return true;
    }

    if (hid_sleep_signal_stage == HID_SLEEP_SIGNAL_PRESS_PENDING) {
        usb_hid_send_consumer_control_report(HID_CONSUMER_SLEEP);
        hid_sleep_signal_stage = HID_SLEEP_SIGNAL_RELEASE_PENDING;
        return true;
    }

    usb_hid_send_consumer_control_report(0);
    prev_consumer_keycode = 0;
    home_b_combo_latched = false;
    hid_sleep_signal_stage = HID_SLEEP_SIGNAL_IDLE;
    printf("Sent HID Sleep press+release\n");
    return true;
}

static bool process_hid_wake_nudge(void) {
    if (hid_wake_nudge_stage == HID_WAKE_NUDGE_IDLE) {
        return false;
    }
    if (!hid_connected || !usb_hid_pointer_ready()) {
        return true;
    }

    if (hid_wake_nudge_stage == HID_WAKE_NUDGE_POSITIVE_PENDING) {
        usb_hid_send_pointer_report(0, 1, 0);
        hid_wake_nudge_stage = HID_WAKE_NUDGE_NEGATIVE_PENDING;
        return true;
    }

    usb_hid_send_pointer_report(0, -1, 0);
    hid_wake_nudge_stage = HID_WAKE_NUDGE_IDLE;
    printf("Sent HID wake nudge\n");
    return true;
}

static bool pointer_should_send_enter_with_a(void) {
    return (hid_output_mode == HID_MODE_POINTER) && (pointer_no_ir_frames >= NO_IR_ENTER_THRESHOLD_FRAMES);
}

static void reset_pointer_motion_state(void) {
    pointer_had_tracking = false;
    pointer_prev_norm_x = 0;
    pointer_prev_norm_y = 0;
    pointer_motion_locked = false;
    pointer_prev_norm_valid = false;
    pointer_no_ir_frames = 0;
}

static void send_hid_pointer_report(const wiimote_tracking_state_t *state) {
    if (!hid_connected || !usb_hid_pointer_ready()) {
        return;
    }

    uint8_t buttons = 0;
    if ((state->buttons & 0x0008u) && !pointer_should_send_enter_with_a()) {
        buttons |= 0x01;
    }

    bool movement_lock_requested = (buttons != 0) && ((state->buttons & 0x0004u) == 0);

    if (!state->have_norm) {
        if (pointer_had_tracking) {
            usb_hid_send_pointer_report(0, 0, 0);
            pointer_had_tracking = false;
            pointer_prev_norm_valid = false;
        }
        return;
    }

    if (!pointer_prev_norm_valid) {
        pointer_prev_norm_x = state->norm_x;
        pointer_prev_norm_y = state->norm_y;
        pointer_prev_norm_valid = true;
        pointer_had_tracking = true;
        usb_hid_send_pointer_report(buttons, 0, 0);
        return;
    }

    int32_t delta_x = (int32_t)state->norm_x - (int32_t)pointer_prev_norm_x;
    int32_t delta_y = (int32_t)state->norm_y - (int32_t)pointer_prev_norm_y;

    pointer_prev_norm_x = state->norm_x;
    pointer_prev_norm_y = state->norm_y;
    pointer_had_tracking = true;

    if (movement_lock_requested && !pointer_motion_locked) {
        pointer_motion_locked = true;
    } else if (!movement_lock_requested) {
        pointer_motion_locked = false;
    }

    if (pointer_motion_locked) {
        usb_hid_send_pointer_report(buttons, 0, 0);
        return;
    }

    delta_x = -delta_x * POINTER_GAIN_X;
    delta_y = delta_y * POINTER_GAIN_Y;

    if (delta_x < -127) delta_x = -127;
    if (delta_x > 127) delta_x = 127;
    if (delta_y < -127) delta_y = -127;
    if (delta_y > 127) delta_y = 127;

    usb_hid_send_pointer_report(buttons, (int8_t)delta_x, (int8_t)delta_y);
}

static void send_hid_digitizer_report(const wiimote_tracking_state_t *state) {
    if (!hid_connected || !usb_hid_digitizer_ready()) {
        return;
    }

    uint8_t switches = 0;
    if (state->buttons & 0x0008) switches |= 0x01;
    if (state->buttons & 0x0010) switches |= 0x02;

    uint16_t x = state->have_norm ? state->norm_x : 500;
    uint16_t y = state->have_norm ? state->norm_y : 500;

    usb_hid_send_digitizer_report(switches, x, y);
}

static void send_hid_gamepad_report(const wiimote_tracking_state_t *state) {
    if (!hid_connected || !usb_hid_gamepad_ready()) {
        return;
    }

    uint16_t gamepad_buttons = 0;
    if ((state->buttons & 0x0008u) && pointer_should_send_enter_with_a()) {
        gamepad_buttons |= 0x0001u;
    }

    uint8_t hat = 0x08u;
    bool up = (state->buttons & 0x0800u) != 0;
    bool down = (state->buttons & 0x0400u) != 0;
    bool left = (state->buttons & 0x0100u) != 0;
    bool right = (state->buttons & 0x0200u) != 0;

    if (up && !down) {
        if (left && !right) {
            hat = 7;
        } else if (right && !left) {
            hat = 1;
        } else {
            hat = 0;
        }
    } else if (down && !up) {
        if (left && !right) {
            hat = 5;
        } else if (right && !left) {
            hat = 3;
        } else {
            hat = 4;
        }
    } else if (left && !right) {
        hat = 6;
    } else if (right && !left) {
        hat = 2;
    }

    if (hat == prev_gamepad_hat && gamepad_buttons == prev_gamepad_buttons) {
        return;
    }

    prev_gamepad_hat = hat;
    prev_gamepad_buttons = gamepad_buttons;
    usb_hid_send_gamepad_report(gamepad_buttons, hat);
}

static void send_hid_consumer_control_report(const wiimote_tracking_state_t *state) {
    if (!hid_connected || !usb_hid_consumer_control_ready()) {
        return;
    }

    bool home_down = (state->buttons & 0x0080u) != 0;
    bool b_down = (state->buttons & 0x0004u) != 0;
    uint16_t keycode = 0;

    if (!home_down) {
        home_b_combo_latched = false;
    }

    if (home_down && b_down) {
        home_b_combo_latched = true;
        keycode = HID_CONSUMER_TV_INPUT;
    } else if (home_down && !home_b_combo_latched) {
        keycode = HID_CONSUMER_AC_HOME;
    } else if (state->buttons & 0x0010u) {
        keycode = HID_CONSUMER_AC_BACK;
    } else if (state->buttons & 0x0002u) {
        keycode = HID_CONSUMER_VOLUME_UP;
    } else if (state->buttons & 0x0001u) {
        keycode = HID_CONSUMER_VOLUME_DOWN;
    } else if (state->buttons & 0x1000u) {
        keycode = HID_CONSUMER_MUTE;
    }

    if (keycode == prev_consumer_keycode) {
        return;
    }

    prev_consumer_keycode = keycode;
    usb_hid_send_consumer_control_report(keycode);
}

static void hid_report_timer_handler_state(const wiimote_tracking_state_t *state, btstack_timer_source_t *ts) {
    (void)ts;

    if (process_hid_sleep_signal()) {
        btstack_run_loop_set_timer(&hid_report_timer, 10);
        btstack_run_loop_add_timer(&hid_report_timer);
        return;
    }

    if (process_hid_wake_nudge()) {
        btstack_run_loop_set_timer(&hid_report_timer, 10);
        btstack_run_loop_add_timer(&hid_report_timer);
        return;
    }

    if (hid_connected) {
        send_hid_gamepad_report(state);
        send_hid_keyboard_report(state);
        send_hid_consumer_control_report(state);
        if (hid_output_mode == HID_MODE_POINTER) {
            send_hid_pointer_report(state);
        } else if (hid_output_mode == HID_MODE_DIGITIZER) {
            send_hid_digitizer_report(state);
        }
    }

    btstack_run_loop_set_timer(&hid_report_timer, 10);
    btstack_run_loop_add_timer(&hid_report_timer);
}

static void hid_report_timer_handler(btstack_timer_source_t *ts) {
    if (hid_state != NULL) {
        hid_report_timer_handler_state(hid_state, ts);
    }
}

void hid_set_connected(bool connected) {
    hid_connected = connected;
}

bool hid_is_connected(void) {
    return hid_connected;
}

void hid_reset_output_state(void) {
    reset_pointer_motion_state();
    prev_consumer_keycode = 0;
    home_b_combo_latched = false;
    prev_gamepad_hat = 0x08u;
    prev_gamepad_buttons = 0;
}

void hid_queue_sleep_signal(void) {
    hid_sleep_signal_stage = HID_SLEEP_SIGNAL_PRESS_PENDING;
    printf("Queued HID Sleep signal\n");
}

void hid_queue_wake_nudge(void) {
    hid_wake_nudge_stage = HID_WAKE_NUDGE_POSITIVE_PENDING;
    printf("Queued HID wake nudge\n");
}

void hid_handle_mode_hotkeys(wiimote_tracking_state_t *state, uint16_t buttons) {
    uint16_t changed = buttons ^ state->previous_buttons_hid_mode;
    bool b_down = (buttons & 0x0004u) != 0;

    if (!b_down) {
        state->previous_buttons_hid_mode = buttons;
        return;
    }

    if ((changed & 0x0002u) && (buttons & 0x0002u)) {
        if (hid_output_mode != HID_MODE_POINTER) {
            hid_output_mode = HID_MODE_POINTER;
            printf("Switched to HID Pointer mode\n");
        }
    } else if ((changed & 0x0001u) && (buttons & 0x0001u)) {
        if (hid_output_mode != HID_MODE_DIGITIZER) {
            hid_output_mode = HID_MODE_DIGITIZER;
            printf("Switched to HID Digitizer mode\n");
        }
    }

    state->previous_buttons_hid_mode = buttons;
}

void hid_init(wiimote_tracking_state_t *state) {
    hid_state = state;
    btstack_run_loop_set_timer_handler(&hid_report_timer, hid_report_timer_handler);
    btstack_run_loop_set_timer(&hid_report_timer, 10);
    btstack_run_loop_add_timer(&hid_report_timer);
}