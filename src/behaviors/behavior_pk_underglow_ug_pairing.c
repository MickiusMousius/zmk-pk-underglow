/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pk_underglow_ug_pairing

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_passkey_state_changed.h>
#include <zmk/events/underglow_color_changed.h>
#include <zmk/pk_underglow.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static bool pairing_active = false;

static int underglow_ug_pairing_listener(const zmk_event_t *eh) {
    const struct ble_passkey_state_changed *ev = as_ble_passkey_state_changed(eh);
    pairing_active = ev->active;

    // Force redraw of underglow to apply new state
    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){.layers = 0xFFFFFFFF, .wakeup = true});

    return ZMK_EV_EVENT_BUBBLE;
}


ZMK_LISTENER(behavior_pk_underglow_ug_pairing, underglow_ug_pairing_listener);
ZMK_SUBSCRIPTION(behavior_pk_underglow_ug_pairing, ble_passkey_state_changed);


static int underglow_ug_pairing_init(const struct device *dev) { return 0; };

static int underglow_ug_pairing_process(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    if (!pairing_active) {
        return 0x000000; // BLACK when not in pairing mode
    }

    uint32_t color = binding->param1;
    bool pulse = binding->param2;

    if (!pulse) {
        return color; // Return the exact static color if pulsing is disabled
    }

    uint64_t uptime = event.timestamp;

    // Gently pulse from 50% to 100% brightness (2000ms cycle: 1000ms up, 1000ms down)
    uint32_t phase = uptime % 2000;
    uint32_t val = (phase < 1000) ? phase : 2000 - phase;

    // Calculate brightness scale (50 to 100 percent)
    uint32_t scale = 50 + (val * 50 / 1000);

    // Unpack RGB
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Scale RGB uniformly to adjust brightness linearly
    r = (r * scale) / 100;
    g = (g * scale) / 100;
    b = (b * scale) / 100;

    // Pack RGB back to hex
    return (r << 16) | (g << 8) | b;
}


static const struct behavior_driver_api underglow_ug_pairing_driver_api = {
    .binding_pressed = underglow_ug_pairing_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};


#define KP_INST(n)                                                                                                     \
    BEHAVIOR_DT_INST_DEFINE(n, underglow_ug_pairing_init, NULL, NULL, NULL, POST_KERNEL,                               \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_ug_pairing_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
