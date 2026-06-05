#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "btstack.h"
#include "wiimote.h"
#include "hid.h"

// Global state objects
static wiimote_tracking_state_t wiimote_state = {0};
static hid_state_t hid_state = {0};

// Behavior profile: Default Wii Remote to HID mapping
#define POINTER_GAIN_X 3.0f
#define POINTER_GAIN_Y 1.25f
#define NO_IR_ENTER_THRESHOLD_FRAMES 5

static bool profile_pointer_prev_norm_valid;
static uint16_t profile_pointer_prev_norm_x;
static uint16_t profile_pointer_prev_norm_y;
static uint8_t profile_pointer_no_ir_frames;

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

void profile_wiimote_default(wiimote_tracking_state_t *wiimote) {
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
    bool home_down = (wiimote->buttons & 0x0080u) != 0;
    if (home_down && (wiimote->buttons & 0x0004u)) {
        consumer_keycode = 0x00B5;
    } else if (home_down) {
        consumer_keycode = 0x0223;
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

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    printf("Pi Pico W 2 Wii Remote HID host proof-of-concept\n");
    printf("Using report mode + legacy PIN pairing (SSP disabled)\n");

    if (cyw43_arch_init() != 0) {
        printf("cyw43_arch_init failed\n");
        return 1;
    }

    // Initialize HID module (pure output layer)
    hid_init(&hid_state);

    // Initialize Wii Remote module (Bluetooth + parsing)
    wiimote_init_state(&wiimote_state, profile_wiimote_default);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
