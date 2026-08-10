/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pk_underglow_eff_bt

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pk_underglow.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif
#include <zmk/endpoints.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int underglow_eff_bt_init(const struct device *dev) { return 0; };

static int underglow_eff_bt_process(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    uint8_t profile = binding->param1;

#if (IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) || !IS_ENABLED(CONFIG_ZMK_BLE)
    // Peripheral half does not track BT profile status of central, fallback to solid blue
    struct zmk_led_hsb hsb = { .h = 240, .s = 100, .b = 100 };
    return zmk_pk_underglow_hsb_to_hex(hsb);
#else
    uint64_t uptime = event.timestamp;
    
    bool is_selected = (zmk_ble_active_profile_index() == profile);
    bool is_connected = zmk_ble_profile_is_connected(profile);
    bool is_open = zmk_ble_profile_is_open(profile);
    bool usb_active = (zmk_endpoints_selected().transport == ZMK_TRANSPORT_USB) && zmk_usb_is_powered();

    if (!is_selected || usb_active) {
        // Not selected (or USB is active & connected): Solid blue
        struct zmk_led_hsb hsb = { .h = 240, .s = 100, .b = 100 };
        return zmk_pk_underglow_hsb_to_hex(hsb);
    }

    if (is_connected) {
        // Selected & connected: Gently pulse blue (1000ms cycle)
        uint32_t phase = uptime % 1000;
        uint32_t val = (phase < 500) ? phase : 1000 - phase; 
        uint8_t b = 10 + (val * 90 / 500); // Pulse between 10% and 100% brightness
        struct zmk_led_hsb hsb = { .h = 240, .s = 100, .b = b };
        return zmk_pk_underglow_hsb_to_hex(hsb);
    }

    if (is_open) {
        // Selected & advertising: Rapidly blink off and on as blue (200ms cycle)
        uint32_t phase = uptime % 200;
        uint8_t b = (phase < 100) ? 100 : 0;
        struct zmk_led_hsb hsb = { .h = 240, .s = 100, .b = b };
        return zmk_pk_underglow_hsb_to_hex(hsb);
    }

    // Selected & not connected (but bonded): Blink blue then red repeatedly (500ms cycle)
    uint32_t phase = uptime % 500;
    uint16_t h = (phase < 250) ? 240 : 0; // 240 = Blue, 0 = Red
    struct zmk_led_hsb hsb = { .h = h, .s = 100, .b = 100 };
    return zmk_pk_underglow_hsb_to_hex(hsb);
#endif
}

static const struct behavior_driver_api underglow_eff_bt_driver_api = {
    .binding_pressed = underglow_eff_bt_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, underglow_eff_bt_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_eff_bt_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
