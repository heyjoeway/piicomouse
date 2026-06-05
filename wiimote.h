#ifndef WIIMOTE_H
#define WIIMOTE_H

#include <stdbool.h>
#include <stdint.h>
#include "hid.h"

#define WIIMOTE_IR_POINTS 4

typedef struct {
    bool valid;
    uint16_t x;
    uint16_t y;
    uint8_t size;
} wiimote_ir_point_t;

typedef struct {
    bool have_buttons;
    uint16_t buttons;
    bool status_flags_valid;
    uint8_t status_flags;
    bool have_ir;
    bool have_center;
    uint16_t center_x;
    uint16_t center_y;
    bool have_norm;
    uint16_t norm_x;
    uint16_t norm_y;
    wiimote_ir_point_t points[WIIMOTE_IR_POINTS];
    bool seen_report_id[256];
    bool ir_init_in_progress;
    uint8_t ir_init_step;
    uint8_t ir_debug_frames_remaining;
    bool ir_report_mode_active;
    uint8_t ir_mode_index;
    uint8_t ir_mode_retry_count;
    uint8_t ir_sensitivity_index;
    int16_t ir_last_half_dx;
    int16_t ir_last_half_dy;
    uint16_t ir_last_center_x;
    uint16_t ir_last_center_y;
    uint8_t ir_dropout_frames;
    bool ir_have_spread;
    uint16_t previous_buttons;
    uint16_t previous_buttons_hid_mode;
    uint8_t target_addr[6];
    bool target_addr_configured;
    bool wii_pin_use_reversed;
} wiimote_tracking_state_t;

// Behavior profile: translates Wii Remote input to HID commands
typedef void (*wiimote_behavior_profile_t)(wiimote_tracking_state_t *wiimote);

void wiimote_init_state(wiimote_tracking_state_t *wiimote_state, wiimote_behavior_profile_t profile);

#endif
