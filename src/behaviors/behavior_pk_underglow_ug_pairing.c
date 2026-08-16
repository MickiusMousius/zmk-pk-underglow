/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_passkey_digits_changed.h>
#include <zmk/events/ble_passkey_state_changed.h>
#include <zmk/events/pk_underglow_color_changed.h>
#include <zmk/pk_underglow/public_api.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define COMPAT_DIGIT zmk_behavior_pk_underglow_ug_pairing_digit
#define COMPAT_ENTER zmk_behavior_pk_underglow_ug_pairing_enter

#if DT_HAS_COMPAT_STATUS_OKAY(COMPAT_DIGIT) || DT_HAS_COMPAT_STATUS_OKAY(COMPAT_ENTER)

static bool pairing_active = false;
static uint8_t passkey_len = 0;

static int underglow_ug_pairing_listener(const zmk_event_t *eh) {
    const struct ble_passkey_state_changed *ev = as_ble_passkey_state_changed(eh);
    pairing_active = ev->active;
    if (!pairing_active) {
        passkey_len = 0;
    }
    raise_zmk_pk_underglow_color_changed((struct zmk_pk_underglow_color_changed){.layers = 0xFFFFFFFF, .wakeup = true});
    return ZMK_EV_EVENT_BUBBLE;
}


ZMK_LISTENER(behavior_pk_underglow_ug_pairing, underglow_ug_pairing_listener);
ZMK_SUBSCRIPTION(behavior_pk_underglow_ug_pairing, ble_passkey_state_changed);

static int underglow_ug_passkey_digits_listener(const zmk_event_t *eh) {
    const struct ble_passkey_digits_changed *ev = as_ble_passkey_digits_changed(eh);
    passkey_len = ev->digits_len;
    raise_zmk_pk_underglow_color_changed((struct zmk_pk_underglow_color_changed){.layers = 0xFFFFFFFF, .wakeup = true});
    return ZMK_EV_EVENT_BUBBLE;
}


ZMK_LISTENER(behavior_pk_underglow_ug_passkey_digits, underglow_ug_passkey_digits_listener);
ZMK_SUBSCRIPTION(behavior_pk_underglow_ug_passkey_digits, ble_passkey_digits_changed);

#endif


#if DT_HAS_COMPAT_STATUS_OKAY(COMPAT_DIGIT)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT COMPAT_DIGIT

static int underglow_ug_pairing_digit_init(const struct device *dev) { return 0; };

static int underglow_ug_pairing_digit_process(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event) {
    if (!pairing_active || passkey_len >= 6) {
        return 0x000000;
    }

    uint32_t color = binding->param1;
    bool pulse = binding->param2;

    if (!pulse) {
        return color;
    }

    uint64_t uptime = event.timestamp;
    uint32_t phase = uptime % 2000;
    uint32_t val = (phase < 1000) ? phase : 2000 - phase;
    uint32_t scale = 50 + (val * 50 / 1000);

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    r = (r * scale) / 100;
    g = (g * scale) / 100;
    b = (b * scale) / 100;

    return (r << 16) | (g << 8) | b;
}


static const struct behavior_driver_api underglow_ug_pairing_digit_driver_api = {
    .binding_pressed = underglow_ug_pairing_digit_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};


#define KP_INST_DIGIT(n)                                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, underglow_ug_pairing_digit_init, NULL, NULL, NULL, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_ug_pairing_digit_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST_DIGIT)

#endif


#if DT_HAS_COMPAT_STATUS_OKAY(COMPAT_ENTER)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT COMPAT_ENTER

static int underglow_ug_pairing_enter_init(const struct device *dev) { return 0; };

static int underglow_ug_pairing_enter_process(struct zmk_behavior_binding *binding,
                                              struct zmk_behavior_binding_event event) {
    if (!pairing_active || passkey_len < 6) {
        return 0x000000;
    }

    uint32_t color = binding->param1;
    uint64_t uptime = event.timestamp;

    uint32_t phase = uptime % 400;
    if (phase < 200) {
        return color;
    } else {
        return 0x000000;
    }
}


static const struct behavior_driver_api underglow_ug_pairing_enter_driver_api = {
    .binding_pressed = underglow_ug_pairing_enter_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};


#define KP_INST_ENTER(n)                                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, underglow_ug_pairing_enter_init, NULL, NULL, NULL, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_ug_pairing_enter_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST_ENTER)

#endif
