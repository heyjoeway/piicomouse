#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack.h"
#include "wiimote.h"
#include "hid.h"
#include "bootsel.h"

#define BOOTSEL_POLL_PERIOD_MS 1000

// Global state objects
static wiimote_tracking_state_t wiimote_state = {0};
static hid_state_t hid_state = {0};

typedef enum {
    APP_PROFILE_WIIMOTE_DEFAULT = 0,
    APP_PROFILE_WIIMOTE_TEST2,
    APP_PROFILE_WIIMOTE_TEST3,
    APP_PROFILE_WIIMOTE_TEST4,
    APP_PROFILE_COUNT,
} app_profile_id_t;

typedef struct {
    const char *name;
    wiimote_behavior_profile_t handler;
} app_profile_entry_t;

// Behavior profile: Default Wii Remote to HID mapping
#define POINTER_GAIN_X 3.0f
#define POINTER_GAIN_Y 1.25f
#define NO_IR_ENTER_THRESHOLD_FRAMES 5
#define WIIMOTE_BUTTON_MASK 0x1F9Fu
#define WIIMOTE_HOME_MASK 0x0080u

static bool profile_pointer_prev_norm_valid;
static uint16_t profile_pointer_prev_norm_x;
static uint16_t profile_pointer_prev_norm_y;
static uint8_t profile_pointer_no_ir_frames;
static bool profile_home_tap_candidate;
static bool profile_home_tap_prev_down;
static bool profile_home_tap_press_pending;
static bool profile_home_tap_release_pending;
static app_profile_id_t active_profile_id = APP_PROFILE_WIIMOTE_DEFAULT;

static void profile_update_home_tap_state(uint16_t buttons) {
    bool home_down = (buttons & WIIMOTE_HOME_MASK) != 0;
    bool non_home_down = (buttons & (WIIMOTE_BUTTON_MASK & ~WIIMOTE_HOME_MASK)) != 0;

    if (home_down && !profile_home_tap_prev_down) {
        profile_home_tap_candidate = !non_home_down;
    }

    if (home_down && non_home_down) {
        profile_home_tap_candidate = false;
    }

    if (!home_down && profile_home_tap_prev_down) {
        if (profile_home_tap_candidate) {
            profile_home_tap_press_pending = true;
            profile_home_tap_release_pending = false;
        }
        profile_home_tap_candidate = false;
    }

    profile_home_tap_prev_down = home_down;
}

static void profile_wiimote_default(wiimote_tracking_state_t *wiimote);
static void profile_wiimote_test2(wiimote_tracking_state_t *wiimote);
static void profile_wiimote_test3(wiimote_tracking_state_t *wiimote);
static void profile_wiimote_test4(wiimote_tracking_state_t *wiimote);

static const app_profile_entry_t app_profiles[APP_PROFILE_COUNT] = {
    [APP_PROFILE_WIIMOTE_DEFAULT] = {.name = "profile_wiimote_default", .handler = profile_wiimote_default},
    [APP_PROFILE_WIIMOTE_TEST2] = {.name = "profile_wiimote_test2", .handler = profile_wiimote_test2},
    [APP_PROFILE_WIIMOTE_TEST3] = {.name = "profile_wiimote_test3", .handler = profile_wiimote_test3},
    [APP_PROFILE_WIIMOTE_TEST4] = {.name = "profile_wiimote_test4", .handler = profile_wiimote_test4},
};

static void reset_profile_runtime_state(void) {
    profile_pointer_prev_norm_valid = false;
    profile_pointer_prev_norm_x = 0;
    profile_pointer_prev_norm_y = 0;
    profile_pointer_no_ir_frames = 0;
    profile_home_tap_candidate = false;
    profile_home_tap_prev_down = false;
    profile_home_tap_press_pending = false;
    profile_home_tap_release_pending = false;
    wiimote_state.previous_buttons_hid_mode = wiimote_state.buttons;
    hid_reset_output_state(&hid_state);
    hid_send_consumer_keycode(&hid_state, 0);
    hid_set_gamepad_hat(&hid_state, 0x08u, 0);
    hid_send_pointer_delta(&hid_state, 0, 0, 0);
}

static void set_active_profile(app_profile_id_t profile_id, bool persist) {
    bool saved = true;

    if (profile_id >= APP_PROFILE_COUNT) {
        return;
    }

    if (profile_id != active_profile_id) {
        active_profile_id = profile_id;
        wiimote_set_behavior_profile(app_profiles[profile_id].handler);
        reset_profile_runtime_state();
    }

    if (persist) {
        saved = wiimote_save_persisted_profile_id((uint8_t)profile_id);
    }

    printf("Active profile: %s%s\n",
           app_profiles[profile_id].name,
           persist ? (saved ? " (saved)" : " (save failed)") : "");
}

static void handle_sync_pair_complete(void) {
    set_active_profile(APP_PROFILE_WIIMOTE_DEFAULT, true);
}

static bool handle_profile_selection_hotkeys(wiimote_tracking_state_t *wiimote) {
    const uint16_t buttons = wiimote->buttons;
    const uint16_t changed = buttons ^ wiimote->previous_buttons;
    const bool home_down = (buttons & 0x0080u) != 0;
    const bool dpad_down = (buttons & (0x0800u | 0x0400u | 0x0100u | 0x0200u)) != 0;
    app_profile_id_t selected_profile = APP_PROFILE_COUNT;

    if (home_down) {
        if ((changed & 0x0800u) && (buttons & 0x0800u)) {
            selected_profile = APP_PROFILE_WIIMOTE_DEFAULT;
        } else if ((changed & 0x0400u) && (buttons & 0x0400u)) {
            selected_profile = APP_PROFILE_WIIMOTE_TEST2;
        } else if ((changed & 0x0100u) && (buttons & 0x0100u)) {
            selected_profile = APP_PROFILE_WIIMOTE_TEST3;
        } else if ((changed & 0x0200u) && (buttons & 0x0200u)) {
            selected_profile = APP_PROFILE_WIIMOTE_TEST4;
        }
    }

    wiimote->previous_buttons = buttons;

    if (selected_profile < APP_PROFILE_COUNT) {
        set_active_profile(selected_profile, true);
    }

    if (home_down && dpad_down) {
        hid_send_consumer_keycode(&hid_state, 0);
        hid_set_gamepad_hat(&hid_state, 0x08u, 0);
        hid_send_pointer_delta(&hid_state, 0, 0, 0);
        return true;
    }

    return false;
}

static uint8_t profile_buttons_to_hat(uint16_t buttons) {
    bool up = (buttons & 0x0800u) != 0;
    bool down = (buttons & 0x0400u) != 0;
    bool left = (buttons & 0x0100u) != 0;
    bool right = (buttons & 0x0200u) != 0;

    if (up && !down) {
        if (left && !right) return 7;
        if (right && !left) return 1;
        return 0;
    }
    if (down && !up) {
        if (left && !right) return 5;
        if (right && !left) return 3;
        return 4;
    }
    if (left && !right) return 6;
    if (right && !left) return 2;
    return 0x08u;
}

static void bootsel_poll_callback(bool pressed, bool changed) {
    if (changed) printf("BOOTSEL: %s\n", pressed ? "pressed" : "released");
    if (pressed) wiimote_enter_sync_mode();
}

static void profile_wiimote_default(wiimote_tracking_state_t *wiimote) {
    profile_update_home_tap_state(wiimote->buttons);

    if (handle_profile_selection_hotkeys(wiimote)) {
        return;
    }

    if (!hid_is_connected(&hid_state)) {
        profile_pointer_prev_norm_valid = false;
        profile_pointer_no_ir_frames = 0;
        return;
    }

    uint16_t changed = wiimote->buttons ^ wiimote->previous_buttons_hid_mode;
    bool b_down = (wiimote->buttons & 0x0004u) != 0;

    if (!b_down) {
        wiimote->previous_buttons_hid_mode = wiimote->buttons;
    } else if ((changed & 0x0002u) && (wiimote->buttons & 0x0002u)) {
        hid_set_output_mode(&hid_state, HID_MODE_POINTER);
        wiimote->previous_buttons_hid_mode = wiimote->buttons;
    } else if ((changed & 0x0001u) && (wiimote->buttons & 0x0001u)) {
        hid_set_output_mode(&hid_state, HID_MODE_DIGITIZER);
        wiimote->previous_buttons_hid_mode = wiimote->buttons;
    } else {
        wiimote->previous_buttons_hid_mode = wiimote->buttons;
    }

    uint16_t consumer_keycode = 0;
    bool home_down = (wiimote->buttons & WIIMOTE_HOME_MASK) != 0;
    if (profile_home_tap_press_pending) {
        consumer_keycode = 0x0223;
        profile_home_tap_press_pending = false;
        profile_home_tap_release_pending = true;
    } else if (profile_home_tap_release_pending) {
        consumer_keycode = 0;
        profile_home_tap_release_pending = false;
    } else if (home_down && (wiimote->buttons & 0x0004u)) {
        consumer_keycode = 0x00B5;
    } else if (wiimote->buttons & 0x0010u) {
        consumer_keycode = 0x0224;
    } else if (wiimote->buttons & 0x0002u) {
        consumer_keycode = 0x00E9;
    } else if (wiimote->buttons & 0x0001u) {
        consumer_keycode = 0x00EA;
    } else if (wiimote->buttons & 0x1000u) {
        consumer_keycode = 0x00E2;
    }
    hid_send_consumer_keycode(&hid_state, consumer_keycode);

    if (wiimote->have_norm) {
        profile_pointer_no_ir_frames = 0;
    } else if (profile_pointer_no_ir_frames < NO_IR_ENTER_THRESHOLD_FRAMES) {
        profile_pointer_no_ir_frames++;
    }

    bool a_maps_to_gamepad =
        (hid_state.output_mode == HID_MODE_POINTER) &&
        (profile_pointer_no_ir_frames >= NO_IR_ENTER_THRESHOLD_FRAMES);

    uint16_t gamepad_buttons = 0;
    if ((wiimote->buttons & 0x0008u) && a_maps_to_gamepad) {
        gamepad_buttons |= 0x0001u;
    }
    uint8_t hat = profile_buttons_to_hat(wiimote->buttons);
    hid_set_gamepad_hat(&hid_state, hat, gamepad_buttons);

    if (wiimote->have_norm) {
        if (!profile_pointer_prev_norm_valid) {
            profile_pointer_prev_norm_valid = true;
            profile_pointer_prev_norm_x = wiimote->norm_x;
            profile_pointer_prev_norm_y = wiimote->norm_y;
        } else {
            int32_t delta_x = (int32_t)wiimote->norm_x - (int32_t)profile_pointer_prev_norm_x;
            int32_t delta_y = (int32_t)wiimote->norm_y - (int32_t)profile_pointer_prev_norm_y;

            profile_pointer_prev_norm_x = wiimote->norm_x;
            profile_pointer_prev_norm_y = wiimote->norm_y;

            delta_x = -delta_x * POINTER_GAIN_X;
            delta_y = delta_y * POINTER_GAIN_Y;

            if (delta_x < -127) delta_x = -127;
            if (delta_x > 127) delta_x = 127;
            if (delta_y < -127) delta_y = -127;
            if (delta_y > 127) delta_y = 127;

            hid_send_pointer_delta(
                &hid_state,
                (int8_t)delta_x,
                (int8_t)delta_y,
                ((wiimote->buttons & 0x0008u) && !a_maps_to_gamepad) ? 0x01u : 0u);
        }
    } else {
        profile_pointer_prev_norm_valid = false;
        hid_send_pointer_delta(&hid_state, 0, 0, 0);
    }

    if (wiimote->buttons == 0) {
        hid_send_pointer_delta(&hid_state, 0, 0, 0);
    }
}

static void profile_wiimote_test2(wiimote_tracking_state_t *wiimote) {
    if (handle_profile_selection_hotkeys(wiimote)) {
        return;
    }
}

static void profile_wiimote_test3(wiimote_tracking_state_t *wiimote) {
    if (handle_profile_selection_hotkeys(wiimote)) {
        return;
    }
}

static void profile_wiimote_test4(wiimote_tracking_state_t *wiimote) {
    if (handle_profile_selection_hotkeys(wiimote)) {
        return;
    }
}

int main(void) {
    uint8_t persisted_profile_id = (uint8_t)APP_PROFILE_WIIMOTE_DEFAULT;

    stdio_init_all();
    sleep_ms(1500);

    printf("Pi Pico W 2 Wii Remote HID host proof-of-concept\n");
    printf("Using report mode + legacy PIN pairing (SSP disabled)\n");

    if (cyw43_arch_init() != 0) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    bootsel_init(BOOTSEL_POLL_PERIOD_MS, bootsel_poll_callback);
    hid_init(&hid_state);

    if (wiimote_load_persisted_profile_id(&persisted_profile_id) &&
        persisted_profile_id < (uint8_t)APP_PROFILE_COUNT) {
        active_profile_id = (app_profile_id_t)persisted_profile_id;
    }

    printf("Boot profile: %s\n", app_profiles[active_profile_id].name);
    wiimote_init_state(&wiimote_state, app_profiles[active_profile_id].handler);
    wiimote_set_sync_pair_callback(handle_sync_pair_complete);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
