#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct pk_underglow_power_changed {
    bool is_powered;
};


ZMK_EVENT_DECLARE(pk_underglow_power_changed);

static inline int raise_pk_underglow_power_changed_event(struct pk_underglow_power_changed event) {
    return raise_pk_underglow_power_changed(event);
}
