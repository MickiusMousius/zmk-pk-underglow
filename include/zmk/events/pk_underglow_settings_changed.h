#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

struct pk_underglow_settings_changed {
    int64_t timestamp;
};


ZMK_EVENT_DECLARE(pk_underglow_settings_changed);

static inline int raise_pk_underglow_settings_changed_event(struct pk_underglow_settings_changed event) {
    return raise_pk_underglow_settings_changed(event);
}
