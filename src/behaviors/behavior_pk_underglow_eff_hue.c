/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pk_underglow_eff_hue

// Dependencies
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/pk_underglow.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// Initialization Function
static int underglow_eff_hue_init(const struct device *dev) { return 0; };

static int underglow_eff_hue_process(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    // 1. Fetch current color (potentially animated) from the module
    struct zmk_led_hsb hsb = zmk_pk_underglow_get_eff_color();
    
    // 2. Add offset and wrap around 360 (HUE_MAX)
    int offset = binding->param1;
    hsb.h = (hsb.h + offset) % HUE_MAX;
    
    // 3. Force brightness to 100% so that `hex_to_rgb` correctly scales it by global brightness
    hsb.b = BRT_MAX;
    
    // 4. Return as a 24-bit RGB hex value
    return zmk_pk_underglow_hsb_to_hex(hsb);
}

// API Structure
static const struct behavior_driver_api underglow_eff_hue_driver_api = {
    .binding_pressed = underglow_eff_hue_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

};

BEHAVIOR_DT_INST_DEFINE(0, underglow_eff_hue_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &underglow_eff_hue_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
