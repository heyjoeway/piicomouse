#ifndef PIICOMOUSE2_HID_H
#define PIICOMOUSE2_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"
#include "timer.h"

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

typedef struct {
    bool connected;
    hid_output_mode_t output_mode;
    bool pointer_had_tracking;
    uint16_t pointer_prev_norm_x;
    uint16_t pointer_prev_norm_y;
    bool pointer_motion_locked;
    bool pointer_prev_norm_valid;
    uint16_t prev_consumer_keycode;
    bool home_b_combo_latched;
    uint8_t prev_gamepad_hat;
    uint16_t prev_gamepad_buttons;
    uint8_t pointer_no_ir_frames;
    bool onboard_led_on;
    hid_sleep_signal_stage_t sleep_signal_stage;
    hid_wake_nudge_stage_t wake_nudge_stage;
    timer_slot_t report_timer_slot;
    timer_manager_t report_timer_manager;
} hid_state_t;

void hid_init(hid_state_t *state);
hid_state_t *hid_get_state(void);
void hid_set_connected(hid_state_t *state, bool connected);
bool hid_is_connected(const hid_state_t *state);
void hid_reset_output_state(hid_state_t *state);
void hid_request_usb_remote_wake_on_reconnect(void);
void hid_queue_sleep_signal(hid_state_t *state);
void hid_queue_wake_nudge(hid_state_t *state);
void hid_set_output_mode(hid_state_t *state, hid_output_mode_t mode);
void hid_send_pointer_delta(hid_state_t *state, int8_t dx, int8_t dy, uint8_t buttons);
void hid_set_gamepad_hat(hid_state_t *state, uint8_t hat, uint16_t buttons);
void hid_send_consumer_keycode(hid_state_t *state, uint16_t keycode);

#endif