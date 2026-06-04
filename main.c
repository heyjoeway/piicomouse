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
void profile_wiimote_default(const wiimote_tracking_state_t *wiimote, hid_state_t *hid) {
    // Placeholder for now - will be populated with full mapping logic
    (void)wiimote;
    (void)hid;
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
    wiimote_init_state(&wiimote_state, &hid_state);

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();

    cyw43_arch_deinit();
    return 0;
}
