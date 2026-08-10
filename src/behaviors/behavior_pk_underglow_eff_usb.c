/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pk_underglow_eff_usb

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pk_underglow.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int underglow_eff_usb_init(const struct device *dev) { return 0; };

static int underglow_eff_usb_process(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
#if (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    // Peripheral half does not track central USB state, fallback to solid
    struct zmk_led_hsb hsb = { .h = 173, .s = 89, .b = 100 };
    return zmk_pk_underglow_hsb_to_hex(hsb);
#else
    uint64_t uptime = event.timestamp;
    bool is_selected = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB);
    bool is_connected = zmk_usb_is_powered();

    if (!is_connected) {
        // No USB connection: BLACK
        struct zmk_led_hsb hsb = { .h = 0, .s = 0, .b = 0 };
        return zmk_pk_underglow_hsb_to_hex(hsb);
    }

    if (!is_selected) {
        // Connected but not active interface: Solid #14B8A6
        struct zmk_led_hsb hsb = { .h = 173, .s = 89, .b = 100 };
        return zmk_pk_underglow_hsb_to_hex(hsb);
    }

    // Connected & active interface: Gently pulse #14B8A6 (1000ms cycle)
    uint32_t phase = uptime % 1000;
    uint32_t val = (phase < 500) ? phase : 1000 - phase; 
    uint8_t b = 10 + (val * 90 / 500); // Pulse between 10% and 100% brightness
    struct zmk_led_hsb hsb = { .h = 173, .s = 89, .b = b };
    return zmk_pk_underglow_hsb_to_hex(hsb);
#endif
}

static const struct behavior_driver_api underglow_eff_usb_driver_api = {
    .binding_pressed = underglow_eff_usb_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, underglow_eff_usb_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_eff_usb_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
