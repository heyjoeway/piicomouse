#include "timer.h"

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static bool timer_time_reached(uint32_t now_ms, uint32_t due_ms) {
    return (int32_t)(now_ms - due_ms) >= 0;
}

static void timer_manager_internal_handler(btstack_timer_source_t *ts) {
    timer_manager_t *tm = container_of(ts, timer_manager_t, real_timer);

    if (tm->armed) {
        tm->armed = false;
    }

    for (;;) {
        uint32_t now_ms = btstack_run_loop_get_time_ms();
        bool handled_due_timer = false;

        for (uint32_t i = 0; i < tm->slot_count; i++) {
            if (!tm->slots[i].active || !timer_time_reached(now_ms, tm->slots[i].due_ms)) {
                continue;
            }

            tm->slots[i].active = false;
            if (tm->slots[i].callback != NULL) {
                tm->slots[i].callback();
            }
            handled_due_timer = true;
            break;
        }

        if (!handled_due_timer) {
            break;
        }
    }

    timer_manager_update_schedule(tm);
}

void timer_manager_init(timer_manager_t *tm, timer_slot_t *slots, uint32_t slot_count) {
    tm->slots = slots;
    tm->slot_count = slot_count;
    tm->armed = false;

    for (uint32_t i = 0; i < slot_count; i++) {
        tm->slots[i].active = false;
        tm->slots[i].due_ms = 0;
    }

    btstack_run_loop_set_timer_handler(&tm->real_timer, timer_manager_internal_handler);
}

void timer_manager_start(timer_manager_t *tm, uint32_t timer_id, uint32_t delay_ms) {
    if (timer_id >= tm->slot_count) {
        return;
    }

    tm->slots[timer_id].active = true;
    tm->slots[timer_id].due_ms = btstack_run_loop_get_time_ms() + delay_ms;
    timer_manager_update_schedule(tm);
}

void timer_manager_stop(timer_manager_t *tm, uint32_t timer_id) {
    if (timer_id >= tm->slot_count) {
        return;
    }

    tm->slots[timer_id].active = false;
    timer_manager_update_schedule(tm);
}

void timer_manager_update_schedule(timer_manager_t *tm) {
    uint32_t now_ms = btstack_run_loop_get_time_ms();
    bool have_due_time = false;
    uint32_t next_due_ms = 0;

    if (tm->armed) {
        btstack_run_loop_remove_timer(&tm->real_timer);
        tm->armed = false;
    }

    for (uint32_t i = 0; i < tm->slot_count; i++) {
        if (!tm->slots[i].active) {
            continue;
        }
        if (!have_due_time || timer_time_reached(next_due_ms, tm->slots[i].due_ms)) {
            next_due_ms = tm->slots[i].due_ms;
            have_due_time = true;
        }
    }

    if (!have_due_time) {
        return;
    }

    uint32_t delay_ms = timer_time_reached(now_ms, next_due_ms) ? 0 : (next_due_ms - now_ms);
    btstack_run_loop_set_timer(&tm->real_timer, delay_ms);
    btstack_run_loop_add_timer(&tm->real_timer);
    tm->armed = true;
}
