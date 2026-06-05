#ifndef PIICOMOUSE2_BOOTSEL_H
#define PIICOMOUSE2_BOOTSEL_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*bootsel_poll_callback_t)(bool pressed, bool changed);

void bootsel_init(uint32_t poll_period_ms, bootsel_poll_callback_t callback);

#endif