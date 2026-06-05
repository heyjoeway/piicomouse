#include "bootsel.h"

#include "pico/stdlib.h"

#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"

#include "btstack.h"
#include "timer.h"

static timer_slot_t bootsel_timer_slot;
static timer_manager_t bootsel_timer_manager;
static bootsel_poll_callback_t bootsel_callback = NULL;
static uint32_t bootsel_poll_period_ms;
static bool bootsel_prev_pressed;

static bool __no_inline_not_in_flash_func(read_bootsel_button_pressed)(void) {
    const uint cs_pin_index = 1;
    uint32_t flags = save_and_disable_interrupts();

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

    return !cs_high;
}

static void bootsel_timer_callback(void) {
    bool pressed = read_bootsel_button_pressed();
    bool changed = pressed != bootsel_prev_pressed;
    if (changed) {
        bootsel_prev_pressed = pressed;
    }

    if (bootsel_callback != NULL) {
        bootsel_callback(pressed, changed);
    }

    timer_manager_start(&bootsel_timer_manager, 0, bootsel_poll_period_ms);
}

void bootsel_init(uint32_t poll_period_ms, bootsel_poll_callback_t callback) {
    bootsel_poll_period_ms = poll_period_ms;
    bootsel_callback = callback;
    bootsel_prev_pressed = false;

    bootsel_timer_slot.callback = bootsel_timer_callback;
    timer_manager_init(&bootsel_timer_manager, &bootsel_timer_slot, 1);
    timer_manager_start(&bootsel_timer_manager, 0, poll_period_ms);
}