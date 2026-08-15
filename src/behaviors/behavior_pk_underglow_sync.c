/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pk_underglow_sync

// Dependencies
#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zmk/pk_underglow/public_api.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// Initialization Function
static int underglow_sync_init(const struct device *dev) { return 0; };

static int underglow_sync_process(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zmk_pk_underglow_sync_state(binding->param1, binding->param2);
#endif
    return 0;
}


static int underglow_sync_released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    return 0;
}


// API Structure
static const struct behavior_driver_api underglow_sync_driver_api = {
    .binding_pressed = underglow_sync_process,
    .binding_released = underglow_sync_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

};


BEHAVIOR_DT_INST_DEFINE(0, underglow_sync_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &underglow_sync_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
