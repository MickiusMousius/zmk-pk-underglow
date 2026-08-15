#pragma once

#include <zmk/event_manager.h>

struct zmk_pk_underglow_color_changed {
    uint32_t layers;
    bool wakeup;
};


ZMK_EVENT_DECLARE(zmk_pk_underglow_color_changed);
