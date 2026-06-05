#ifndef PIICOMOUSE2_TIMER_H
#define PIICOMOUSE2_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include "btstack.h"

typedef struct {
    bool active;
    uint32_t due_ms;
    void (*callback)(void);
} timer_slot_t;

typedef struct {
    btstack_timer_source_t real_timer;
    timer_slot_t *slots;
    uint32_t slot_count;
    bool armed;
} timer_manager_t;

void timer_manager_init(timer_manager_t *tm, timer_slot_t *slots, uint32_t slot_count);
void timer_manager_start(timer_manager_t *tm, uint32_t timer_id, uint32_t delay_ms);
void timer_manager_stop(timer_manager_t *tm, uint32_t timer_id);
void timer_manager_update_schedule(timer_manager_t *tm);

#endif
