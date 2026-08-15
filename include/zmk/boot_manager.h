/**
 * @file pk_underglow_stabilization.h
 * @brief Public interface for the pk_underglow stabilization tracker.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Evaluates whether ZMK has finished its boot cycle and stabilized.
 *
 * "Stabilized" means that all initial post-boot configuration sweeps (like NVS loads,
 * BLE initial connection syncs) have completed. Checking this flag prevents race
 * conditions where underglow attempts to access properties that don't exist yet or
 * overwrites them prematurely.
 *
 * @return true if the system has stabilized, false if still booting.
 */
bool zmk_pk_underglow_is_stabilized(void);

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
/**
 * @brief Signal that the peripheral has received its first synchronization packet.
 *
 * Called by the sync handler to notify the stabilization tracker that the central
 * has pushed its initial state.
 */
void zmk_pk_underglow_signal_peripheral_sync(void);
#endif
#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
/**
 * @brief Signal that the central has finished loading NVS settings.
 *
 * Called by the settings subsystem commit callback to notify the stabilization tracker.
 */
void zmk_pk_underglow_signal_central_nvs_loaded(void);
#endif

/**
 * @brief Sets the persistent boot power state and queues an NVS write.
 *
 * @param power_on true to boot with underglow on, false to boot with it off.
 */
void zmk_pk_underglow_set_boot_power(bool power_on);
