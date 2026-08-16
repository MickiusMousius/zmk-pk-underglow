/**
 * @file boot_manager.c
 * @brief Implementation of the ZMK boot stabilization tracker for pk_underglow.
 *
 * Determining if ZMK has "finished booting and stabilized" is non-trivial because ZMK
 * does not have a singular BOOT_COMPLETE event. Initialization happens asynchronously
 * across multiple subsystems (Settings NVS, Bluetooth, Matrix).
 *
 * This module deduces stabilization through an event-driven fallback structure:
 * 1. Uptime Fallback Timer: A delayed work queue item initialized during SYS_INIT. If
 *    the timeout (CONFIG_ZMK_PK_UNDERGLOW_STABILIZATION_TIMEOUT) is reached, we
 *    forcefully assume stabilization. This ensures we never deadlock if settings are disabled.
 * 2. Settings Load (Central): We hook into ZMK's settings subsystem commit callback in state_manager.c.
 *    Zephyr guarantees this is called once after sweeping flash.
 * 3. Sync Packets (Peripheral): The peripheral doesn't load settings, it waits for the
 *    central. It is considered stable once the central pushes its first visual state payload.
 */

#include "pk_underglow_internal.h"
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zmk/pk_underglow/boot_manager.h>
#include <zmk/pk_underglow/public_api.h>

LOG_MODULE_DECLARE(zmk_pk_underglow, CONFIG_ZMK_PK_UNDERGLOW_LOG_LEVEL);

static bool is_stabilized = false;
static bool boot_power_on = false; // Loaded from NVS, defaults to false

bool zmk_pk_underglow_is_stabilized(void) { return is_stabilized; }


#if defined(HAS_NVS_SETTINGS)
static void evaluate_boot_power_state(void) {
    LOG_INF("Evaluating boot power state: %s", boot_power_on ? "ON" : "OFF");
    if (boot_power_on) {
        zmk_pk_underglow_on();
    } else {
        zmk_pk_underglow_off();
    }
}


#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void zmk_pk_underglow_signal_peripheral_sync(void) {
    if (!is_stabilized) {
        LOG_DBG("Peripheral stabilized via first BT sync.");
        is_stabilized = true;
    }
}


#endif

#if defined(HAS_NVS_SETTINGS)
void zmk_pk_underglow_signal_central_nvs_loaded(void) {
    if (!is_stabilized) {
        LOG_DBG("Central stabilized via NVS load completion.");
        is_stabilized = true;
        evaluate_boot_power_state();
    }
}


#endif

static void stabilization_fallback_work_handler(struct k_work *work) {
    if (!is_stabilized) {
        LOG_DBG("System stabilized via uptime fallback timer (%d ms).", CONFIG_ZMK_PK_UNDERGLOW_STABILIZATION_TIMEOUT);
        is_stabilized = true;
#if defined(HAS_NVS_SETTINGS)
        evaluate_boot_power_state();
#endif
    }
}


K_WORK_DELAYABLE_DEFINE(stabilization_fallback_work, stabilization_fallback_work_handler);

// Initialization hook to start the fallback timer at boot
static int stabilization_init(void) {
    // Schedule the fallback timer
    k_work_schedule(&stabilization_fallback_work, K_MSEC(CONFIG_ZMK_PK_UNDERGLOW_STABILIZATION_TIMEOUT));
    return 0;
}


// Run at POST_KERNEL priority so it initializes before application-level components
SYS_INIT(stabilization_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_SETTINGS)
static int boot_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "power", &next) && !next) {
        if (len != sizeof(boot_power_on)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &boot_power_on, sizeof(boot_power_on));
    }
    return -ENOENT;
}


SETTINGS_STATIC_HANDLER_DEFINE(pk_ug_boot, "pk_ug_boot", NULL, boot_settings_set, NULL, NULL);

static void zmk_pk_underglow_save_boot_power_work(struct k_work *_work) {
    settings_save_one("pk_ug_boot/power", &boot_power_on, sizeof(boot_power_on));
}


K_WORK_DELAYABLE_DEFINE(boot_power_save_work, zmk_pk_underglow_save_boot_power_work);
#endif

void zmk_pk_underglow_set_boot_power(bool power_on) {
    if (boot_power_on != power_on) {
        boot_power_on = power_on;
#if IS_ENABLED(CONFIG_SETTINGS)
        k_work_schedule(&boot_power_save_work, K_NO_WAIT);
#endif
    }
}
